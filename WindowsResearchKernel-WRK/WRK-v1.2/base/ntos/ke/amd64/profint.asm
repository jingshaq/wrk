        title  "Profile Interrupt"


; Copyright (c) Microsoft Corporation. All rights reserved.

; You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
; If you do not agree to the terms, do not use the code.


; Module Name:

;   profint.asm

; Abstract:

;   This module implements the architecture dependent code necessary to
;   process the profile interrupt.



include ksamd64.inc

        extern  KiProfileListHead:qword
        extern  KiProfileLock:qword
        extern  PerfProfileInterrupt:proc

        subttl  "Profile Interrupt"


; VOID
; KeProfileInterruptWithSource (
;     IN PKTRAP_FRAME TrapFrame,
;     IN KPROFILE_SOURCE ProfileSource
;     )

; Routine Description:

;   This routine is executed in response to an interrupt generated by one
;   of the profile sources. Its function is to process the system and process
;   profile lists and update bucket hit counters.

; Arguments:

;   TrapFrame (rcx) - Supplies the address of a trap frame.

;   ProfileSource (rdx) - Supplies the source of profile interrupt.

; Return Value:

;    None.



PiFrame struct
        P1Home  dq ?                    ; parameter home addresses
        P2Home  dq ?                    ;
        P3Home  dq ?                    ;
        P4Home  dq ?                    ;
        Source  dq ?                    ; interrupt source home address
        ListHead dq ?                   ; list head address home address
        TrapFrame dq ?                  ; saved trap frame
PiFrame ends

        NESTED_ENTRY KeProfileInterruptWithSource, _TEXT$00

        alloc_stack (sizeof PiFrame)    ; allocate stack frame

        END_PROLOGUE

        mov     PiFrame.TrapFrame[rsp], rcx; save trap frame
        mov     PiFrame.Source[rsp], rdx; save interrupt source


; Check for profile logging.


        mov     rax, gs:[PcPerfGlobalGroupMask]; get global mask address
        test    rax, rax                ; test if logging enabled
        je      short KePI10            ; if e, logging not enabled
        test    dword ptr PERF_PROFILE_OFFSET[rax], PERF_PROFILE_FLAG ; check flag
        jz      short KePI10            ; if z, profile events not enabled
        xchg    rcx, rdx                ; exchange source and trap frame address
        mov     rdx, TrRip + 128[rdx]   ; set profile interrupt address
        call    PerfProfileInterrupt

KePI10: AcquireSpinLock KiProfileLock   ; acquire profile spin lock

        mov     rcx, PiFrame.TrapFrame[rsp] ; set trap frame
        mov     rdx, PiFrame.Source[rsp]; set interrupt source
        mov     r8, gs:[PcCurrentThread]; get current thread address
        mov     r8, ThApcState + AsProcess[r8] ; get current process address
        add     r8, PrProfileListHead   ; compute profile listhead address
        call    KiProcessProfileList    ; process profile list
        mov     rcx, PiFrame.TrapFrame[rsp] ; set trap frame
        mov     rdx, PiFrame.Source[rsp]; set interrupt source
        lea     r8, KiProfileListHead   ; get system profile listhead address
        call    KiProcessProfileList    ; process profile list

        ReleaseSpinLock KiProfileLock   ; release spin lock

        add     rsp, (sizeof PiFrame)   ; deallocate stack frame
        ret                             ; return

        NESTED_END KeProfileInterruptWithSource, _TEXT$00

        subttl  "Process Profile List"


; VOID
; KiProcessProfileList (
;     IN PKTRAP_FRAME TrapFrame,
;     IN KPROFILE_SOURCE Source,
;     IN PLIST_ENTRY ListHead
;     )

; Routine Description:

;   This routine processes a profile list.

; Arguments:

;   TrapFrame (rcx) - Supplies the address of a trap frame.

;   Source (dx) - Supplies the source of profile interrupt.

;   ListHead (r8) - Supplies a pointer to a profile list.

; Return Value:

;    None.



        LEAF_ENTRY KiProcessProfileList, _TEXT$00

        movzx   eax, dx                 ; save profile source
        mov     rdx, LsFlink[r8]        ; get first entry address
        cmp     rdx, r8                 ; check if list is empty
        je      short KiPP30            ; if e, list is empty
        mov     r9, gs:[PcSetMember]    ; get processor set member
        mov     r10, TrRip + 128[rcx]   ; get profile interrupt address


; Process list entry.


KiPP10: cmp     ax, (PfSource - PfProfileListEntry)[rdx] ; check for source match
        jne     short KiPP20            ; if ne, source mismatch
        cmp     r10, (PfRangeBase - PfProfileListEntry)[rdx] ; check if below base
        jb      short KiPP20            ; if b, address below base
        cmp     r10, (PfRangeLimit - PfProfileListEntry)[rdx] ; check if above limit
        jae     short KiPP20            ; if ae, address above limit
        test    r9, (PfAffinity - PfProfileListEntry)[rdx] ; check if in set
        jz      short KiPP20            ; if z, processor not in set
        movzx   ecx, byte ptr (PfBucketShift - PfProfileListEntry)[rdx] ; get shift count
        mov     r11, r10                ; compute offset into profile buffer
        sub     r11, (PfRangeBase - PfProfileListEntry)[rdx] ;
        shr     r11, cl                 ;
        and     r11, NOT 3              ;
        mov     rcx, (PfBuffer - PfProfileListEntry)[rdx] ; get profile buffer address
        inc     dword ptr [r11][rcx]    ; increment profile bucket
KiPP20: mov     rdx, LsFlink[rdx]         ; get next entry address
        cmp     rdx, r8                 ; check if end of list
        jne     short KiPP10            ; if ne, not end of list
KiPP30: ret                             ; return

        LEAF_END KiProcessProfileList, _TEXT$00

        end

