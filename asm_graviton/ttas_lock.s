_Z12ttas_lock_fnR9ttas_lock:            // @_Z12ttas_lock_fnR9ttas_lock
// %bb.0:
	stp	x29, x30, [sp, #-32]!           // 16-byte Folded Spill
	str	x19, [sp, #16]                  // 8-byte Folded Spill
	mov	x29, sp
	mov	x19, x0
	b	.LBB2_2
	//APP
	yield
	//NO_APP
	ldrb	w8, [x19]
	tbnz	w8, #0, .LBB2_1
// %bb.3:                               //   in Loop: Header=BB2_2 Depth=1
	mov	w0, #1
	mov	x1, x19
	bl	__aarch64_swp1_acq
	tbnz	w0, #0, .LBB2_2
// %bb.4:
	ldr	x19, [sp, #16]                  // 8-byte Folded Reload
	ldp	x29, x30, [sp], #32             // 16-byte Folded Reload
	ret
                                        // -- End function
