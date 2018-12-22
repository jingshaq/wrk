/*
Copyright (c) Microsoft Corporation. All rights reserved.

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.

Module Name:
    callback.c

Abstract:
   This module implements the executive callback object.
   Functions are provided to open, register, unregister , and notify callback objects.

Revision History:
    Added low overhead callbacks for critical components like thread/registry etc.
    These routines have a high probability of not requiring any locks for an individual call.
*/

#include "exp.h"

// Callback Specific Access Rights.
#define CALLBACK_MODIFY_STATE    0x0001
#define CALLBACK_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | CALLBACK_MODIFY_STATE )

KEVENT ExpCallbackEvent;// Event to wait for registration to become idle
EX_PUSH_LOCK ExpCallBackFlush;// Lock used when fast referencing fails.

// Debug flag to force certain code paths. Let it get optimized away on free builds.
#if DBG
BOOLEAN ExpCallBackReturnRefs = FALSE;
#else
const BOOLEAN ExpCallBackReturnRefs = FALSE;
#endif

POBJECT_TYPE ExCallbackObjectType;// Address of callback object type descriptor.

// Structure that describes the mapping of generic access rights to object specific access rights for callback objects.

#ifdef ALLOC_DATA_PRAGMA
#pragma const_seg("INITCONST")
#endif
const GENERIC_MAPPING ExpCallbackMapping = {
    STANDARD_RIGHTS_READ ,
    STANDARD_RIGHTS_WRITE | CALLBACK_MODIFY_STATE,
    STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
    CALLBACK_ALL_ACCESS
};
#ifdef ALLOC_DATA_PRAGMA
#pragma const_seg()
#endif

typedef struct _CALLBACK_OBJECT{// Executive callback object structure definition.
    ULONG       Signature;
    KSPIN_LOCK  Lock;
    LIST_ENTRY  RegisteredCallbacks;
    BOOLEAN     AllowMultipleCallbacks;
    UCHAR       reserved[3];
} CALLBACK_OBJECT, *PCALLBACK_OBJECT;

typedef struct _CALLBACK_REGISTRATION{// Executive callback registration structure definition.
    LIST_ENTRY          Link;
    PCALLBACK_OBJECT    CallbackObject;
    PCALLBACK_FUNCTION  CallbackFunction;
    PVOID               CallbackContext;
    ULONG               Busy;
    BOOLEAN             UnregisterWaiting;
} CALLBACK_REGISTRATION, *PCALLBACK_REGISTRATION;

VOID ExpDeleteCallback(IN PCALLBACK_OBJECT CallbackObject);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, ExpInitializeCallbacks)
#pragma alloc_text(PAGE, ExCreateCallback)
#pragma alloc_text(PAGE, ExpDeleteCallback)
#pragma alloc_text(PAGE, ExInitializeCallBack)
#pragma alloc_text(PAGE, ExCompareExchangeCallBack)
#pragma alloc_text(PAGE, ExCallCallBack)
#pragma alloc_text(PAGE, ExFreeCallBack)
#pragma alloc_text(PAGE, ExAllocateCallBack)
#pragma alloc_text(PAGE, ExReferenceCallBackBlock)
#pragma alloc_text(PAGE, ExGetCallBackBlockRoutine)
#pragma alloc_text(PAGE, ExWaitForCallBacks)
#pragma alloc_text(PAGE, ExGetCallBackBlockContext)
#pragma alloc_text(PAGE, ExDereferenceCallBackBlock)
#endif


