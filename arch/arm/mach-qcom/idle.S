#include <linux/linkage.h>
#include <linux/threads.h>
#include <asm/assembler.h>

ENTRY(msm_arch_idle)
	mrc 	p15, 0, r0, c0, c0, 0
	bic	r1, r0, #0xff
	movw	r2, #0x0400
	movt	r2, #0x511F
	movw	r3, #0x0600
	movt	r3, #0x510F
	cmp	r2, r1
	cmpne	r3, r1
	bne	go_wfi

	mrs	r0, cpsr
	cpsid	if

	mrc	p15, 7, r1, c15, c0, 5
	bic	r2, r1, #0x20000
	mcr	p15, 7, r2, c15, c0, 5
	isb

go_wfi:
	wfi
	bne	wfi_done
	mcr	p15, 7, r1, c15, c0, 5
	isb
	msr	cpsr_c, r0

wfi_done:
	bx	lr
