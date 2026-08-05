; ProbeStub.asm - an ABI-safe way to observe any engine function's arguments
; without knowing its prototype.
;
; WHY THIS EXISTS
;
; The obvious way to probe an engine function is to declare a C++ function with
; the same signature, hook it, record the arguments, and call through. That
; works only if the declared prototype is exactly right, and on this target it
; usually is not:
;
;   * docs/RE-notes.md documents six projection variants with different shapes.
;     Guessing which arguments are floats in xmm and which are pointers in
;     integer registers is exactly the kind of plausible-but-wrong step that
;     the project rules forbids. Get it backwards and we forward garbage to the engine.
;   * on_calc_mvp takes 21 arguments. Writing that prototype by hand, from a
;     static reading of one call site, and then having the engine execute it, is
;     not a risk worth taking to answer a question about call counts.
;
; This stub sidesteps prototypes entirely. It saves every register that can
; carry an argument under the Microsoft x64 ABI, calls a recorder with a pointer
; to the saved block, restores everything, and TAIL JUMPS to the real function.
; Because it jumps rather than calls, the stack pointer and the caller's return
; address are exactly what the real function would have seen, and stack-passed
; arguments above the return address are never touched. The real function cannot
; tell the difference.
;
; Registers clobbered are r10, r11 and rax, all volatile at function entry under
; the ABI, so overwriting them is safe.
;
; Stack alignment: entry via jmp from the thunk leaves rsp congruent to 8 mod 16
; (the caller's `call` pushed a return address onto a 16-aligned stack). The ABI
; requires rsp congruent to 0 mod 16 immediately BEFORE a `call`, so that the
; callee sees 8 mod 16 after the return-address push. The frame reservation must
; therefore be 8 mod 16 itself: 0B8h. An earlier revision reserved 0B0h and
; called the recorder off by 8; that was latent until 2026-07-29, when added
; float math made the compiler spill xmm6-8 with movaps in the recorder's
; prologue, and the first aligned store faulted (ACCESS_VIOLATION, bad address
; 0xFFFFFFFFFFFFFFFF, the alignment-fault signature). See CURRENT-STATE.md.

EXTERN grwxr_probe_record : PROC
EXTERN grwxr_probe_originals : QWORD

; Build 17, the aim-architecture experiment (see CameraProbe.cpp).
EXTERN grwxr_setyaw_pending : DWORD    ; 1 = a one-shot bump is queued
EXTERN grwxr_setyaw_bump    : DWORD    ; float bits of the bump, radians
EXTERN grwxr_setyaw_disp    : DWORD    ; vtable byte offset from the slot bytes
EXTERN grwxr_setyaw_count   : QWORD
EXTERN grwxr_setyaw_lastobj : QWORD
EXTERN grwxr_setyaw_lastval : DWORD    ; float bits of the last incoming yaw
EXTERN grwxr_setyaw_shipped : DWORD    ; float bits of the bumped value we sent
EXTERN grwxr_setyaw_fired   : DWORD    ; 1 = a bump was consumed (drain clears)

; Build 19, the pitch half of aim injection (see CameraProbe.cpp).
EXTERN grwxr_setpitch_pending : DWORD
EXTERN grwxr_setpitch_bump    : DWORD
EXTERN grwxr_setpitch_disp    : DWORD
EXTERN grwxr_setpitch_count   : QWORD
EXTERN grwxr_setpitch_lastobj : QWORD
EXTERN grwxr_setpitch_lastval : DWORD

; Build 18, head hide (see CameraProbe.cpp).
EXTERN grwxr_headhide_table : QWORD    ; verified head-class method table VA
EXTERN grwxr_headhide_impl  : QWORD    ; the real SetHidden implementation
EXTERN grwxr_headhide_on    : DWORD    ; 1 = force hide on the matching class
EXTERN grwxr_headhide_calls : QWORD
EXTERN grwxr_headhide_forced: QWORD
EXTERN grwxr_headhide_obj   : QWORD    ; last matching object seen

.code

; ---------------------------------------------------------------------------
; The shared body. Entered with the probe index in r10.
; ---------------------------------------------------------------------------
grwxr_probe_common PROC PRIVATE
    sub     rsp, 0B8h

    ; Saved argument block starts at rsp+40h. Keep this layout in sync with
    ; struct SavedArgs in CameraProbe.cpp.
    mov     [rsp+40h], rcx
    mov     [rsp+48h], rdx
    mov     [rsp+50h], r8
    mov     [rsp+58h], r9
    movups  [rsp+60h], xmm0
    movups  [rsp+70h], xmm1
    movups  [rsp+80h], xmm2
    movups  [rsp+90h], xmm3
    mov     [rsp+0A0h], r10

    ; grwxr_probe_record(index, &saved, return_address, stack_args)
    mov     rcx, r10
    lea     rdx, [rsp+40h]
    mov     r8,  [rsp+0B8h]         ; the caller's return address
    lea     r9,  [rsp+0C0h]         ; first stack argument slot (shadow space)
    call    grwxr_probe_record

    mov     r10, [rsp+0A0h]
    movups  xmm3, [rsp+90h]
    movups  xmm2, [rsp+80h]
    movups  xmm1, [rsp+70h]
    movups  xmm0, [rsp+60h]
    mov     r9,  [rsp+58h]
    mov     r8,  [rsp+50h]
    mov     rdx, [rsp+48h]
    mov     rcx, [rsp+40h]

    lea     r11, grwxr_probe_originals
    mov     r11, [r11 + r10*8]

    add     rsp, 0B8h
    jmp     r11                     ; tail jump: the real function sees the
                                    ; original rsp, return address and stack args
grwxr_probe_common ENDP

; ---------------------------------------------------------------------------
; One entry point per hooked thunk, so the recorder knows which one fired.
; ---------------------------------------------------------------------------
PROBE_ENTRY MACRO n
grwxr_probe_entry_&n& PROC
    mov     r10d, n
    jmp     grwxr_probe_common
grwxr_probe_entry_&n& ENDP
ENDM

    PROBE_ENTRY 0
    PROBE_ENTRY 1
    PROBE_ENTRY 2
    PROBE_ENTRY 3
    PROBE_ENTRY 4
    PROBE_ENTRY 5
    PROBE_ENTRY 6
    PROBE_ENTRY 7
    PROBE_ENTRY 8
    PROBE_ENTRY 9
    PROBE_ENTRY 10

; ---------------------------------------------------------------------------
; Build 17: replacement for the SetYaw virtual-dispatch stub at RVA 0x006777C0,
; whose verified body is `mov rax,[rcx]; jmp qword ptr [rax+570h]` with
; rcx = the absolute-aim angle object and the yaw (radians) in xmm1
; (docs/RE-notes.md "THE ABSOLUTE AIM ANGLE EXISTS"; the movaps xmm1,xmm0
; immediately before the call at 0x124D34CE is the register proof).
;
; Normally a pure pass-through that only updates counters. When the user has
; queued a one-shot bump (Numpad Decimal), the NEXT call ships yaw+bump once.
; The engine's own integrate loop is get-modify-write on the same object, so
; a one-time bump persists without us touching anything again.
;
; project rule 8: no logging, no allocation, no locks, no calls. Plain and
; interlocked stores only. No stack use at all, so no alignment concerns.
; rax, r11 and the flags are volatile at function entry; xmm0 maps to argument
; slot 1, which is rcx (the object), so xmm0 carries nothing and is free.
; The tail emulates the original dispatch exactly, with the vtable offset
; taken from the verified slot bytes rather than hardcoded.
; ---------------------------------------------------------------------------
grwxr_setyaw_entry PROC
    lock inc qword ptr [grwxr_setyaw_count]
    mov     [grwxr_setyaw_lastobj], rcx
    movd    eax, xmm1
    mov     [grwxr_setyaw_lastval], eax

    ; consume a queued one-shot bump: pending -> 0, atomically
    xor     eax, eax
    xchg    eax, dword ptr [grwxr_setyaw_pending]
    test    eax, eax
    jz      sy_pass
    movd    xmm0, dword ptr [grwxr_setyaw_bump]
    addss   xmm1, xmm0
    movd    eax, xmm1
    mov     [grwxr_setyaw_shipped], eax
    mov     dword ptr [grwxr_setyaw_fired], 1
sy_pass:
    ; the original stub, re-implemented: mov rax,[rcx]; jmp [rax+disp]
    mov     rax, [rcx]
    mov     r11d, dword ptr [grwxr_setyaw_disp]
    mov     r11, [rax + r11]
    jmp     r11
grwxr_setyaw_entry ENDP

; ---------------------------------------------------------------------------
; Build 19: replacement for the SetPitch virtual-dispatch stub at RVA
; 0x005FA190 (`mov rax,[rcx]; jmp qword ptr [rax+5D0h]`, verified at install).
; Identical mechanism to grwxr_setyaw_entry: pass-through with counters, plus
; a consume-once delta armed by the render thread's aim pump. Same rule-8
; constraints, same register reasoning.
; ---------------------------------------------------------------------------
grwxr_setpitch_entry PROC
    lock inc qword ptr [grwxr_setpitch_count]
    mov     [grwxr_setpitch_lastobj], rcx
    movd    eax, xmm1
    mov     [grwxr_setpitch_lastval], eax

    xor     eax, eax
    xchg    eax, dword ptr [grwxr_setpitch_pending]
    test    eax, eax
    jz      sp_pass
    movd    xmm0, dword ptr [grwxr_setpitch_bump]
    addss   xmm1, xmm0
sp_pass:
    mov     rax, [rcx]
    mov     r11d, dword ptr [grwxr_setpitch_disp]
    mov     r11, [rax + r11]
    jmp     r11
grwxr_setpitch_entry ENDP

; ---------------------------------------------------------------------------
; Build 18: replacement for the SetHidden thunk (0x029DC7D0 -> 0x12582AC0),
; `__fastcall void(void* self rcx, bool hide dl)`, dl=1 hides
; (docs/RE-notes.md "The visibility setter" and "THE PROXIMITY HIDE").
;
; The engine re-asserts visibility EVERY camera update (hazard 29), so hiding
; the head is not one call, it is winning the argument on every call: when the
; object is THE head-visibility component (class identity: [rcx+8] equals the
; method table verified at install) and the mod wants the head hidden, the
; engine's `hide` argument is overridden to 1 on the way through. Everything
; else passes through untouched, and a zeroed table pointer disables the
; whole test.
;
; project rule 8: stores and interlocked increments only, no calls, no
; stack. rax and r11 are volatile at entry. [rcx+8] is the same field the
; real function's callers dereference, so reading it here adds no new risk.
; ---------------------------------------------------------------------------
grwxr_headhide_entry PROC
    lock inc qword ptr [grwxr_headhide_calls]
    mov     rax, [grwxr_headhide_table]
    test    rax, rax
    jz      hh_pass
    cmp     rax, [rcx+8]
    jne     hh_pass
    mov     [grwxr_headhide_obj], rcx      ; latched: the verified route to
                                           ; the pointer (RE-notes)
    cmp     dword ptr [grwxr_headhide_on], 0
    je      hh_pass
    mov     dl, 1                          ; force HIDE
    lock inc qword ptr [grwxr_headhide_forced]
hh_pass:
    mov     rax, [grwxr_headhide_impl]
    jmp     rax
grwxr_headhide_entry ENDP

END