BOOLEAN ExpInitializeCallbacks()
/*
Routine Description:
    This function creates the callback object type descriptor at system initialization and stores the address of the object type descriptor in local static storage.
Return Value:
    A value of TRUE is returned if the timer object type descriptor is successfully initialized. Otherwise a value of FALSE is returned.
*/
{
    OBJECT_TYPE_INITIALIZER ObjectTypeInitializer;
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;
    UNICODE_STRING unicodeString;
    ULONG i;
    HANDLE handle;

    ExInitializePushLock(&ExpCallBackFlush);// Initialize the slow referencing lock
    RtlInitUnicodeString(&unicodeString, L"Callback");// Initialize string descriptor.

    // Create timer object type descriptor.
    RtlZeroMemory(&ObjectTypeInitializer, sizeof(ObjectTypeInitializer));
    ObjectTypeInitializer.Length = sizeof(ObjectTypeInitializer);
    ObjectTypeInitializer.InvalidAttributes = OBJ_OPENLINK;
    ObjectTypeInitializer.GenericMapping = ExpCallbackMapping;
    ObjectTypeInitializer.DeleteProcedure = ExpDeleteCallback;
    ObjectTypeInitializer.PoolType = NonPagedPool;
    ObjectTypeInitializer.ValidAccessMask = CALLBACK_ALL_ACCESS;
    Status = ObCreateObjectType(&unicodeString, &ObjectTypeInitializer, (PSECURITY_DESCRIPTOR)NULL, &ExCallbackObjectType);
    if (!NT_SUCCESS(Status)) {
        return FALSE;
    }

    RtlInitUnicodeString(&unicodeString, ExpWstrCallback);
    InitializeObjectAttributes(&ObjectAttributes, &unicodeString, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, SePublicDefaultSd);
    Status = NtCreateDirectoryObject(&handle, DIRECTORY_ALL_ACCESS, &ObjectAttributes);
    if (!NT_SUCCESS(Status)) {
        return FALSE;
    }
    NtClose(handle);

    KeInitializeEvent(&ExpCallbackEvent, NotificationEvent, 0);// Initialize event to wait on for Unregisters which occur while notifications are in progress

    // Initialize NT global callbacks
    for (i = 0; ExpInitializeCallback[i].CallBackObject; i++) {
        RtlInitUnicodeString(&unicodeString, ExpInitializeCallback[i].CallbackName);
        InitializeObjectAttributes(&ObjectAttributes, &unicodeString, OBJ_PERMANENT | OBJ_CASE_INSENSITIVE, NULL, NULL);
        Status = ExCreateCallback(ExpInitializeCallback[i].CallBackObject, &ObjectAttributes, TRUE, TRUE);// Create named calledback
        if (!NT_SUCCESS(Status)) {
            return FALSE;
        }
    }

    return TRUE;
}


NTSTATUS ExCreateCallback(__deref_out PCALLBACK_OBJECT * CallbackObject,
                          __in POBJECT_ATTRIBUTES ObjectAttributes,
                          __in BOOLEAN Create, 
                          __in BOOLEAN AllowMultipleCallbacks)
/*
Routine Description:
    This function opens a callback object with the specified callback object.
    If the callback object does not exist or it is a NULL then a callback object will be created if create is TRUE.
    If a callbackobject is created it will only support multiple registered callbacks if AllowMulitipleCallbacks is TRUE.
Arguments:
    CallbackObject - Supplies a pointer to a variable that will receive the Callback object.
    CallbackName  - Supplies a pointer to a object name that will receive the
    Create - Supplies a flag which indicates whether a callback object will be created or not .
    AllowMultipleCallbacks - Supplies a flag which indicates only support multiple registered callbacks.
*/
{
    PCALLBACK_OBJECT cbObject;
    NTSTATUS Status;
    HANDLE Handle;

    PAGED_CODE();

    // Initializing cbObject & Handle is not needed for correctness but without it the compiler cannot compile this code W4 to check for use of uninitialized variables.
    Handle = NULL;
    cbObject = NULL;

    // If named callback, open handle to it
    if (ObjectAttributes->ObjectName) {
        Status = ObOpenObjectByName(ObjectAttributes, ExCallbackObjectType, KernelMode, NULL, 0,// DesiredAccess,
                                    NULL, &Handle);
    } else {
        Status = STATUS_UNSUCCESSFUL;
    }

    // If not opened, check if callback should be created
    if (!NT_SUCCESS(Status) && Create) {
        Status = ObCreateObject(KernelMode, ExCallbackObjectType, ObjectAttributes, KernelMode, NULL, sizeof(CALLBACK_OBJECT), 0, 0, (PVOID *)&cbObject);
        if (NT_SUCCESS(Status)) {
            cbObject->Signature = 'llaC';// Fill in structure signature
            cbObject->AllowMultipleCallbacks = AllowMultipleCallbacks;// It will support multiple registered callbacks if AllowMultipleCallbacks is TRUE.
            InitializeListHead(&cbObject->RegisteredCallbacks);// Initialize CallbackObject queue.
            KeInitializeSpinLock(&cbObject->Lock);// Initialize spinlock
            Status = ObInsertObject(cbObject, NULL, FILE_READ_DATA, 0, NULL, &Handle);// Put the object in the root directory
        }
    }

    if (NT_SUCCESS(Status)) {// Add one to callback object reference count.
        Status = ObReferenceObjectByHandle(Handle, 0,// DesiredAccess
                                           ExCallbackObjectType, KernelMode, &cbObject, NULL);
        ZwClose(Handle);
    }
    if (NT_SUCCESS(Status)) {// If success, returns a referenced pointer to the CallbackObject.
        *CallbackObject = cbObject;
    }
    return Status;
}


