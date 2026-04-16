_Z15rw_read_lock_fnR7rw_lock:           // @_Z15rw_read_lock_fnR7rw_lock
// %bb.0:
	stp	x29, x30, [sp, #-32]!           // 16-byte Folded Spill
	stp	x20, x19, [sp, #16]             // 16-byte Folded Spill
	mov	x29, sp
	mov	x19, x0
	b	.LBB8_2
	//APP
	yield
	//NO_APP
	ldr	w20, [x19]
	tbnz	w20, #31, .LBB8_1
// %bb.3:                               //   in Loop: Header=BB8_2 Depth=1
	add	w1, w20, #1
	mov	w0, w20
	mov	x2, x19
	bl	__aarch64_cas4_acq
	cmp	w0, w20
	b.ne	.LBB8_2
// %bb.4:
	ldp	x20, x19, [sp, #16]             // 16-byte Folded Reload
	ldp	x29, x30, [sp], #32             // 16-byte Folded Reload
	ret
                                        // -- End function
