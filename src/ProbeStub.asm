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
EXTERN grwxr_headhide_ring     : QWORD ; BUILD 121: 16 distinct objects
EXTERN grwxr_headhide_ring_cls : QWORD ; their class method tables

; Build 67: the gun-root bone write. See CameraProbe.cpp for the evidence.
EXTERN grwxr_wgun_skel  : QWORD        ; player skeleton to match, 0 = disarmed
EXTERN grwxr_wgun_impl  : QWORD        ; the real PublishAttachments
EXTERN grwxr_wgun_node  : DWORD        ; bone node index (Fake_gunroot)
EXTERN grwxr_wgun_dz    : DWORD        ; float, metres added to the z lane
EXTERN grwxr_wgun_calls : QWORD
EXTERN grwxr_wgun_writes: QWORD
EXTERN grwxr_wgun_rot   : DWORD       ; build 80: 1 = rotate, 0 = the lift
EXTERN grwxr_wgun_apply : PROC        ; build 80: the quaternion helper

; Build 68: TransformNode::SetWorldTransform substitution.
EXTERN grwxr_wnode_impl : QWORD
EXTERN grwxr_wnode_calls: QWORD
EXTERN grwxr_wnode_prep : PROC     ; C: returns replacement matrix, or 0

; Build 50, the aim-reader census (see AimTrace.h). Unlike every stub above,
; these two RECORD ONLY: they modify no argument and no engine state.
EXTERN grwxr_aimget_record : PROC
EXTERN grwxr_aimget_disp   : DWORD   ; [0] yaw getter, [1] pitch getter
EXTERN grwxr_aimget_vals   : DWORD   ; [0] value in, [1] value out (build 52)

; Build 53, the per-shot weapon routine observer. Pure pass-through.
EXTERN grwxr_wfire_record : PROC
EXTERN grwxr_wfire_orig   : QWORD    ; the real function, from the E9 thunk

; Build 54, the Havok world raycast census. Pure pass-through.
EXTERN grwxr_ray_record : PROC
EXTERN grwxr_ray_orig   : QWORD

; Build 58, TtCastRay: the raycast body the wrapper reaches virtually, with
; its own thunk and three direct callers. Shares build 54's recorder so the
; caller census and the shot window cover both entries in one table.
EXTERN grwxr_ray2_orig  : QWORD

; Build 55, GetAimOrientation. The ONE stub here that can modify a result.
EXTERN grwxr_aimq_post : PROC
EXTERN grwxr_aimq_orig : QWORD

; BUILD 105, the anchor world getters. Same shape as aimq: they RETURN a
; value into the caller's buffer, so these CALL the original and then
; post-process, rather than tail-jumping like the observers.
EXTERN grwxr_aimpt_post   : PROC
EXTERN grwxr_aimpt_orig   : QWORD
EXTERN grwxr_muzpt_post   : PROC
EXTERN grwxr_muzpt_orig   : QWORD

; Build 56, the ballistic projectile spawn.
EXTERN grwxr_spawn_pre  : PROC
EXTERN grwxr_spawn_post : PROC
EXTERN grwxr_spawn_orig : QWORD

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

    ; BUILD 121 CENSUS. Record every DISTINCT object this setter is called on,
    ; with its class method table, so we can see whether hands and helmet come
    ; through here and not just the head. Runs BEFORE the class test on purpose:
    ; the whole point is to see the objects that do NOT match.
    ;
    ; r10/r11/rax are volatile and are not argument registers for
    ; SetHidden(rcx = this, dl = hidden), so clobbering them is safe. No call,
    ; no stack, no allocation: rule 8 is unchanged. 16 slots, and once full the
    ; scan simply stops recording, which is a bounded cost by construction.
    lea     r10, [grwxr_headhide_ring]
    xor     r11d, r11d
hh_scan:
    cmp     r11d, 16
    jae     hh_census_done
    mov     rax, [r10 + r11*8]
    test    rax, rax
    jz      hh_census_store
    cmp     rax, rcx
    je      hh_census_done
    inc     r11d
    jmp     hh_scan
hh_census_store:
    mov     [r10 + r11*8], rcx
    mov     rax, [rcx+8]
    lea     r10, [grwxr_headhide_ring_cls]
    mov     [r10 + r11*8], rax
hh_census_done:

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