VOID ExpDeleteCallback(IN PCALLBACK_OBJECT CallbackObject)
{
#if !DBG
    UNREFERENCED_PARAMETER(CallbackObject);
#endif

    ASSERT(IsListEmpty(&CallbackObject->RegisteredCallbacks));
}


PVOID ExRegisterCallback(__inout PCALLBACK_OBJECT CallbackObject, 
                         __in PCALLBACK_FUNCTION CallbackFunction,
                         __in_opt PVOID CallbackContext)
/*
Routine Description:
    This routine allows a caller to register that it would like to have its callback Function invoked when the callback notification call occurs.
Arguments:
    CallbackObject - Supplies a pointer to a CallbackObject.
    CallbackFunction - Supplies a pointer to a function which is to be executed when the Callback notification occurs.
    CallbackContext - Supplies a pointer to an arbitrary data structure that will be passed to the function specified by the CallbackFunction parameter.
Return Value:
    Returns handle to callback registration.
*/
{
    PCALLBACK_REGISTRATION CallbackRegistration;
    BOOLEAN Inserted;
    KIRQL OldIrql;

    ASSERT(CallbackFunction);
    ASSERT(KeGetCurrentIrql() < DISPATCH_LEVEL);

    ObReferenceObject(CallbackObject);// Add reference to object

    // Begin by attempting to allocate storage for the CallbackRegistration.
    // one cannot be allocated, return the error status.
    CallbackRegistration = ExAllocatePoolWithTag(NonPagedPool, sizeof(CALLBACK_REGISTRATION), 'eRBC');
    if (!CallbackRegistration) {
        ObDereferenceObject(CallbackObject);
        return NULL;
    }

    // Initialize the callback packet
    CallbackRegistration->CallbackObject = CallbackObject;
    CallbackRegistration->CallbackFunction = CallbackFunction;
    CallbackRegistration->CallbackContext = CallbackContext;
    CallbackRegistration->Busy = 0;
    CallbackRegistration->UnregisterWaiting = FALSE;

    Inserted = FALSE;
    KeAcquireSpinLock(&CallbackObject->Lock, &OldIrql);
    if (CallbackObject->AllowMultipleCallbacks || IsListEmpty(&CallbackObject->RegisteredCallbacks)) {
        Inserted = TRUE;
        InsertTailList(&CallbackObject->RegisteredCallbacks, &CallbackRegistration->Link);// add CallbackRegistration to tail
    }
    KeReleaseSpinLock(&CallbackObject->Lock, OldIrql);

    if (!Inserted) {
        ExFreePool(CallbackRegistration);
        ObDereferenceObject(CallbackObject);
        CallbackRegistration = NULL;
    }

    return (PVOID)CallbackRegistration;
}


