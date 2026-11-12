/* arm64 Linux "Image" header so the MediaTek bootloader accepts us as a kernel */
.section .head, "ax"
.globl _start
_start:
    b       entry                 /* code0 */
    .long   0                     /* code1 */
    .quad   0                     /* text_offset */
    .quad   _end - _start         /* image_size */
    .quad   0xa                   /* flags: LE, 4K pages, anywhere */
    .quad   0, 0, 0
    .ascii  "ARM\x64"             /* magic */
    .long   0
entry:
    mov     x19, x0               /* x0 = device tree from bootloader */
    msr     daifset, #0xf         /* mask interrupts */
    adr     x1, stack_top
    mov     sp, x1
    adr     x1, __bss_start
    adr     x2, __bss_end
1:  cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b
2:  mov     x0, x19
    bl      shim_main
3:  wfe
    b       3b

.section .bss
.balign 16
    .space 0x4000
stack_top:
