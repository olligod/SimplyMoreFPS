// NativeAOT 10 on Apple ARM64 can lose d8-d15 while handling an exception.
// Keep the public C ABI intact even on those paths; internal entries stay hidden.
// AAPCS64 preserves only the low 64 bits of v8-v15. All five calls use x0-x6.
#if !defined(__APPLE__) || !defined(__aarch64__)
#error This camera ABI boundary is only for Apple ARM64.
#endif

.text

.macro CAMERA_EXPORT public_name, managed_name
    .p2align 2
    .globl \public_name
    .private_extern \managed_name
\public_name:
    .cfi_startproc
    stp x29, x30, [sp, #-80]!
    .cfi_def_cfa_offset 80
    .cfi_offset 29, -80
    .cfi_offset 30, -72
    mov x29, sp

    stp d8, d9, [sp, #16]
    stp d10, d11, [sp, #32]
    stp d12, d13, [sp, #48]
    stp d14, d15, [sp, #64]
    .cfi_offset 72, -64
    .cfi_offset 73, -56
    .cfi_offset 74, -48
    .cfi_offset 75, -40
    .cfi_offset 76, -32
    .cfi_offset 77, -24
    .cfi_offset 78, -16
    .cfi_offset 79, -8

    bl \managed_name

    ldp d8, d9, [sp, #16]
    ldp d10, d11, [sp, #32]
    ldp d12, d13, [sp, #48]
    ldp d14, d15, [sp, #64]
    ldp x29, x30, [sp], #80
    .cfi_def_cfa_offset 0
    ret
    .cfi_endproc
.endm

CAMERA_EXPORT _smf_camera_create, _smf_managed_camera_create
CAMERA_EXPORT _smf_camera_adopt, _smf_managed_camera_adopt
CAMERA_EXPORT _smf_camera_configure, _smf_managed_camera_configure
CAMERA_EXPORT _smf_camera_step, _smf_managed_camera_step
CAMERA_EXPORT _smf_camera_release, _smf_managed_camera_release