VOID ExUnregisterCallback(__inout PVOID CbRegistration)
/*
Routine Description:
    This function removes the callback registration for the callbacks from the list of callback object .
Arguments:
    CallbackRegistration - Pointer to device object for the file system.
*/
{
    PCALLBACK_REGISTRATION CallbackRegistration;
    PCALLBACK_OBJECT CallbackObject;
    KIRQL OldIrql;

    ASSERT(KeGetCurrentIrql() < DISPATCH_LEVEL);

    CallbackRegistration = (PCALLBACK_REGISTRATION)CbRegistration;
    CallbackObject = CallbackRegistration->CallbackObject;

    KeAcquireSpinLock(&CallbackObject->Lock, &OldIrql);

    // Wait for registration
    while (CallbackRegistration->Busy) {
        // Set waiting flag, then wait.  (not performance critical - use single global event to wait for any and all unregister waits)
        CallbackRegistration->UnregisterWaiting = TRUE;
        KeClearEvent(&ExpCallbackEvent);
        KeReleaseSpinLock(&CallbackObject->Lock, OldIrql);
        KeWaitForSingleObject(&ExpCallbackEvent, Executive, KernelMode, FALSE, NULL);
        KeAcquireSpinLock(&CallbackObject->Lock, &OldIrql);// Synchronize with callback object and recheck registration busy
    }

    // Registration not busy, remove it from the callback object
    RemoveEntryList(&CallbackRegistration->Link);
    KeReleaseSpinLock(&CallbackObject->Lock, OldIrql);

    ExFreePool(CallbackRegistration);// Free memory used for CallbackRegistration
    ObDereferenceObject(CallbackObject);// Remove reference count on CallbackObject
}


VOID ExNotifyCallback(__in PCALLBACK_OBJECT CallbackObject, __in_opt PVOID Argument1, __in_opt PVOID Argument2)
/*
Routine Description:
    This function notifies all registered callbacks .
Arguments:
    CallbackObject - supplies a pointer to the callback object should be notified.
    SystemArgument1 - supplies a pointer will be passed to callback function.
    SystemArgument2 - supplies a pointer will be passed to callback function.
*/
{
    PLIST_ENTRY Link;
    PCALLBACK_REGISTRATION CallbackRegistration;
    KIRQL OldIrql;

    if (CallbackObject == NULL || IsListEmpty(&CallbackObject->RegisteredCallbacks)) {
        return;
    }

    KeAcquireSpinLock(&CallbackObject->Lock, &OldIrql);// Synchronize with callback object

    // call registered callbacks at callers IRQL level ( done if FIFO order of registration )
    if (OldIrql == DISPATCH_LEVEL) {
        // OldIrql is DISPATCH_LEVEL, just invoke all callbacks without releasing the lock
        for (Link = CallbackObject->RegisteredCallbacks.Flink; Link != &CallbackObject->RegisteredCallbacks; Link = Link->Flink) {
            CallbackRegistration = CONTAINING_RECORD(Link, CALLBACK_REGISTRATION, Link); // Get current registration to notify
            CallbackRegistration->CallbackFunction(CallbackRegistration->CallbackContext, Argument1, Argument2);// Notify registration
        }// next registration
    } else {// OldIrql is < DISPATCH_LEVEL, the code being called may be pageable and the callback object spinlock needs to be released around each registration callback.
        for (Link = CallbackObject->RegisteredCallbacks.Flink; Link != &CallbackObject->RegisteredCallbacks; Link = Link->Flink) {
            CallbackRegistration = CONTAINING_RECORD(Link, CALLBACK_REGISTRATION, Link);// Get current registration to notify
            if (!CallbackRegistration->UnregisterWaiting) {// If registration is being removed, don't bother calling it
                CallbackRegistration->Busy += 1;// Set registration busy

                // Release SpinLock and notify this callback
                KeReleaseSpinLock(&CallbackObject->Lock, OldIrql);
                CallbackRegistration->CallbackFunction(CallbackRegistration->CallbackContext, Argument1, Argument2);

                KeAcquireSpinLock(&CallbackObject->Lock, &OldIrql);// Synchronize with CallbackObject
                CallbackRegistration->Busy -= 1;// Remove our busy count
                if (CallbackRegistration->UnregisterWaiting  && CallbackRegistration->Busy == 0) {// If the registration removal is pending, kick global event let unregister continue
                    KeSetEvent(&ExpCallbackEvent, 0, FALSE);
                }
            }
        }
    }

    KeReleaseSpinLock(&CallbackObject->Lock, OldIrql);// Release callback
}


VOID ExInitializeCallBack(IN OUT PEX_CALLBACK CallBack)
/*
Routine Description:
    This function initializes a low overhead callback.
Arguments:
    CallBack - Pointer to the callback structure
*/
{
    ExFastRefInitialize(&CallBack->RoutineBlock, NULL);
}


