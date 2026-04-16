_Z14ticket_lock_fnR11ticket_lock:       // @_Z14ticket_lock_fnR11ticket_lock
// %bb.0:
	stp	x29, x30, [sp, #-32]!           // 16-byte Folded Spill
	str	x19, [sp, #16]                  // 8-byte Folded Spill
	mov	x29, sp
	mov	x19, x0
	mov	w0, #1
	mov	x1, x19
	bl	__aarch64_ldadd4_relax
	add	x8, x19, #64
	ldar	w8, [x8]
	cmp	w8, w0
	b.eq	.LBB6_2
	add	x8, x19, #64
	//APP
	yield
	//NO_APP
	ldar	w8, [x8]
	cmp	w8, w0
	b.ne	.LBB6_1
	ldr	x19, [sp, #16]                  // 8-byte Folded Reload
	ldp	x29, x30, [sp], #32             // 16-byte Folded Reload
	ret
                                        // -- End function
