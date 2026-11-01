/*
 * Galaxy A8 (2018, jackpotlte) diagnostic shim entry.
 * S-Boot loads us like a Linux kernel: arm64 Image header, EL1, MMU/caches
 * off, x0 = FDT physical address. Position independent, no storage access.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0x80000			/* text_offset: same as the stock kernel, so S-Boot places us where it places stock */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 */

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* FDT pointer from S-Boot */
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	adr	x1, _bss_start		/* zero .bss */
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	mov	x0, x19
	bl	a8_main
3:	wfe
	b	3b

/* Every vector prints slot/ESR/ELR/FAR on screen and halts. */
.macro vent slot
	.balign	0x80
	mov	x0, #\slot
	mrs	x1, esr_el1
	mrs	x2, elr_el1
	mrs	x3, far_el1
	bl	a8_exc
	b	.
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
