_Z19occ_write_unlock_fnR8occ_lock:      // @_Z19occ_write_unlock_fnR8occ_lock
// %bb.0:
	stp	x29, x30, [sp, #-16]!           // 16-byte Folded Spill
	mov	x29, sp
	mov	x1, x0
	mov	w0, #1
	bl	__aarch64_ldadd8_rel
	ldp	x29, x30, [sp], #16             // 16-byte Folded Reload
	ret
                                        // -- End function
