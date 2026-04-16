_Z11cas_lock_fnR8cas_lock:              // @_Z11cas_lock_fnR8cas_lock
// %bb.0:
	stp	x29, x30, [sp, #-32]!           // 16-byte Folded Spill
	str	x19, [sp, #16]                  // 8-byte Folded Spill
	mov	x29, sp
	mov	x19, x0
                                        //     Child Loop BB4_3 Depth 2
	mov	w0, wzr
	mov	w1, #1
	mov	x2, x19
	bl	__aarch64_cas1_acq
	cmp	w0, #0
	b.eq	.LBB4_4
// %bb.2:                               //   in Loop: Header=BB4_1 Depth=1
	ldrb	w8, [x19]
	tbz	w8, #0, .LBB4_1
                                        // =>  This Inner Loop Header: Depth=2
	//APP
	yield
	//NO_APP
	ldrb	w8, [x19]
	tbnz	w8, #0, .LBB4_3
	b	.LBB4_1
	ldr	x19, [sp, #16]                  // 8-byte Folded Reload
	ldp	x29, x30, [sp], #32             // 16-byte Folded Reload
	ret
                                        // -- End function
