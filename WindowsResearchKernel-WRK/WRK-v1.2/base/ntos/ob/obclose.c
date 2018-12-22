/*
Copyright (c) Microsoft Corporation. All rights reserved.

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.

Module Name:
    obclose.c

Abstract:
    Object close system service
*/

#include "obp.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE,NtMakeTemporaryObject)
#pragma alloc_text(PAGE,NtClose)
#pragma alloc_text(PAGE,ObMakeTemporaryObject)
#pragma alloc_text(PAGE,ObpCloseHandleTableEntry)
#pragma alloc_text(PAGE,ObCloseHandle)
#pragma alloc_text(PAGE,ObpCloseHandle)
#endif

extern BOOLEAN SepAdtAuditingEnabled;//  Indicates if auditing is enabled so we have to close down the object audit alarm


NTSTATUS ObpCloseHandleTableEntry(
    IN PHANDLE_TABLE ObjectTable,
    IN PHANDLE_TABLE_ENTRY ObjectTableEntry,
    IN HANDLE Handle,
    IN KPROCESSOR_MODE PreviousMode,
    IN BOOLEAN Rundown)
    /*
    Routine Description:
        This function is used to close a handle table entry
    Arguments:
        ObjectTableEntry - Supplies the entry being closed. It must be locked
        PreviousMode     - Mode of caller
        Rundown          - Called as part of process rundown, ignore protected handles in this mode
    */
{
    POBJECT_HEADER ObjectHeader;
    POBJECT_TYPE ObjectType;
    PVOID Object;
    ULONG CapturedGrantedAccess;
    ULONG CapturedAttributes;
#if DBG
    KIRQL SaveIrql;
#endif // DBG

    //  From the object table entry we can grab a pointer to the object header, get its type and its body
    ObjectHeader = (POBJECT_HEADER)(((ULONG_PTR)(ObjectTableEntry->Object)) & ~OBJ_HANDLE_ATTRIBUTES);
    ObjectType = ObjectHeader->Type;
    Object = &ObjectHeader->Body;

    //  If the object type specifies an okay to close procedure then we need to invoke that callback.
    //  If the callback doesn't want us to close handle then unlock the object table and return the error to our caller
    if (ObjectType->TypeInfo.OkayToCloseProcedure != NULL) {
        ObpBeginTypeSpecificCallOut(SaveIrql);
        if (!(*ObjectType->TypeInfo.OkayToCloseProcedure)(PsGetCurrentProcess(), Object, Handle, PreviousMode)) {
            ObpEndTypeSpecificCallOut(SaveIrql, "NtClose", ObjectType, Object);
            ExUnlockHandleTableEntry(ObjectTable, ObjectTableEntry);
            return STATUS_HANDLE_NOT_CLOSABLE;
        }

        ObpEndTypeSpecificCallOut(SaveIrql, "NtClose", ObjectType, Object);
    }

    CapturedAttributes = ObpGetHandleAttributes(ObjectTableEntry);

    //  If the previous mode was user and the handle is protected from being closed, 
    //  then we'll either raise or return an error depending on the global flags and debugger port situation.
    if ((CapturedAttributes & OBJ_PROTECT_CLOSE) != 0 && Rundown == FALSE) {
        if (PreviousMode != KernelMode) {
            ExUnlockHandleTableEntry(ObjectTable, ObjectTableEntry);
            if (!KeIsAttachedProcess() && ((NtGlobalFlag & FLG_ENABLE_CLOSE_EXCEPTIONS) || (PsGetCurrentProcess()->DebugPort != NULL) || (ObjectTable->DebugInfo != NULL))) {
                return KeRaiseUserException(STATUS_HANDLE_NOT_CLOSABLE);//  Raise and exception in user mode
            } else {
                return STATUS_HANDLE_NOT_CLOSABLE;
            }
        } else {
            KeBugCheckEx(INVALID_KERNEL_HANDLE, (ULONG_PTR)Handle, 0, 0, 0);
        }
    }

    //  Get the granted access for the handle
#if i386
    if (NtGlobalFlag & FLG_KERNEL_STACK_TRACE_DB) {
        CapturedGrantedAccess = ObpTranslateGrantedAccessIndex(ObjectTableEntry->GrantedAccessIndex);
    } else {
        CapturedGrantedAccess = ObpDecodeGrantedAccess(ObjectTableEntry->GrantedAccess);
    }
#else
    CapturedGrantedAccess = ObpDecodeGrantedAccess(ObjectTableEntry->GrantedAccess);
#endif // i386

    ExDestroyHandle(ObjectTable, Handle, ObjectTableEntry);//  Now remove the handle from the handle table

    //  perform any auditing required

    //  Extract the value of the GenerateOnClose bit stored after object open auditing is performed.
    //  This value was stored by a call to ObSetGenerateOnClosed.
    if (CapturedAttributes & OBJ_AUDIT_OBJECT_CLOSE) {
        if (SepAdtAuditingEnabled) {
            SeCloseObjectAuditAlarm(Object, (HANDLE)((ULONG_PTR)Handle & ~OBJ_HANDLE_TAGBITS),  // Mask off the tagbits defined for OB objects.
                                    TRUE);
        }
    }

    //  Since we took the handle away we need to decrement the objects handle count, and remove a reference
    ObpDecrementHandleCount(PsGetCurrentProcess(), ObjectHeader, ObjectType, CapturedGrantedAccess);
    ObDereferenceObject(Object);
    return STATUS_SUCCESS;//  And return to our caller
}


