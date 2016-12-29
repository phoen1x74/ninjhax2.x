.section ".init"
.arm
.align 4
.global _start
.type _start, %function
.global _main
.type _main, %function
.global _heap_size
.global _heap_base
.global _appCodeAddress
.global _tidLow
.global _tidHigh
.global _mediatype

b _start

_appCodeAddress:
	.word 0x0
_tidLow:
	.word 0x0
_tidHigh:
	.word 0x0
_mediatype:
	.word 0x0

_start:
	@ reset stack
	mov sp, #0x10000000

	@ allocate bss/heap
	@ no need to initialize as OS does that already.
	@ need to save registers because _runBootloader takes parameters

		@ MEMOP COMMIT
		ldr r0, =0x3
		@ addr0
		mov r1, #0x08000000
		@ addr1
		mov r2, #0
		@ size
		ldr r3, =_heap_size
		ldr r3, [r3]
		@ RW permissions
		mov r4, #3

		@ svcControlMemory
		svc 0x01

		@ save heap address
		mov r1, #0x08000000
		ldr r2, =_heap_base
		str r1, [r2]

	b _main

.global svc_queryMemory
.type svc_queryMemory, %function
svc_queryMemory:
	push {r0, r1, r4-r6}
	svc  0x02
	ldr  r6, [sp]
	str  r1, [r6]
	str  r2, [r6, #4]
	str  r3, [r6, #8]
	str  r4, [r6, #0xc]
	ldr  r6, [sp, #4]
	str  r5, [r6]
	add  sp, sp, #8
	pop  {r4-r6}
	bx   lr

.global svc_duplicateHandle
.type svc_duplicateHandle, %function
svc_duplicateHandle:
	str r0, [sp, #-0x4]!
	svc 0x27
	ldr r3, [sp], #4
	str r1, [r3]
	bx  lr

.global svc_getResourceLimit
.type svc_getResourceLimit, %function
svc_getResourceLimit:
	str r0, [sp, #-4]!
	svc 0x38
	ldr r2, [sp], #4
	str r1, [r2]
	bx lr

.global svc_getResourceLimitLimitValues
.type svc_getResourceLimitLimitValues, %function
svc_getResourceLimitLimitValues:
	svc 0x39
	bx lr

.global svc_getResourceLimitCurrentValue
.type svc_getResourceLimitCurrentValue, %function
svc_getResourceLimitCurrentValue:
	svc 0x3a
	bx lr
