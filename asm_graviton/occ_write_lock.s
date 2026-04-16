_Z17occ_write_lock_fnR8occ_lock:        // @_Z17occ_write_lock_fnR8occ_lock
// %bb.0:
	stp	x29, x30, [sp, #-32]!           // 16-byte Folded Spill
	stp	x20, x19, [sp, #16]             // 16-byte Folded Spill
	mov	x29, sp
	mov	x19, x0
	b	.LBB12_2
	//APP
	yield
	//NO_APP
	ldr	x20, [x19]
	tbnz	w20, #0, .LBB12_1
// %bb.3:                               //   in Loop: Header=BB12_2 Depth=1
	add	x1, x20, #1
	mov	x0, x20
	mov	x2, x19
	bl	__aarch64_cas8_acq
	cmp	x0, x20
	b.ne	.LBB12_2
// %bb.4:
	ldp	x20, x19, [sp, #16]             // 16-byte Folded Reload
	ldp	x29, x30, [sp], #32             // 16-byte Folded Reload
	ret
                                        // -- End function
