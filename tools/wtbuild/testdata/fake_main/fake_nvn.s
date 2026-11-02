/* tables test: an nvnDeviceGetInteger-shaped pair for tables.rs's nvn_devinfo search (no sdk code; shapes only):
 * a getter switch whose cases are "mov w8, #imm" into a shared "*out = w8" tail, and its one caller: a compact jump
 * table whose cases are a fixed number, the preset w0 at the shared return, the getter's answer, or a device field. */
    .text
    .p2align 2
    .globl nvn_fake_getter
    .hidden nvn_fake_getter
    .type nvn_fake_getter, %function
nvn_fake_getter:
    sub w9, w0, #1
    cmp w9, #0x44
    b.hi 90f
    adrp x10, getter_tbl
    add x10, x10, :lo12:getter_tbl
    adr x11, 80f
    ldrb w12, [x10, x9]
    add x11, x11, x12, lsl #2
    mov w8, #4096
    br x11
80: str w8, [x1]
    mov w0, #1
    ret
90: mov w0, #0
    ret
g_1:
    mov w8, #0xf0f0f0f0
    b 80b
g_2:
    mov w8, #6328
    b 80b
g_4:
    mov w8, #256
    b 80b
g_6:
    mov w8, #0x3fffc000
    b 80b
g_8:
    mov w8, #28140
    b 80b
g_11:
    mov w8, #54810
    b 80b
g_13:
    mov w0, #0
    ret
g_15:
    mov w8, #32
    b 80b
g_16:
    mov w8, #7747
    b 80b
g_17:
    mov w8, #16226
    b 80b
g_18:
    mov w0, #0
    ret
g_19:
    mov w8, #8108
    b 80b
g_20:
    mov w8, #51993
    b 80b
g_22:
    mov w0, #0
    ret
g_24:
    mov w8, #17455
    b 80b
g_25:
    mov w8, #0x7ffffffe
    b 80b
g_26:
    mov w0, #0
    ret
g_27:
    mov w8, #15439
    b 80b
g_28:
    mov w8, #40433
    b 80b
g_29:
    mov w8, #23688
    b 80b
g_31:
    mov w8, #24624
    b 80b
g_32:
    mov w8, #0xff
    b 80b
g_33:
    mov w8, #8229
    b 80b
g_34:
    mov w8, #7812
    b 80b
g_35:
    mov w8, #26995
    b 80b
g_36:
    mov w8, #1048576
    b 80b
g_37:
    mov w8, #4096
    b 80b
g_38:
    mov w8, #41175
    b 80b
g_39:
    mov w8, #59399
    b 80b
g_40:
    mov w8, #0x55555555
    b 80b
g_41:
    mov w0, #0
    ret
g_42:
    mov w0, #0
    ret
g_43:
    mov w0, #0
    ret
g_45:
    mov w8, #39354
    b 80b
g_46:
    mov w8, #64895
    b 80b
g_47:
    mov w8, #0x7ffffffe
    b 80b
g_48:
    mov w8, #0x3fffc000
    b 80b
g_51:
    mov w8, #54804
    b 80b
g_52:
    mov w0, #0
    ret
g_53:
    mov w8, #0xf0f0f0f0
    b 80b
g_54:
    mov w8, #55272
    b 80b
g_57:
    mov w8, #41123
    b 80b
g_58:
    mov w8, #0x55555555
    b 80b
g_59:
    mov w8, #65100
    b 80b
g_60:
    mov w8, #59795
    b 80b
g_63:
    mov w8, #0x7ffffffe
    b 80b
g_66:
    mov w8, #0x3fffc000
    b 80b
g_67:
    mov w8, #37302
    b 80b
g_68:
    mov w8, #45482
    b 80b
    .size nvn_fake_getter, . - nvn_fake_getter

    .p2align 2
    .globl nvn_fake_get_integer
    .type nvn_fake_get_integer, %function
