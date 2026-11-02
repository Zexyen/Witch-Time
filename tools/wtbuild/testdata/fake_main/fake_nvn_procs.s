/* tables test: the shapes src/nvn_procs.rs finds NVN by (no sdk code): a proc table (sorted name offsets,
 * a GOT slot to the function pointers, "mov w23, #count - 1"), two classes with typeinfo names and vtables, a format
 * table loaded through a GOT slot, the Tegra X1 descriptor and a texture layout function. */
    .text
    .p2align 2
    .globl fake_bootstrap_loader
fake_bootstrap_loader:
    adrp x1, proc_names
    add x1, x1, :lo12:proc_names
    adrp x2, proc_name_offs
    add x2, x2, :lo12:proc_name_offs
    adrp x3, :got:proc_ptrs
    ldr x3, [x3, :got_lo12:proc_ptrs]
    mov w23, #4
    ret

    .hidden p_buffer_get_address, p_device_get_integer, p_queue_submit, p_tb_alignment, p_tb_size, tex_layout
p_buffer_get_address:
    ldr x0, [x0, #8]
    ret
p_device_get_integer:
    mov w0, #7
    ret
p_queue_submit:
    add w0, w1, w2
    ret
p_tb_alignment:
    stp x29, x30, [sp, #-16]!
    adrp x8, :got:format_table
    ldr x8, [x8, :got_lo12:format_table]
    ldr w0, [x8, #4]
    ldp x29, x30, [sp], #16
    ret
p_tb_size:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    bl tex_layout
    add x0, x0, #0x100
    ldp x29, x30, [sp], #16
    ret
tex_layout:
    lsl x0, x0, #2
    ret
gm200_m0:
    mov w0, #0x12b
    ret
gm200_m1:
    mov w0, #1
    ret
dev_m0:
    mov w0, #2
    ret
dev_m1:
    ldr x0, [x0]
    ret
dev_m2:
    mov w0, #3
    ret

    .section .rodata
proc_names:
    .byte 0
1:  .asciz "BufferGetAddress"
2:  .asciz "DeviceGetInteger"
3:  .asciz "QueueSubmit"
4:  .asciz "TextureBuilderGetStorageAlignment"
5:  .asciz "TextureBuilderGetStorageSize"
    .p2align 2
proc_name_offs:
    .word 1b - proc_names, 2b - proc_names, 3b - proc_names, 4b - proc_names, 5b - proc_names
tegra_name:
    .asciz "NVIDIA Tegra/Nintendo Switch"
other_gpu_name:
    .asciz "Some Other GPU"
ti_name_dev:
    .asciz "N8nvrm_gpu5nvgpu15NvRmNvgpuDeviceE"
ti_name_hal:
    .asciz "N8nvrm_gpu33NvRmGpu_HwHalFunctions_GM200_BaseE"

    .data
    .p2align 3
proc_ptrs:
    .quad p_buffer_get_address, p_device_get_integer, p_queue_submit, p_tb_alignment, p_tb_size
format_table:
    .rept 7
    .quad 0x0000002500000001, 0x10, 0x20, 0x30, 0x40, 0x50
    .endr
format_next:
    .quad tex_layout, gm200_m0
chip_desc:
    .quad tegra_name, 0
    .word 0x12b, 0
other_desc:
    .quad other_gpu_name, 0
    .word 0x12c, 0
typeinfo_dev:
    .quad 0, ti_name_dev
typeinfo_hal:
    .quad 0, ti_name_hal
vtable_dev:
    .quad 0, typeinfo_dev, dev_m0, dev_m1, dev_m2
vtable_hal:
    .quad 0, typeinfo_hal, gm200_m0, gm200_m1
