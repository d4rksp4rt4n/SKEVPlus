; Small x64 stubs called from the game's code (via a jump slot near the exe, see near_jmp in skmod.c).
; Each replaces the game instructions noted and keeps every register the game still uses there.
EXTERN g_scene:QWORD, g_flag:QWORD, g_app:QWORD, res_w:DWORD, res_h:DWORD, fov_ref:DWORD, near_max:DWORD, one_f:DWORD, shadow_vt:QWORD, g_dofnear:QWORD, shadow_ret:QWORD
EXTERN g_frametime:QWORD, ui_acc:DWORD, g_wstep:QWORD, fast_menus:DWORD, g_gamestep:QWORD, shop_model_fn:QWORD
.code

; FrameRate: menu/UI code was written for steps of at least 1/60 s and often truncates the step to a
; whole number (a step of 0.5 truncates to 0: the lottery wheel never turned and the menu hung). The UI
; manager update (once per frame) computes its step as max(1.0, frame time); above 60 fps time is
; accumulated instead, giving a whole step: 1.0 on the frames where a full 1/60 s has passed, 0 on the
; others (same speed, same whole frames as at 60 fps). It is stored at [g_wstep] (read by the shop
; menu, patched in skmod.c) and used as the UI step unless FastMenus=1 (then the original 1.0 = 2x).
; Replaces "comiss xmm6, xmm0; ja +3; movaps xmm6, xmm0" (xmm6 = 1.0, xmm0 = frame time).
ui_step PROC
    mov r11, g_wstep
    comiss xmm0, dword ptr one_f
    jb short_frame
    movaps xmm6, xmm0                   ; 1/60 s or longer: original behaviour
    movss dword ptr [r11], xmm0
    mov dword ptr ui_acc, 0
    ret
short_frame:
    addss xmm0, dword ptr ui_acc
    comiss xmm0, dword ptr one_f
    jb not_yet
    subss xmm0, dword ptr one_f         ; xmm6 stays 1.0
    movss dword ptr ui_acc, xmm0
    movss dword ptr [r11], xmm6
    ret
not_yet:
    movss dword ptr ui_acc, xmm0
    mov dword ptr [r11], 0
    cmp dword ptr fast_menus, 0
    jne keep_one                        ; FastMenus=1: UI keeps 1.0 every frame
    xorps xmm6, xmm6
keep_one:
    ret
ui_step ENDP

; FrameRate: the shop passes its (now whole) step to the clerk's model update too; a step of 0 on
; every other frame made her chest alternate between two poses (ghosting at 120 fps). That call
; (replaces "call 140198a20" after "movaps xmm0, xmm6") gets the real frame-time step instead.
shop_model PROC
    mov rax, g_gamestep
    movss xmm0, dword ptr [rax]
    jmp qword ptr shop_model_fn
shop_model ENDP

; FrameRate: model instances advance their object animation (frame +60h += speed +64h) and material
; animation (frame +80h += speed +84h) once per frame. Some callers set the speed from the frame time
; every frame; where it is still the default 1.0 (e.g. the sea's scrolling material, props) it is
; scaled by min(frame time, 1): unchanged at 60 fps and below, never applied twice.
; xmm1 = scaled speed (xmm0 is free here; xmm2 must stay 0 for the game).
scale_speed PROC
    ucomiss xmm1, dword ptr one_f
    jne keep
    jp keep
    mov r11, g_frametime
    movss xmm0, dword ptr [r11]
    minss xmm0, dword ptr one_f
    mulss xmm1, xmm0
keep:
    ret
scale_speed ENDP

obj_adv PROC                        ; replaces movss xmm1, [rbx+64h]; addss xmm1, [rbx+60h]
    movss xmm1, dword ptr [rbx + 64h]
    call scale_speed
    addss xmm1, dword ptr [rbx + 60h]
    ret
obj_adv ENDP

mat_adv PROC                        ; replaces movss xmm1, [rbx+84h]
    movss xmm1, dword ptr [rbx + 84h]
    jmp scale_speed
mat_adv ENDP

; SkipIntro=1, replaces "mov [scene], 4" (CTaskFirstCheck -> Marvelous movie): go where the CRI logo
; would go after the logos: OP movie (9) once the prologue was seen, else the prologue (10).
intro_stub PROC
    mov rax, g_flag
    movzx eax, byte ptr [rax]
    neg al
    sbb eax, eax
    add eax, 10
    mov r11, g_scene
    mov dword ptr [r11], eax
    ret
intro_stub ENDP

; MaxResolution, fullscreen sizes. App object: +2Ch/+30h target size, +44h/+48h fullscreen size.
; res_start replaces "mov [+44h], ecx; mov [+48h], eax" at startup (ecx/eax = monitor size, then
; used as the back buffer size); res_apply the same with edx when the screen mode is applied.
res_start PROC
    cmp res_w, 0
    je keep
    mov ecx, res_w
    mov eax, res_h
keep:
    mov r11, g_app
    mov dword ptr [r11 + 2Ch], ecx
    mov dword ptr [r11 + 30h], eax
    mov dword ptr [r11 + 44h], ecx
    mov dword ptr [r11 + 48h], eax
    ret
res_start ENDP

res_apply PROC
    cmp res_w, 0
    je keep
    mov ecx, res_w
    mov edx, res_h
keep:
    mov r11, g_app
    mov dword ptr [r11 + 2Ch], ecx
    mov dword ptr [r11 + 30h], edx
    mov dword ptr [r11 + 44h], ecx
    mov dword ptr [r11 + 48h], edx
    ret
res_apply ENDP

; NearClip: replace Tamlib::CCamera GetNear (returns xmm0) and GetNearFar (writes [rdx], [r8]); rcx =
; camera, +260h near, +264h far, +26Ch vertical fov (radians). For view cameras the near distance
; grows with the zoom: near * clamp(fov_ref / fov, 1, near_max). Depth of field uses its own fixed
; near (1.0, tuned for the cameras' 0.1) and gets the same factor. Shadow cameras, and the shadow
; code fitting its cascades to the view camera (4 call sites, return addresses in shadow_ret), get the
; original near.
near_get PROC
    movss xmm0, dword ptr [rcx + 260h]
    mov rax, shadow_vt
    cmp qword ptr [rcx], rax
    je done
    mov rax, qword ptr [rsp]
    lea r10, shadow_ret
    cmp rax, qword ptr [r10]
    je done
    cmp rax, qword ptr [r10 + 8]
    je done
    cmp rax, qword ptr [r10 + 16]
    je done
    cmp rax, qword ptr [r10 + 24]
    je done
    movss xmm1, dword ptr fov_ref
    divss xmm1, dword ptr [rcx + 26Ch]
    minss xmm1, dword ptr near_max
    maxss xmm1, dword ptr one_f
    mulss xmm0, xmm1
    mov rax, g_dofnear
    movss dword ptr [rax], xmm1
done:
    ret
near_get ENDP

near_getpair PROC
    call near_get
    movss dword ptr [rdx], xmm0
    mov eax, dword ptr [rcx + 264h]
    mov dword ptr [r8], eax
    ret
near_getpair ENDP

END