NTSTATUS ObpCloseHandle(IN HANDLE Handle, IN KPROCESSOR_MODE PreviousMode)
/*
Routine Description:
    This function is used to close access to the specified handle with the given mode
Arguments:
    Handle - Supplies the handle being closed
    PreviousMode - Processor mode to be used in the handle access checks.
    CanNotRaise - We are not allowed to do a user mode raise.
Return Value:
    An appropriate status value
*/
{
    PHANDLE_TABLE ObjectTable;
    PHANDLE_TABLE_ENTRY ObjectTableEntry;
    NTSTATUS Status;
    BOOLEAN AttachedToProcess = FALSE;
    KAPC_STATE ApcState;
    PETHREAD CurrentThread;
    PEPROCESS CurrentProcess;

    ObpValidateIrql("NtClose");

    CurrentThread = PsGetCurrentThread();
    CurrentProcess = PsGetCurrentProcessByThread(CurrentThread);

    //  For the current process we will grab its handle/object table and translate the handle to its corresponding table entry.
    //  If the call is successful it also lock down the handle table.
    //  But first check for a kernel handle and attach and use that table if so.
    if (IsKernelHandle(Handle, PreviousMode)) {
        Handle = DecodeKernelHandle(Handle);
        ObjectTable = ObpKernelHandleTable;
        if (CurrentProcess != PsInitialSystemProcess) {//  Go to the system process if we have to
            KeStackAttachProcess(&PsInitialSystemProcess->Pcb, &ApcState);
            AttachedToProcess = TRUE;
        }
    } else {
        ObjectTable = CurrentProcess->ObjectTable;
    }

    KeEnterCriticalRegionThread(&CurrentThread->Tcb);//  Protect ourselves from being interrupted while we hold a handle table entry lock

    ObjectTableEntry = ExMapHandleToPointer(ObjectTable, Handle);
    if (ObjectTableEntry != NULL) {//  Check that the specified handle is legitimate otherwise we can assume the caller just passed in some bogus handle value
        Status = ObpCloseHandleTableEntry(ObjectTable, ObjectTableEntry, Handle, PreviousMode, FALSE);
        KeLeaveCriticalRegionThread(&CurrentThread->Tcb);
        if (AttachedToProcess) {//  If we are attached to the system process then detach
            KeUnstackDetachProcess(&ApcState);
            AttachedToProcess = FALSE;
        }
    } else {
        KeLeaveCriticalRegionThread(&CurrentThread->Tcb);
        //  At this point the input handle did not translate to a valid object table entry
        if (AttachedToProcess) {//  If we are attached to the system process then return back to our caller
            KeUnstackDetachProcess(&ApcState);
            AttachedToProcess = FALSE;
        }

        //  Now if the handle is not null and it does not represent the current thread or process then if we're user mode we either raise or return an error
        if ((Handle != NULL) && (Handle != NtCurrentThread()) && (Handle != NtCurrentProcess())) {
            if (PreviousMode != KernelMode) {
                if ((NtGlobalFlag & FLG_ENABLE_CLOSE_EXCEPTIONS) || (CurrentProcess->DebugPort != NULL) || (ObjectTable->DebugInfo != NULL)) {
                    if (!KeIsAttachedProcess()) {
                        return KeRaiseUserException(STATUS_INVALID_HANDLE);
                    } else {
                        return STATUS_INVALID_HANDLE;
                    }
                }
            } else {
                //  bugcheck here if kernel debugger is enabled and if kernel mode code is closing a bogus handle and process is not exiting.
                //  Ignore if no PEB as this occurs if process is killed before really starting.
                if ((!PsIsThreadTerminating(CurrentThread)) && (CurrentProcess->Peb != NULL)) {
                    if (KdDebuggerEnabled) {
                        KeBugCheckEx(INVALID_KERNEL_HANDLE, (ULONG_PTR)Handle, 1, 0, 0);
                    }
                }
            }
        }

        Status = STATUS_INVALID_HANDLE;
    }

    return Status;
}