; ---------------------------------------------------------------------------
; Build 67: THE GUN-ROOT BONE WRITE, at Skeleton::PublishAttachments.
;
; VERIFIED (headset, 88 samples, log grwxr-18692): the held weapon's world
; origin tracks the PLAYER rig's Fake_gunroot bone to within 0..6 mm across
; 0.7 m of movement. PublishAttachments is the function that composes each
; attached object's world transform from this bone buffer and then places the
; object, so its entry is the last instant at which changing the bone still
; changes where the engine puts the gun. Writing at SkeletonPostUpdate entry
; instead would be overwritten by the animation solver that runs in between.
;
; rcx = Skeleton*, edx = 0xFFFF, r8b = flag. xmm0 carries no argument here
; (all three parameters are integer), and xmm0 is volatile, so using it needs
; no save. Rule 8 holds: no call, no stack, no allocation, no logging.
;
; Every dereference is guarded, and a zero grwxr_wgun_skel disarms the whole
; stub, so the pass-through path is the same instruction stream the game
; would have run anyway.
; ---------------------------------------------------------------------------
; BUILD 80: mode 3 routes to a C helper instead, because setting the barrel on
; the controller ray is quaternion work. The LIFT path below is untouched: it
; is verified in the headset (node 10 raises the gun 60 cm) and a refactor of a
; proven path to share code with an unproven one would put both at risk.
;
; The call needs rdx and r8 preserved, which the lift path never did because it
; called nothing: PublishAttachments takes (rcx skeleton, dx boneFilter,
; r8b recurse) and the C helper is free to clobber all three volatiles.
; Entry via jmp leaves rsp congruent to 8 mod 16 and 38h is itself 8 mod 16, so
; the call sees a 16-aligned stack, the same arithmetic grwxr_aimget_common
; documents.
grwxr_wgun_entry PROC
    lock inc qword ptr [grwxr_wgun_calls]
    mov     rax, [grwxr_wgun_skel]
    test    rax, rax
    jz      wg_pass
    cmp     rax, rcx                       ; player skeleton only
    jne     wg_pass
    cmp     dword ptr [grwxr_wgun_rot], 0
    jne     wg_rot
    mov     rax, [rcx + 238h]              ; pose = [skel+0x238]
    test    rax, rax
    jz      wg_pass
    mov     r11, [rax + 178h]              ; buf = [pose+0x178]
    test    r11, r11
    jz      wg_pass
    mov     eax, dword ptr [grwxr_wgun_node]
    shl     rax, 5                         ; record stride 0x20
    add     r11, rax                       ; rec = buf + node*0x20
    movss   xmm0, dword ptr [r11 + 8]      ; translation z lane (game up axis)
    addss   xmm0, dword ptr [grwxr_wgun_dz]
    movss   dword ptr [r11 + 8], xmm0
    lock inc qword ptr [grwxr_wgun_writes]
    jmp     wg_pass
wg_rot:
    sub     rsp, 38h
    mov     [rsp+20h], rcx
    mov     [rsp+28h], rdx
    mov     [rsp+30h], r8
    call    grwxr_wgun_apply               ; (rcx = skeleton)
    mov     rcx, [rsp+20h]
    mov     rdx, [rsp+28h]
    mov     r8,  [rsp+30h]
    add     rsp, 38h
wg_pass:
    mov     rax, [grwxr_wgun_impl]
    jmp     rax
grwxr_wgun_entry ENDP

; ---------------------------------------------------------------------------
; Build 68: THE WEAPON PLACEMENT SUBSTITUTION.
;
; TransformNode::SetWorldTransform(node rcx, const float4x4* rdx, bool r8b,
; bool r9b) is the engine's own commit of an object's world placement, and for
; a held weapon it is called by the character's attachment publish. Replacing
; rdx here is NOT a race with the engine: we are inside the writer, handing it
; a different value to commit. Every precedent converges on this (FRIK
; abandoned its renderer and animation hooks for the player update; UEVR calls
; the equivalent "Permanent Change"; REFramework hooks named pipeline stages),
; and it is the only option with no timing hazard at all.
;
; We never write through the caller's pointer. The C helper fills OUR buffer
; and returns it, so a non-matching node leaves every byte of the game's own
; argument untouched and the call proceeds exactly as it would have.
;
; 9 rel32 sites reach this function, so the C helper's node gate is what keeps
; us off every other object in the world.
; ---------------------------------------------------------------------------
grwxr_wnode_entry PROC
    lock inc qword ptr [grwxr_wnode_calls]
    sub     rsp, 68h                       ; 0x20 shadow + saves, keeps 16-align
    mov     [rsp+30h], rcx
    mov     [rsp+38h], rdx
    mov     [rsp+40h], r8
    mov     [rsp+48h], r9
    call    grwxr_wnode_prep               ; (rcx = node, rdx = matrix)
    mov     rcx, [rsp+30h]
    mov     rdx, [rsp+38h]
    mov     r8,  [rsp+40h]
    mov     r9,  [rsp+48h]
    test    rax, rax
    cmovnz  rdx, rax                       ; substitute only on a gate match
    add     rsp, 68h
    mov     rax, [grwxr_wnode_impl]
    jmp     rax
