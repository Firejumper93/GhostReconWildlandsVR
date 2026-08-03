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
;     the project rules forbid. Get it backwards and we forward garbage to the engine.
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

END