NTSTATUS ObCloseHandle(__in HANDLE Handle, __in KPROCESSOR_MODE PreviousMode)
/*
Routine Description:
    This function is used to close access to the specified handle with the given mode
Arguments:
    Handle - Supplies the handle being closed
    PreviousMode - Processor mode to be used in the handle access checks.
Return Value:
    An appropriate status value
*/
{
    return ObpCloseHandle(Handle, PreviousMode);
}


NTSTATUS NtClose(__in HANDLE Handle)
/*
Routine Description:
    This function is used to close access to the specified handle
Arguments:
    Handle - Supplies the handle being closed
Return Value:
    An appropriate status value
*/
{
    return ObpCloseHandle(Handle, KeGetPreviousMode());
}


NTSTATUS NtMakeTemporaryObject(__in HANDLE Handle)
/*
Routine Description:
    This routine makes the specified object non permanent.
Arguments:
    Handle - Supplies a handle to the object being modified
Return Value:
    An appropriate status value.
*/
{
    KPROCESSOR_MODE PreviousMode;
    NTSTATUS Status;
    PVOID Object;
    OBJECT_HANDLE_INFORMATION HandleInformation;

    PAGED_CODE();

    //  Get previous processor mode and probe output argument if necessary.
    PreviousMode = KeGetPreviousMode();
    Status = ObReferenceObjectByHandle(Handle, DELETE, (POBJECT_TYPE)NULL, PreviousMode, &Object, &HandleInformation);
    if (!NT_SUCCESS(Status)) {
        return(Status);
    }

    //  Make the object temporary.  Note that the object should still have a name and directory entry because its handle count is not zero
    ObMakeTemporaryObject(Object);

    if (HandleInformation.HandleAttributes & OBJ_AUDIT_OBJECT_CLOSE) {//  Check if we need to generate a delete object audit/alarm
        SeDeleteObjectAuditAlarm(Object, Handle);
    }

    ObDereferenceObject(Object);
    return(Status);
}


VOID ObMakeTemporaryObject(__in PVOID Object)
/*
Routine Description:
    This routine removes the name of the object from its parent directory.
    The object is only removed if it has a non zero handle count and a name.
    Otherwise the object is simply made non permanent
Arguments:
    Object - Supplies the object being modified
*/
{
    POBJECT_HEADER ObjectHeader;
    POBJECT_TYPE ObjectType;

    PAGED_CODE();

    ObjectHeader = OBJECT_TO_OBJECT_HEADER(Object);
    ObjectType = ObjectHeader->Type;

    // Other bits are set in this flags field by the handle database code. Synchronize with that.
    ObpLockObject(ObjectHeader);
    ObjectHeader->Flags &= ~OB_FLAG_PERMANENT_OBJECT;
    ObpUnlockObject(ObjectHeader);
    ObpDeleteNameCheck(Object);// Now delete the object name if no more handles are present.
}