grwxr_wnode_entry ENDP

; ---------------------------------------------------------------------------
; Build 50: replacements for the aim-angle GETTER dispatch stubs, RVA
; 0x006764B0 (yaw, vtable +0x5B0) and 0x00677600 (pitch, vtable +0x5F0),
; whose verified bodies are `mov rax,[rcx]; mov rdx,[rax+disp]; jmp rdx`
; (docs/RE-notes.md "THE ABSOLUTE AIM ANGLE EXISTS"; both byte sequences are
; unique in both shipped binaries).
;
; These are PURE OBSERVERS: they record the CALLER's return address and the
; object, then re-emulate the dispatch exactly. Nothing about the call the
; engine makes changes, which is the whole point of a log-only probe.
;
; The recorder is a real call, so unlike the setter stubs above this one needs
; the full argument-register save and the frame alignment that
; grwxr_probe_common documents: entry via jmp leaves rsp congruent to 8 mod 16,
; and 0B8h is itself 8 mod 16, so the `call` sees a 16-aligned stack. rcx must
; survive the call because the tail dispatch dereferences it.
;
; xmm0-3 are saved for the same reason the projection probe saves them: these
; are dispatch stubs shared with other classes, and a class we have not
; analysed may take float arguments. rax, r10 and r11 are volatile at entry.
; ---------------------------------------------------------------------------
grwxr_aimget_common PROC PRIVATE
    sub     rsp, 0B8h

    mov     [rsp+40h], rcx
    mov     [rsp+48h], rdx
    mov     [rsp+50h], r8
    mov     [rsp+58h], r9
    movups  [rsp+60h], xmm0
    movups  [rsp+70h], xmm1
    movups  [rsp+80h], xmm2
    movups  [rsp+90h], xmm3
    mov     [rsp+0A0h], r10

    ; grwxr_aimget_record(index, return_address, object) -> delta float bits
    mov     rcx, r10
    mov     rdx, [rsp+0B8h]         ; the caller's return address
    mov     r8,  [rsp+40h]          ; the object (this)
    call    grwxr_aimget_record
    mov     [rsp+0B0h], eax         ; stash the delta (free slot in our frame)

    mov     r10, [rsp+0A0h]
    movups  xmm3, [rsp+90h]
    movups  xmm2, [rsp+80h]
    movups  xmm1, [rsp+70h]
    movups  xmm0, [rsp+60h]
    mov     r9,  [rsp+58h]
    mov     r8,  [rsp+50h]
    mov     rdx, [rsp+48h]
    mov     rcx, [rsp+40h]

    ; the vtable byte offset for this accessor, taken from the verified slot
    ; bytes at install rather than hardcoded here
    lea     r11, grwxr_aimget_disp
    mov     r11d, dword ptr [r11 + r10*4]

    ; Build 51: a non-zero delta means THIS caller is one of the verified
    ; per-shot sites and its result is to be adjusted. That path has to see
    ; the return value, so it calls instead of tail-jumping. Every other
    ; caller, which is essentially all of them, keeps the pure tail jump.
    mov     eax, [rsp+0B0h]
    test    eax, eax
    jnz     ag_override

    add     rsp, 0B8h               ; rsp and [rsp] are now exactly as the real
                                    ; accessor would have seen them
    ; The original dispatch, re-emulated register for register: it leaves the
    ; vtable in rax and the method in rdx, so the accessor is entered in the
    ; exact state the engine's own stub would have left. The only difference
    ; is r11, which is volatile scratch that no callee may rely on.
    mov     rax, [rcx]
    mov     rdx, [rax + r11]
    jmp     rdx

    ; Override path. rsp is congruent to 0 mod 16 here (entry was 8, minus
    ; 0B8h), which is exactly what a call needs, and [rsp+0..3Fh] is unused
    ; frame that serves as the callee's shadow space. The accessor returns its
    ; float in xmm0; we add the delta and return to the original caller.
    ; Build 52: record what came back and what we ship. r11 has served its
    ; purpose by now, so it is free to address the pair. Two plain stores,
    ; still no logging and no calls on this path.
