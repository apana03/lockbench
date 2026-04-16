_Z16rw_write_lock_fnR7rw_lock:          // @_Z16rw_write_lock_fnR7rw_lock
// %bb.0:
	stp	x29, x30, [sp, #-32]!           // 16-byte Folded Spill
	str	x19, [sp, #16]                  // 8-byte Folded Spill
	mov	x29, sp
	mov	x19, x0
	mov	w0, wzr
	mov	w1, #-1
	mov	x2, x19
	bl	__aarch64_cas4_acq
	cmp	w0, #0
	b.eq	.LBB10_2
	//APP
	yield
	//NO_APP
	mov	w0, wzr
	mov	w1, #-1
	mov	x2, x19
	bl	__aarch64_cas4_acq
	cmp	w0, #0
	b.ne	.LBB10_1
	ldr	x19, [sp, #16]                  // 8-byte Folded Reload
	ldp	x29, x30, [sp], #32             // 16-byte Folded Reload
	ret
                                        // -- End function