PEX_CALLBACK_ROUTINE_BLOCK ExAllocateCallBack(IN PEX_CALLBACK_FUNCTION Function, IN PVOID Context)
/*
Routine Description:
    This function allocates a low overhead callback.
Arguments:
    Function - Routine to issue callbacks to
    Context  - A context value to issue
Return Value:
    PEX_CALLBACK_ROUTINE_BLOCK - Allocated block or NULL if allocation fails.
*/
{
    PEX_CALLBACK_ROUTINE_BLOCK NewBlock = ExAllocatePoolWithTag(PagedPool, sizeof(EX_CALLBACK_ROUTINE_BLOCK), 'brbC');
    if (NewBlock != NULL) {
        NewBlock->Function = Function;
        NewBlock->Context = Context;
        ExInitializeRundownProtection(&NewBlock->RundownProtect);
    }

    return NewBlock;
}


VOID ExFreeCallBack(IN PEX_CALLBACK_ROUTINE_BLOCK CallBackBlock)
/*
Routine Description:
    This function destroys a low overhead callback block.
Arguments:
    CallBackBlock - Call back block to destroy
*/
{
    ExFreePool(CallBackBlock);
}


VOID ExWaitForCallBacks(IN PEX_CALLBACK_ROUTINE_BLOCK CallBackBlock)
/*
Routine Description:
    This function waits for all outcalls on the specified callback block to complete
Arguments:
    CallBackBlock - Call back block to wait for
*/
{
    ExWaitForRundownProtectionRelease(&CallBackBlock->RundownProtect);// Wait for all active callbacks to be finished.
}


BOOLEAN ExCompareExchangeCallBack(IN OUT PEX_CALLBACK CallBack,
                                  IN PEX_CALLBACK_ROUTINE_BLOCK NewBlock,
                                  IN PEX_CALLBACK_ROUTINE_BLOCK OldBlock)
/*
Routine Description:
    This function assigns, removes or swaps a low overhead callback function.
Arguments:
    CallBack - Callback structure to be modified
    NewBlock - New block to be installed in the callback
    OldBlock - The old block that must be there now to be replaced
Return Value:
    BOOLEAN - TRUE: The swap occured, FALSE: The swap failed
*/
{
    EX_FAST_REF OldRef;
    PEX_CALLBACK_ROUTINE_BLOCK ReplacedBlock;

    if (NewBlock != NULL) {// Add the additional references to the routine block
        if (!ExAcquireRundownProtectionEx(&NewBlock->RundownProtect, ExFastRefGetAdditionalReferenceCount() + 1)) {
            ASSERTMSG("Callback block is already undergoing rundown", FALSE);
            return FALSE;
        }
    }

    // Attempt to replace the existing object and balance all the reference counts
    OldRef = ExFastRefCompareSwapObject(&CallBack->RoutineBlock, NewBlock, OldBlock);
    ReplacedBlock = ExFastRefGetObject(OldRef);

    // See if the swap occured. If it didn't undo the original references we added.
    // If it did then release remaining references on the original
    if (ReplacedBlock == OldBlock) {// We need to flush out any slow referencers at this point. We do this by acquiring and releasing a lock.
        if (ReplacedBlock != NULL) {
            PKTHREAD CurrentThread = KeGetCurrentThread();
            KeEnterCriticalRegionThread(CurrentThread);
            ExAcquireReleasePushLockExclusive(&ExpCallBackFlush);
            KeLeaveCriticalRegionThread(CurrentThread);
            ExReleaseRundownProtectionEx(&ReplacedBlock->RundownProtect, ExFastRefGetUnusedReferences(OldRef) + 1);
        }
        return TRUE;
    } else {// The swap failed. Remove the addition references if we had added any.
        if (NewBlock != NULL) {
            ExReleaseRundownProtectionEx(&NewBlock->RundownProtect, ExFastRefGetAdditionalReferenceCount() + 1);
        }
        return FALSE;
    }
}