ag_override:
    mov     rax, [rcx]
    mov     rdx, [rax + r11]
    call    rdx
    lea     r11, grwxr_aimget_vals
    movd    eax, xmm0
    mov     [r11], eax                  ; the accessor's real answer
    movd    xmm1, dword ptr [rsp+0B0h]
    addss   xmm0, xmm1
    movd    eax, xmm0
    mov     [r11+4], eax                ; what the caller actually receives
    add     rsp, 0B8h
    ret
grwxr_aimget_common ENDP

grwxr_aimget_yaw_entry PROC
    mov     r10d, 0
    jmp     grwxr_aimget_common
grwxr_aimget_yaw_entry ENDP

grwxr_aimget_pitch_entry PROC
    mov     r10d, 1
    jmp     grwxr_aimget_common
grwxr_aimget_pitch_entry ENDP

; ---------------------------------------------------------------------------
; Build 53: observer on the per-shot weapon routine (thunk 0x029AB510 ->
; 0x124B8360). The function is VIRTUAL: it has no direct callers and lives in
; a vtable slot, so static analysis cannot say who invokes it. Hooking it and
; recording the caller's return address is the only way to learn the pipeline.
;
; From its own prologue the arguments are known: rcx = this (the object whose
; +0xAC / +0xB0 receive the computed angle pair), r8 = the context whose +0x20
; leads to the aim angle object, and xmm1 = a float the routine spills
; immediately (a spread or deviation radius, shape-wise).
;
; PURE PASS-THROUGH: records and tail-jumps to the real function, so the
; engine cannot tell the difference. Same frame and alignment reasoning as
; grwxr_probe_common above.
; ---------------------------------------------------------------------------
grwxr_wfire_entry PROC
    sub     rsp, 0B8h

    mov     [rsp+40h], rcx
    mov     [rsp+48h], rdx
    mov     [rsp+50h], r8
    mov     [rsp+58h], r9
    movups  [rsp+60h], xmm0
    movups  [rsp+70h], xmm1
    movups  [rsp+80h], xmm2
    movups  [rsp+90h], xmm3

    ; grwxr_wfire_record(return_address, this, ctx, float bits of xmm1)
    mov     rcx, [rsp+0B8h]
    mov     rdx, [rsp+40h]
    mov     r8,  [rsp+50h]
    movd    r9d, xmm1
    call    grwxr_wfire_record

    movups  xmm3, [rsp+90h]
    movups  xmm2, [rsp+80h]
    movups  xmm1, [rsp+70h]
    movups  xmm0, [rsp+60h]
    mov     r9,  [rsp+58h]
    mov     r8,  [rsp+50h]
    mov     rdx, [rsp+48h]
    mov     rcx, [rsp+40h]

    mov     r11, [grwxr_wfire_orig]
    add     rsp, 0B8h
    jmp     r11
grwxr_wfire_entry ENDP

; ---------------------------------------------------------------------------
; Build 54: observer on hknpWorld::castRay (thunk 0x030B08E0 -> 0x13E68070),
; signature castRay(hknpWorld* rcx, RayInput* rdx, Collector* r8).
;
; THE HOTTEST FUNCTION WE HAVE EVER HOOKED. Every vehicle wheel, every AI
; line-of-sight check and every world query comes through here, so rule 8 is
; not a guideline on this path: the recorder does a bounded scan of a 12-entry
; table, a handful of float loads and plain stores, and nothing else. All
; logging happens on the drain thread.
;
; Pure pass-through: records the caller and the ray, then tail-jumps.
; ---------------------------------------------------------------------------
grwxr_ray_entry PROC
    sub     rsp, 0B8h

    mov     [rsp+40h], rcx
    mov     [rsp+48h], rdx
    mov     [rsp+50h], r8
    mov     [rsp+58h], r9
    movups  [rsp+60h], xmm0
    movups  [rsp+70h], xmm1
    movups  [rsp+80h], xmm2
    movups  [rsp+90h], xmm3

    ; grwxr_ray_record(return_address, ray_input)
    mov     rcx, [rsp+0B8h]
    mov     rdx, [rsp+48h]
    call    grwxr_ray_record

    movups  xmm3, [rsp+90h]
    movups  xmm2, [rsp+80h]
    movups  xmm1, [rsp+70h]
    movups  xmm0, [rsp+60h]
    mov     r9,  [rsp+58h]
    mov     r8,  [rsp+50h]
    mov     rdx, [rsp+48h]
    mov     rcx, [rsp+40h]

    mov     r11, [grwxr_ray_orig]
    add     rsp, 0B8h
    jmp     r11
