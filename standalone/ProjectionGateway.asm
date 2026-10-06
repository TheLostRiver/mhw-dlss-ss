; Original shim for the verified native projection-copy site (not a function entry).
; At this site RSP is 16-byte aligned, RAX is the getter result, RBX is the view,
; EBP is its slot, and RSP+20h is the getter's caller-owned scratch matrix.
; No exception may unwind through this shim. The C++ callback contains its own
; exception boundary and guards all game-data accesses.
option casemap:none
EXTERN MhwObserveProjection:PROC
EXTERN MhwProjectionTrampoline:QWORD
PUBLIC MhwProjectionGateway
.code
MhwProjectionGateway PROC
    pushfq
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    sub rsp, 80h
    ; 20h bytes of caller shadow space, followed by the six volatile XMMs.
    movdqu [rsp+20h], xmm0
    movdqu [rsp+30h], xmm1
    movdqu [rsp+40h], xmm2
    movdqu [rsp+50h], xmm3
    movdqu [rsp+60h], xmm4
    movdqu [rsp+70h], xmm5
    mov rcx, [rsp+0B0h]
    mov rdx, rbx
    lea r8, [rsp+0E0h]
    mov r9d, ebp
    call MhwObserveProjection
    movdqu xmm0, [rsp+20h]
    movdqu xmm1, [rsp+30h]
    movdqu xmm2, [rsp+40h]
    movdqu xmm3, [rsp+50h]
    movdqu xmm4, [rsp+60h]
    movdqu xmm5, [rsp+70h]
    add rsp, 80h
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    popfq
    ; The trampoline executes the displaced 7-byte LEA, then resumes the game.
    jmp QWORD PTR [MhwProjectionTrampoline]
MhwProjectionGateway ENDP
END
