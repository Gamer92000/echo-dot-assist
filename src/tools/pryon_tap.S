.syntax unified
.thumb
.fpu neon
.text
.align 2
.global pryon_capture_stub
.type pryon_capture_stub,%function
.thumb_func
pryon_capture_stub:
    push.w {r0-r12,lr}
    vpush {d0-d15}
    vpush {d16-d31}
    add.w r0,sp,#256
    add.w r1,r0,#56
    bl pryon_capture_enter
    vpop {d16-d31}
    vpop {d0-d15}
    pop.w {r0-r12,lr}
    b.w pryon_capture_resume
.align 2
.global pryon_capture_resume
.type pryon_capture_resume,%function
.thumb_func
pryon_capture_resume:
    .space 16
.section .note.GNU-stack,"",%progbits