grwxr_ray_entry ENDP

; ---------------------------------------------------------------------------
; Build 58: TtCastRay (thunk 0x030C9990 -> 0x13EA3550), the raycast body
; behind the wrapper's virtual call. rdx is the ray input here too (the
; wrapper passes its own rdx through unchanged). Identical pass-through
; shape to grwxr_ray_entry; only the saved original differs.
; ---------------------------------------------------------------------------
grwxr_ray2_entry PROC
    sub     rsp, 0B8h

    mov     [rsp+40h], rcx
    mov     [rsp+48h], rdx
    mov     [rsp+50h], r8
    mov     [rsp+58h], r9
    movups  [rsp+60h], xmm0
    movups  [rsp+70h], xmm1
    movups  [rsp+80h], xmm2
    movups  [rsp+90h], xmm3

    ; grwxr_ray_record(return_address, ray_input)
    mov     rcx, [rsp+0B8h]
    mov     rdx, [rsp+48h]
    call    grwxr_ray_record

    movups  xmm3, [rsp+90h]
    movups  xmm2, [rsp+80h]
    movups  xmm1, [rsp+70h]
    movups  xmm0, [rsp+60h]
    mov     r9,  [rsp+58h]
    mov     r8,  [rsp+50h]
    mov     rdx, [rsp+48h]
    mov     rcx, [rsp+40h]

    mov     r11, [grwxr_ray2_orig]
    add     rsp, 0B8h
    jmp     r11
grwxr_ray2_entry ENDP

; ---------------------------------------------------------------------------
; Build 55: GetAimOrientation (thunk 0x029A8E80 -> 0x124B0770).
;   float4 GetAimOrientation(out /*rcx*/, aimSubObj /*rdx*/, int* mode /*r8*/)
; Verified body: parent = rdx-0x48; when *mode == -1 and the dirty byte
; [parent+0x4B0] is clear it returns the CACHED quaternion [parent+0x420],
; otherwise it rebuilds base * yaw * pitch * roll. Either way the result is
; written to [rcx].
;
; This is the only stub here that MODIFIES a result, so unlike the observers
; above it CALLS the original rather than tail-jumping, then rotates the
; quaternion in the caller's own output buffer. xmm0 is reloaded from that
; buffer afterwards, because the fast path leaves the quaternion in xmm0 as
; well and we do not know which the caller reads.
;
; rax is preserved across our post-processing in case the function returns
; the out pointer, which is the usual convention for this shape.
; ---------------------------------------------------------------------------
grwxr_aimq_entry PROC
    sub     rsp, 0B8h
    mov     [rsp+40h], rcx          ; the caller's output buffer

    mov     r11, [grwxr_aimq_orig]
    call    r11                     ; args are still in rcx/rdx/r8/r9
    mov     [rsp+68h], rax          ; whatever it returned

    mov     rcx, [rsp+40h]
    test    rcx, rcx
    jz      aq_done
    mov     rdx, [rsp+0B8h]         ; the caller's return address (build 56)
    call    grwxr_aimq_post         ; void post(float4* q, u64 ret)
    mov     rcx, [rsp+40h]
    movups  xmm0, xmmword ptr [rcx] ; keep both return conventions in agreement
aq_done:
    mov     rax, [rsp+68h]
    add     rsp, 0B8h
    ret
grwxr_aimq_entry ENDP