PEX_CALLBACK_ROUTINE_BLOCK ExReferenceCallBackBlock(IN OUT PEX_CALLBACK CallBack)
/*
Routine Description:
    This function takes a reference on the call back block inside the callback structure.
Arguments:
    CallBack - Call back to obtain the call back block from
Return Value:
    PEX_CALLBACK_ROUTINE_BLOCK - Referenced structure or NULL if these wasn't one
*/
{
    PEX_CALLBACK_ROUTINE_BLOCK CallBackBlock;
    EX_FAST_REF OldRef = ExFastReference(&CallBack->RoutineBlock);// Get a reference to the callback block if we can.

    if (ExFastRefObjectNull(OldRef)) {// If there is no callback then return
        return NULL;
    }

    if (!ExFastRefCanBeReferenced(OldRef)) {// If we didn't get a reference then use a lock to get one.
        PKTHREAD CurrentThread = KeGetCurrentThread();
        KeEnterCriticalRegionThread(CurrentThread);
        ExAcquirePushLockExclusive(&ExpCallBackFlush);
        CallBackBlock = ExFastRefGetObject(CallBack->RoutineBlock);
        if (CallBackBlock && !ExAcquireRundownProtection(&CallBackBlock->RundownProtect)) {
            CallBackBlock = NULL;
        }
        ExReleasePushLockExclusive(&ExpCallBackFlush);
        KeLeaveCriticalRegionThread(CurrentThread);
        if (CallBackBlock == NULL) {
            return NULL;
        }
    } else {
        CallBackBlock = ExFastRefGetObject(OldRef);
        if (ExFastRefIsLastReference(OldRef) && !ExpCallBackReturnRefs) {// If we just removed the last reference then attempt fix it up.
            ULONG RefsToAdd;
            RefsToAdd = ExFastRefGetAdditionalReferenceCount();
            if (ExAcquireRundownProtectionEx(&CallBackBlock->RundownProtect, RefsToAdd)) {// If we can't add the references then just give up
                if (!ExFastRefAddAdditionalReferenceCounts(&CallBack->RoutineBlock, CallBackBlock, RefsToAdd)) {// Repopulate the cached refs. If this fails we just give them back.
                    ExReleaseRundownProtectionEx(&CallBackBlock->RundownProtect, RefsToAdd);
                }
            }
        }
    }

    return CallBackBlock;
}


PEX_CALLBACK_FUNCTION ExGetCallBackBlockRoutine(IN PEX_CALLBACK_ROUTINE_BLOCK CallBackBlock)
/*
Routine Description:
    This function gets the routine associated with a call back block
Arguments:
    CallBackBlock - Call back block to obtain routine for
Return Value:
    PEX_CALLBACK_FUNCTION - The function pointer associated with this block
*/
{
    return CallBackBlock->Function;
}


PVOID ExGetCallBackBlockContext(IN PEX_CALLBACK_ROUTINE_BLOCK CallBackBlock)
/*
Routine Description:
    This function gets the context associated with a call back block
Arguments:
    CallBackBlock - Call back block to obtain context for
Return Value:
    PVOID - The context associated with this block
*/
{
    return CallBackBlock->Context;
}


VOID ExDereferenceCallBackBlock(IN OUT PEX_CALLBACK CallBack, IN PEX_CALLBACK_ROUTINE_BLOCK CallBackBlock)
/*
Routine Description:
    This returns a reference previous obtained on a call back block
Arguments:
    CallBackBlock - Call back block to return reference to
*/
{
    if (ExpCallBackReturnRefs || !ExFastRefDereference(&CallBack->RoutineBlock, CallBackBlock)) {
        ExReleaseRundownProtection(&CallBackBlock->RundownProtect);
    }
}


NTSTATUS ExCallCallBack(IN OUT PEX_CALLBACK CallBack, IN PVOID Argument1, IN PVOID Argument2)
/*
Routine Description:
    This function calls the callback thats inside a callback structure
Arguments:
    CallBack - Call back that needs to be called through
    Argument1 - Caller provided argument to pass on
    Argument2 - Caller provided argument to pass on
Return Value:
    NTSTATUS - Status returned by callback or STATUS_SUCCESS if theres wasn't one
*/
{
    NTSTATUS Status;
    PEX_CALLBACK_ROUTINE_BLOCK CallBackBlock = ExReferenceCallBackBlock(CallBack);
    if (CallBackBlock) {
        Status = CallBackBlock->Function(CallBackBlock->Context, Argument1, Argument2);// Call the function
        ExDereferenceCallBackBlock(CallBack, CallBackBlock);
    } else {
        Status = STATUS_SUCCESS;
    }

    return Status;
}
