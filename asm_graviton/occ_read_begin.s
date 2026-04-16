_Z17occ_read_begin_fnRK8occ_lock:       // @_Z17occ_read_begin_fnRK8occ_lock
// %bb.0:
	mov	x8, x0
	ldar	x0, [x0]
	tbz	w0, #0, .LBB14_2
	//APP
	yield
	//NO_APP
	ldar	x0, [x8]
	tbnz	w0, #0, .LBB14_1
	ret
                                        // -- End function