; ---------------------------------------------------------------------------
; BUILD 105: the anchor world getters.
;
;   float4* f(float4* out /*rcx*/, IBallisticGenerator* iface /*rdx*/)
;   weapon = rdx - 0x80   (MSVC multiple-inheritance this-adjustment)
;
; Cloned from grwxr_aimq_entry above, which is the only proven
; modify-the-result stub in this file, with ONE addition: rdx is preserved
; across the original call and handed to the post routine, because the weapon
; pointer is derived from it and there is no other way to reach the owner
; gate.
;
; Stack safety: after `sub rsp,0B8h` the callee's shadow space overlaps our
; [rsp+0..0x20), so every slot we use is at 0x40 or above. That is why aimq
; chose 0x40 and 0x68, and it is why 0x48 is safe here.
; ---------------------------------------------------------------------------
grwxr_aimpt_entry PROC
    sub     rsp, 0B8h
    mov     [rsp+40h], rcx          ; the caller's output buffer
    mov     [rsp+48h], rdx          ; the interface pointer (weapon + 0x80)

    mov     r11, [grwxr_aimpt_orig]
    call    r11
    mov     [rsp+68h], rax

    mov     rcx, [rsp+40h]
    test    rcx, rcx
    jz      ap_done
    mov     rdx, [rsp+48h]
    call    grwxr_aimpt_post        ; void post(float4* out, u64 iface)
    mov     rcx, [rsp+40h]
    movups  xmm0, xmmword ptr [rcx] ; keep both return conventions in agreement
ap_done:
    mov     rax, [rsp+68h]
    add     rsp, 0B8h
    ret
grwxr_aimpt_entry ENDP

; The muzzle twin. Observer only: it never modifies the result, it caches it
; so the drain line can print the muzzle-to-aim-point distance. Kept in the
; same shape as its sibling so the two cannot drift apart.
grwxr_muzpt_entry PROC
    sub     rsp, 0B8h
    mov     [rsp+40h], rcx
    mov     [rsp+48h], rdx

    mov     r11, [grwxr_muzpt_orig]
    call    r11
    mov     [rsp+68h], rax

    mov     rcx, [rsp+40h]
    test    rcx, rcx
    jz      mp_done
    mov     rdx, [rsp+48h]
    call    grwxr_muzpt_post        ; void post(const float4* out, u64 iface)
mp_done:
    mov     rax, [rsp+68h]
    add     rsp, 0B8h
    ret
grwxr_muzpt_entry ENDP

; ---------------------------------------------------------------------------
; Build 56: the ballistic projectile spawn (thunk 0x02986B20 -> 0x12458BD0),
; __fastcall(rcx = owner/shot object, rdx = bullet DB entry).
;
; The spawn copies [owner+0x140] into the projectile's
; m_vBulletSimulationDirection. So to steer the bullet we rewrite that field
; just before the copy and put it back immediately after, exactly the
; "modify the argument in place, never leave it modified" pattern build 47
; used on the placement setter. That is why this CALLS the original instead
; of tail-jumping: the restore has to happen on the way out.
;
; pre() records and optionally rotates; post() restores. Both take the owner.
; ---------------------------------------------------------------------------
grwxr_spawn_entry PROC
    sub     rsp, 0B8h

    mov     [rsp+40h], rcx
    mov     [rsp+48h], rdx
    mov     [rsp+50h], r8
    mov     [rsp+58h], r9
    movups  [rsp+60h], xmm0
    movups  [rsp+70h], xmm1
    movups  [rsp+80h], xmm2
    movups  [rsp+90h], xmm3

    mov     rcx, [rsp+40h]
    call    grwxr_spawn_pre

    movups  xmm3, [rsp+90h]
    movups  xmm2, [rsp+80h]
    movups  xmm1, [rsp+70h]
    movups  xmm0, [rsp+60h]
    mov     r9,  [rsp+58h]
    mov     r8,  [rsp+50h]
    mov     rdx, [rsp+48h]
    mov     rcx, [rsp+40h]

    mov     r11, [grwxr_spawn_orig]
    call    r11                     ; the real spawn, with our direction in place
    mov     [rsp+68h], rax

    mov     rcx, [rsp+40h]
    mov     rdx, [rsp+68h]          ; build 59: the spawn's result, so post can
    call    grwxr_spawn_post        ; validate it as the projectile instance

    mov     rax, [rsp+68h]
    add     rsp, 0B8h
    ret
grwxr_spawn_entry ENDP

END
