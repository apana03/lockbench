_Z11tas_lock_fnR8tas_lock:              // @_Z11tas_lock_fnR8tas_lock
// %bb.0:
	stp	x29, x30, [sp, #-32]!           // 16-byte Folded Spill
	str	x19, [sp, #16]                  // 8-byte Folded Spill
	mov	x29, sp
	mov	x19, x0
	mov	w0, #1
	mov	x1, x19
	bl	__aarch64_swp1_acq
	cbz	w0, .LBB0_2
	//APP
	yield
	//NO_APP
	mov	w0, #1
	mov	x1, x19
	bl	__aarch64_swp1_acq
	cbnz	w0, .LBB0_1
	ldr	x19, [sp, #16]                  // 8-byte Folded Reload
	ldp	x29, x30, [sp], #32             // 16-byte Folded Reload
	ret
                                        // -- End function
