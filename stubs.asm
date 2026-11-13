; Small x64 stubs called from the game's code (via a jump slot near the exe, see near_jmp in skmod.c).
; Each replaces the game instructions noted and keeps every register the game still uses there.
EXTERN g_scene:QWORD, g_flag:QWORD, g_app:QWORD, res_w:DWORD, res_h:DWORD, near_min:DWORD, shadow_vt:QWORD
.code

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

; NearClip: replaces the first instruction (movss xmm0, [rcx+260h]) of Tamlib::CCamera SetNear and
; SetNearFar (xmm1 = near). View cameras get at least near_min; shadow cameras are left alone.
near_stub PROC
    mov rax, shadow_vt
    cmp qword ptr [rcx], rax
    je keep
    maxss xmm1, dword ptr near_min
keep:
    movss xmm0, dword ptr [rcx + 260h]
    ret
near_stub ENDP

END
