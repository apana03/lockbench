_Z16ticket_unlock_fnR11ticket_lock:     // @_Z16ticket_unlock_fnR11ticket_lock
// %bb.0:
	ldr	w9, [x0, #64]
	add	x8, x0, #64
	add	w9, w9, #1
	stlr	w9, [x8]
	ret
                                        // -- End function