nvn_fake_get_integer:
    sub sp, sp, #0x30
    stp x29, x30, [sp, #0x10]
    stp x20, x19, [sp, #0x20]
    add x29, sp, #0x10
    mov w20, w1
    mov x19, x0
    sub x1, x29, #4
    mov w0, w20
    stur wzr, [x29, #-4]
    bl nvn_fake_getter
    sub w8, w20, #2
    cmp w8, #0x30
    b.hi 72f
    adrp x9, caller_tbl
    add x9, x9, :lo12:caller_tbl
    adr x10, 70f
    ldrb w11, [x9, x8]
    add x10, x10, x11, lsl #2
    mov w0, #0x35
    br x10
70: ldp x20, x19, [sp, #0x20]
71:
    ldp x29, x30, [sp, #0x10]
    add sp, sp, #0x30
    ret
72: ldur w0, [x29, #-4]
    b 70b
73: ldr w0, [x19]
    b 70b
74: ldr w0, [x19, #8]
    b 70b
c_4: mov w0, #0xffff
    b 70b
c_8: mov w0, #0xffff
    b 70b
c_15: mov w0, #0x9
    b 70b
c_17: mov w0, #0x800000
    b 70b
c_26: mov w0, #0x1
    b 70b
c_28: mov w0, #0x400
    b 70b
c_32: mov w0, #0x10000
    b 70b
c_33: mov w0, #0x9
    b 70b
c_34: mov w0, #0x10000
    b 70b
c_35: mov w0, #0xffff
    b 70b
c_36: mov w0, #0xffff
    b 70b
c_42: mov w0, #0x1
    b 70b
    .size nvn_fake_get_integer, . - nvn_fake_get_integer

    .section .rodata
getter_tbl:
    .byte (g_1 - 80b) / 4
    .byte (g_2 - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (g_4 - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (g_6 - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (g_8 - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (g_11 - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (g_13 - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (g_15 - 80b) / 4
    .byte (g_16 - 80b) / 4
    .byte (g_17 - 80b) / 4
    .byte (g_18 - 80b) / 4
    .byte (g_19 - 80b) / 4
    .byte (g_20 - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (g_22 - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (g_24 - 80b) / 4
    .byte (g_25 - 80b) / 4
    .byte (g_26 - 80b) / 4
    .byte (g_27 - 80b) / 4
    .byte (g_28 - 80b) / 4
    .byte (g_29 - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (g_31 - 80b) / 4
    .byte (g_32 - 80b) / 4
    .byte (g_33 - 80b) / 4
    .byte (g_34 - 80b) / 4
    .byte (g_35 - 80b) / 4
    .byte (g_36 - 80b) / 4
    .byte (g_37 - 80b) / 4
    .byte (g_38 - 80b) / 4
    .byte (g_39 - 80b) / 4
    .byte (g_40 - 80b) / 4
    .byte (g_41 - 80b) / 4
    .byte (g_42 - 80b) / 4
    .byte (g_43 - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (g_45 - 80b) / 4
    .byte (g_46 - 80b) / 4
    .byte (g_47 - 80b) / 4
    .byte (g_48 - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (g_51 - 80b) / 4
    .byte (g_52 - 80b) / 4
    .byte (g_53 - 80b) / 4
    .byte (g_54 - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (g_57 - 80b) / 4
    .byte (g_58 - 80b) / 4
    .byte (g_59 - 80b) / 4
    .byte (g_60 - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (g_63 - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (80b - 80b) / 4
    .byte (g_66 - 80b) / 4
    .byte (g_67 - 80b) / 4
    .byte (g_68 - 80b) / 4
    .byte (80b - 80b) / 4
caller_tbl:
    .byte (71b - 70b) / 4
    .byte (73b - 70b) / 4
    .byte (c_4 - 70b) / 4
    .byte (70b - 70b) / 4
    .byte (72b - 70b) / 4
    .byte (73b - 70b) / 4
    .byte (c_8 - 70b) / 4
    .byte (71b - 70b) / 4
    .byte (71b - 70b) / 4
    .byte (70b - 70b) / 4
    .byte (72b - 70b) / 4
    .byte (71b - 70b) / 4
    .byte (71b - 70b) / 4
    .byte (c_15 - 70b) / 4
    .byte (71b - 70b) / 4
    .byte (c_17 - 70b) / 4
    .byte (71b - 70b) / 4
    .byte (74b - 70b) / 4
    .byte (72b - 70b) / 4
    .byte (72b - 70b) / 4
    .byte (70b - 70b) / 4
    .byte (72b - 70b) / 4
    .byte (72b - 70b) / 4
    .byte (72b - 70b) / 4
    .byte (c_26 - 70b) / 4
    .byte (71b - 70b) / 4
    .byte (c_28 - 70b) / 4
    .byte (73b - 70b) / 4
    .byte (72b - 70b) / 4
    .byte (71b - 70b) / 4
    .byte (c_32 - 70b) / 4
    .byte (c_33 - 70b) / 4
    .byte (c_34 - 70b) / 4
    .byte (c_35 - 70b) / 4
    .byte (c_36 - 70b) / 4
    .byte (71b - 70b) / 4
    .byte (71b - 70b) / 4
    .byte (71b - 70b) / 4
    .byte (70b - 70b) / 4
    .byte (71b - 70b) / 4
    .byte (c_42 - 70b) / 4
    .byte (72b - 70b) / 4
    .byte (70b - 70b) / 4
    .byte (72b - 70b) / 4
    .byte (71b - 70b) / 4
    .byte (72b - 70b) / 4
    .byte (70b - 70b) / 4
    .byte (73b - 70b) / 4
    .byte (70b - 70b) / 4
