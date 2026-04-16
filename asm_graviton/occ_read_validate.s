_Z20occ_read_validate_fnRK8occ_lockm:   // @_Z20occ_read_validate_fnRK8occ_lockm
// %bb.0:
	dmb	ishld
	ldar	x8, [x0]
	cmp	x8, x1
	cset	w0, eq
	ret
                                        // -- End function
