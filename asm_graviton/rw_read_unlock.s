_Z17rw_read_unlock_fnR7rw_lock:         // @_Z17rw_read_unlock_fnR7rw_lock
// %bb.0:
	stp	x29, x30, [sp, #-16]!           // 16-byte Folded Spill
	mov	x29, sp
	mov	x1, x0
	mov	w0, #-1
	bl	__aarch64_ldadd4_rel
	ldp	x29, x30, [sp], #16             // 16-byte Folded Reload
	ret
                                        // -- End function
