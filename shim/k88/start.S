/*
 * ZTE K88 (MSM8952) diagnostic shim entry.
 * The Qualcomm LK bootloader (aboot) loads us like a Linux kernel (gzip'd arm64
 * Image + appended DTB, tools/mkqcomboot.py): EL1, MMU/caches off, x0 = FDT.
 * Position independent, no storage access.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0x80000			/* text_offset: same as the stock kernel */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 */

/*
 * adrp + :lo12: (+-4 GB): with an XNU payload linked into .rodata, .bss and
 * the stacks are ~17 MB away, out of adr's +-1 MB range. Needs a 4 KiB
 * aligned load address, which the Image header (text_offset) guarantees.
 */
.macro adrl reg, sym
	adrp	\reg, \sym
	add	\reg, \reg, :lo12:\sym
.endm

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* FDT pointer from LK */
	msr	spsel, #1
	adrl	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	adrl	x1, _bss_start		/* zero .bss */
	adrl	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	mov	x0, x19
	bl	k88_main
3:	wfe
	b	3b

/*
 * PSCI CPU_ON entry for the other cores. No context id is relied on: the
 * stack slot comes from MPIDR: Aff1 * 4 + Aff0
 * (K88: 0x0-0x3 -> 0-3, 0x100-0x103 -> 4-7).
 */
	.globl secondary_entry
secondary_entry:
	msr	daifset, #0xf
	msr	spsel, #1
	mrs	x0, mpidr_el1
	ubfx	x1, x0, #8, #8
	and	x0, x0, #0xff
	add	x0, x0, x1, lsl #2
	adrl	x1, _sec_stacks
	add	x1, x1, x0, lsl #12
	add	x1, x1, #0x1000
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	bl	secondary_main
4:	wfe
	b	4b

/*
 * Exception vectors. Each slot stashes x29/x30, loads its slot number and
 * jumps to the common path, which saves x0-x28 and calls k88_exc(slot, regs).
 * IRQs return; everything else ends on the red screen.
 */
.macro vent slot
	.balign	0x80
	stp	x29, x30, [sp, #-16]!
	mov	x29, #\slot
	b	exc_common
.endm

	.balign	0x800
	.globl vectors
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	sub	sp, sp, #(30 * 8)
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	str	x28, [sp, #224]
	mov	x0, x29
	mov	x1, sp
	bl	k88_exc
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldr	x28, [sp, #224]
	add	sp, sp, #(30 * 8)
	ldp	x29, x30, [sp], #16
	eret
