	.text
	.file	"lock_asm.cpp"
	.globl	_Z11tas_lock_fnR8tas_lock       // -- Begin function _Z11tas_lock_fnR8tas_lock
	.p2align	2
	.type	_Z11tas_lock_fnR8tas_lock,@function
_Z11tas_lock_fnR8tas_lock:              // @_Z11tas_lock_fnR8tas_lock
	.cfi_startproc
// %bb.0:
	stp	x29, x30, [sp, #-32]!           // 16-byte Folded Spill
	.cfi_def_cfa_offset 32
	str	x19, [sp, #16]                  // 8-byte Folded Spill
	mov	x29, sp
	.cfi_def_cfa w29, 32
	.cfi_offset w19, -16
	.cfi_offset w30, -24
	.cfi_offset w29, -32
	mov	x19, x0
	mov	w0, #1
	mov	x1, x19
	bl	__aarch64_swp1_acq
	cbz	w0, .LBB0_2
.LBB0_1:                                // =>This Inner Loop Header: Depth=1
	//APP
	yield
	//NO_APP
	mov	w0, #1
	mov	x1, x19
	bl	__aarch64_swp1_acq
	cbnz	w0, .LBB0_1
.LBB0_2:
	.cfi_def_cfa wsp, 32
	ldr	x19, [sp, #16]                  // 8-byte Folded Reload
	ldp	x29, x30, [sp], #32             // 16-byte Folded Reload
	.cfi_def_cfa_offset 0
	.cfi_restore w19
	.cfi_restore w30
	.cfi_restore w29
	ret
.Lfunc_end0:
	.size	_Z11tas_lock_fnR8tas_lock, .Lfunc_end0-_Z11tas_lock_fnR8tas_lock
	.cfi_endproc
                                        // -- End function
	.globl	_Z13tas_unlock_fnR8tas_lock     // -- Begin function _Z13tas_unlock_fnR8tas_lock
	.p2align	2
	.type	_Z13tas_unlock_fnR8tas_lock,@function
_Z13tas_unlock_fnR8tas_lock:            // @_Z13tas_unlock_fnR8tas_lock
	.cfi_startproc
// %bb.0:
	stlrb	wzr, [x0]
	ret
.Lfunc_end1:
	.size	_Z13tas_unlock_fnR8tas_lock, .Lfunc_end1-_Z13tas_unlock_fnR8tas_lock
	.cfi_endproc
                                        // -- End function
	.globl	_Z12ttas_lock_fnR9ttas_lock     // -- Begin function _Z12ttas_lock_fnR9ttas_lock
	.p2align	2
	.type	_Z12ttas_lock_fnR9ttas_lock,@function
_Z12ttas_lock_fnR9ttas_lock:            // @_Z12ttas_lock_fnR9ttas_lock
	.cfi_startproc
// %bb.0:
	stp	x29, x30, [sp, #-32]!           // 16-byte Folded Spill
	.cfi_def_cfa_offset 32
	str	x19, [sp, #16]                  // 8-byte Folded Spill
	mov	x29, sp
	.cfi_def_cfa w29, 32
	.cfi_offset w19, -16
	.cfi_offset w30, -24
	.cfi_offset w29, -32
	mov	x19, x0
	b	.LBB2_2
.LBB2_1:                                //   in Loop: Header=BB2_2 Depth=1
	//APP
	yield
	//NO_APP
.LBB2_2:                                // =>This Inner Loop Header: Depth=1
	ldrb	w8, [x19]
	tbnz	w8, #0, .LBB2_1
// %bb.3:                               //   in Loop: Header=BB2_2 Depth=1
	mov	w0, #1
	mov	x1, x19
	bl	__aarch64_swp1_acq
	tbnz	w0, #0, .LBB2_2
// %bb.4:
	.cfi_def_cfa wsp, 32
	ldr	x19, [sp, #16]                  // 8-byte Folded Reload
	ldp	x29, x30, [sp], #32             // 16-byte Folded Reload
	.cfi_def_cfa_offset 0
	.cfi_restore w19
	.cfi_restore w30
	.cfi_restore w29
	ret
.Lfunc_end2:
	.size	_Z12ttas_lock_fnR9ttas_lock, .Lfunc_end2-_Z12ttas_lock_fnR9ttas_lock
	.cfi_endproc
                                        // -- End function
	.globl	_Z14ttas_unlock_fnR9ttas_lock   // -- Begin function _Z14ttas_unlock_fnR9ttas_lock
	.p2align	2
	.type	_Z14ttas_unlock_fnR9ttas_lock,@function
_Z14ttas_unlock_fnR9ttas_lock:          // @_Z14ttas_unlock_fnR9ttas_lock
	.cfi_startproc
// %bb.0:
	stlrb	wzr, [x0]
	ret
.Lfunc_end3:
	.size	_Z14ttas_unlock_fnR9ttas_lock, .Lfunc_end3-_Z14ttas_unlock_fnR9ttas_lock
	.cfi_endproc
                                        // -- End function
	.globl	_Z11cas_lock_fnR8cas_lock       // -- Begin function _Z11cas_lock_fnR8cas_lock
	.p2align	2
	.type	_Z11cas_lock_fnR8cas_lock,@function
_Z11cas_lock_fnR8cas_lock:              // @_Z11cas_lock_fnR8cas_lock
	.cfi_startproc
// %bb.0:
	stp	x29, x30, [sp, #-32]!           // 16-byte Folded Spill
	.cfi_def_cfa_offset 32
	str	x19, [sp, #16]                  // 8-byte Folded Spill
	mov	x29, sp
	.cfi_def_cfa w29, 32
	.cfi_offset w19, -16
	.cfi_offset w30, -24
	.cfi_offset w29, -32
	mov	x19, x0
.LBB4_1:                                // =>This Loop Header: Depth=1
                                        //     Child Loop BB4_3 Depth 2
	mov	w0, wzr
	mov	w1, #1
	mov	x2, x19
	bl	__aarch64_cas1_acq
	cmp	w0, #0
	b.eq	.LBB4_4
// %bb.2:                               //   in Loop: Header=BB4_1 Depth=1
	ldrb	w8, [x19]
	tbz	w8, #0, .LBB4_1
.LBB4_3:                                //   Parent Loop BB4_1 Depth=1
                                        // =>  This Inner Loop Header: Depth=2
	//APP
	yield
	//NO_APP
	ldrb	w8, [x19]
	tbnz	w8, #0, .LBB4_3
	b	.LBB4_1
.LBB4_4:
	.cfi_def_cfa wsp, 32
	ldr	x19, [sp, #16]                  // 8-byte Folded Reload
	ldp	x29, x30, [sp], #32             // 16-byte Folded Reload
	.cfi_def_cfa_offset 0
	.cfi_restore w19
	.cfi_restore w30
	.cfi_restore w29
	ret
.Lfunc_end4:
	.size	_Z11cas_lock_fnR8cas_lock, .Lfunc_end4-_Z11cas_lock_fnR8cas_lock
	.cfi_endproc
                                        // -- End function
	.globl	_Z13cas_unlock_fnR8cas_lock     // -- Begin function _Z13cas_unlock_fnR8cas_lock
	.p2align	2
	.type	_Z13cas_unlock_fnR8cas_lock,@function
_Z13cas_unlock_fnR8cas_lock:            // @_Z13cas_unlock_fnR8cas_lock
	.cfi_startproc
// %bb.0:
	stlrb	wzr, [x0]
	ret
.Lfunc_end5:
	.size	_Z13cas_unlock_fnR8cas_lock, .Lfunc_end5-_Z13cas_unlock_fnR8cas_lock
	.cfi_endproc
                                        // -- End function
	.globl	_Z14ticket_lock_fnR11ticket_lock // -- Begin function _Z14ticket_lock_fnR11ticket_lock
	.p2align	2
	.type	_Z14ticket_lock_fnR11ticket_lock,@function
_Z14ticket_lock_fnR11ticket_lock:       // @_Z14ticket_lock_fnR11ticket_lock
	.cfi_startproc
// %bb.0:
	stp	x29, x30, [sp, #-32]!           // 16-byte Folded Spill
	.cfi_def_cfa_offset 32
	str	x19, [sp, #16]                  // 8-byte Folded Spill
	mov	x29, sp
	.cfi_def_cfa w29, 32
	.cfi_offset w19, -16
	.cfi_offset w30, -24
	.cfi_offset w29, -32
	mov	x19, x0
	mov	w0, #1
	mov	x1, x19
	bl	__aarch64_ldadd4_relax
	add	x8, x19, #64
	ldar	w8, [x8]
	cmp	w8, w0
	b.eq	.LBB6_2
.LBB6_1:                                // =>This Inner Loop Header: Depth=1
	add	x8, x19, #64
	//APP
	yield
	//NO_APP
	ldar	w8, [x8]
	cmp	w8, w0
	b.ne	.LBB6_1
.LBB6_2:
	.cfi_def_cfa wsp, 32
	ldr	x19, [sp, #16]                  // 8-byte Folded Reload
	ldp	x29, x30, [sp], #32             // 16-byte Folded Reload
	.cfi_def_cfa_offset 0
	.cfi_restore w19
	.cfi_restore w30
	.cfi_restore w29
	ret
.Lfunc_end6:
	.size	_Z14ticket_lock_fnR11ticket_lock, .Lfunc_end6-_Z14ticket_lock_fnR11ticket_lock
	.cfi_endproc
                                        // -- End function
	.globl	_Z16ticket_unlock_fnR11ticket_lock // -- Begin function _Z16ticket_unlock_fnR11ticket_lock
	.p2align	2
	.type	_Z16ticket_unlock_fnR11ticket_lock,@function
_Z16ticket_unlock_fnR11ticket_lock:     // @_Z16ticket_unlock_fnR11ticket_lock
	.cfi_startproc
// %bb.0:
	ldr	w9, [x0, #64]
	add	x8, x0, #64
	add	w9, w9, #1
	stlr	w9, [x8]
	ret
.Lfunc_end7:
	.size	_Z16ticket_unlock_fnR11ticket_lock, .Lfunc_end7-_Z16ticket_unlock_fnR11ticket_lock
	.cfi_endproc
                                        // -- End function
	.globl	_Z15rw_read_lock_fnR7rw_lock    // -- Begin function _Z15rw_read_lock_fnR7rw_lock
	.p2align	2
	.type	_Z15rw_read_lock_fnR7rw_lock,@function
_Z15rw_read_lock_fnR7rw_lock:           // @_Z15rw_read_lock_fnR7rw_lock
	.cfi_startproc
// %bb.0:
	stp	x29, x30, [sp, #-32]!           // 16-byte Folded Spill
	.cfi_def_cfa_offset 32
	stp	x20, x19, [sp, #16]             // 16-byte Folded Spill
	mov	x29, sp
	.cfi_def_cfa w29, 32
	.cfi_offset w19, -8
	.cfi_offset w20, -16
	.cfi_offset w30, -24
	.cfi_offset w29, -32
	mov	x19, x0
	b	.LBB8_2
.LBB8_1:                                //   in Loop: Header=BB8_2 Depth=1
	//APP
	yield
	//NO_APP
.LBB8_2:                                // =>This Inner Loop Header: Depth=1
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
	.cfi_def_cfa wsp, 32
	ldp	x20, x19, [sp, #16]             // 16-byte Folded Reload
	ldp	x29, x30, [sp], #32             // 16-byte Folded Reload
	.cfi_def_cfa_offset 0
	.cfi_restore w19
	.cfi_restore w20
	.cfi_restore w30
	.cfi_restore w29
	ret
.Lfunc_end8:
	.size	_Z15rw_read_lock_fnR7rw_lock, .Lfunc_end8-_Z15rw_read_lock_fnR7rw_lock
	.cfi_endproc
                                        // -- End function
	.globl	_Z17rw_read_unlock_fnR7rw_lock  // -- Begin function _Z17rw_read_unlock_fnR7rw_lock
	.p2align	2
	.type	_Z17rw_read_unlock_fnR7rw_lock,@function
_Z17rw_read_unlock_fnR7rw_lock:         // @_Z17rw_read_unlock_fnR7rw_lock
	.cfi_startproc
// %bb.0:
	stp	x29, x30, [sp, #-16]!           // 16-byte Folded Spill
	.cfi_def_cfa_offset 16
	mov	x29, sp
	.cfi_def_cfa w29, 16
	.cfi_offset w30, -8
	.cfi_offset w29, -16
	mov	x1, x0
	mov	w0, #-1
	bl	__aarch64_ldadd4_rel
	.cfi_def_cfa wsp, 16
	ldp	x29, x30, [sp], #16             // 16-byte Folded Reload
	.cfi_def_cfa_offset 0
	.cfi_restore w30
	.cfi_restore w29
	ret
.Lfunc_end9:
	.size	_Z17rw_read_unlock_fnR7rw_lock, .Lfunc_end9-_Z17rw_read_unlock_fnR7rw_lock
	.cfi_endproc
                                        // -- End function
	.globl	_Z16rw_write_lock_fnR7rw_lock   // -- Begin function _Z16rw_write_lock_fnR7rw_lock
	.p2align	2
	.type	_Z16rw_write_lock_fnR7rw_lock,@function
_Z16rw_write_lock_fnR7rw_lock:          // @_Z16rw_write_lock_fnR7rw_lock
	.cfi_startproc
// %bb.0:
	stp	x29, x30, [sp, #-32]!           // 16-byte Folded Spill
	.cfi_def_cfa_offset 32
	str	x19, [sp, #16]                  // 8-byte Folded Spill
	mov	x29, sp
	.cfi_def_cfa w29, 32
	.cfi_offset w19, -16
	.cfi_offset w30, -24
	.cfi_offset w29, -32
	mov	x19, x0
	mov	w0, wzr
	mov	w1, #-1
	mov	x2, x19
	bl	__aarch64_cas4_acq
	cmp	w0, #0
	b.eq	.LBB10_2
.LBB10_1:                               // =>This Inner Loop Header: Depth=1
	//APP
	yield
	//NO_APP
	mov	w0, wzr
	mov	w1, #-1
	mov	x2, x19
	bl	__aarch64_cas4_acq
	cmp	w0, #0
	b.ne	.LBB10_1
.LBB10_2:
	.cfi_def_cfa wsp, 32
	ldr	x19, [sp, #16]                  // 8-byte Folded Reload
	ldp	x29, x30, [sp], #32             // 16-byte Folded Reload
	.cfi_def_cfa_offset 0
	.cfi_restore w19
	.cfi_restore w30
	.cfi_restore w29
	ret
.Lfunc_end10:
	.size	_Z16rw_write_lock_fnR7rw_lock, .Lfunc_end10-_Z16rw_write_lock_fnR7rw_lock
	.cfi_endproc
                                        // -- End function
	.globl	_Z18rw_write_unlock_fnR7rw_lock // -- Begin function _Z18rw_write_unlock_fnR7rw_lock
	.p2align	2
	.type	_Z18rw_write_unlock_fnR7rw_lock,@function
_Z18rw_write_unlock_fnR7rw_lock:        // @_Z18rw_write_unlock_fnR7rw_lock
	.cfi_startproc
// %bb.0:
	stlr	wzr, [x0]
	ret
.Lfunc_end11:
	.size	_Z18rw_write_unlock_fnR7rw_lock, .Lfunc_end11-_Z18rw_write_unlock_fnR7rw_lock
	.cfi_endproc
                                        // -- End function
	.globl	_Z17occ_write_lock_fnR8occ_lock // -- Begin function _Z17occ_write_lock_fnR8occ_lock
	.p2align	2
	.type	_Z17occ_write_lock_fnR8occ_lock,@function
_Z17occ_write_lock_fnR8occ_lock:        // @_Z17occ_write_lock_fnR8occ_lock
	.cfi_startproc
// %bb.0:
	stp	x29, x30, [sp, #-32]!           // 16-byte Folded Spill
	.cfi_def_cfa_offset 32
	stp	x20, x19, [sp, #16]             // 16-byte Folded Spill
	mov	x29, sp
	.cfi_def_cfa w29, 32
	.cfi_offset w19, -8
	.cfi_offset w20, -16
	.cfi_offset w30, -24
	.cfi_offset w29, -32
	mov	x19, x0
	b	.LBB12_2
.LBB12_1:                               //   in Loop: Header=BB12_2 Depth=1
	//APP
	yield
	//NO_APP
.LBB12_2:                               // =>This Inner Loop Header: Depth=1
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
	.cfi_def_cfa wsp, 32
	ldp	x20, x19, [sp, #16]             // 16-byte Folded Reload
	ldp	x29, x30, [sp], #32             // 16-byte Folded Reload
	.cfi_def_cfa_offset 0
	.cfi_restore w19
	.cfi_restore w20
	.cfi_restore w30
	.cfi_restore w29
	ret
.Lfunc_end12:
	.size	_Z17occ_write_lock_fnR8occ_lock, .Lfunc_end12-_Z17occ_write_lock_fnR8occ_lock
	.cfi_endproc
                                        // -- End function
	.globl	_Z19occ_write_unlock_fnR8occ_lock // -- Begin function _Z19occ_write_unlock_fnR8occ_lock
	.p2align	2
	.type	_Z19occ_write_unlock_fnR8occ_lock,@function
_Z19occ_write_unlock_fnR8occ_lock:      // @_Z19occ_write_unlock_fnR8occ_lock
	.cfi_startproc
// %bb.0:
	stp	x29, x30, [sp, #-16]!           // 16-byte Folded Spill
	.cfi_def_cfa_offset 16
	mov	x29, sp
	.cfi_def_cfa w29, 16
	.cfi_offset w30, -8
	.cfi_offset w29, -16
	mov	x1, x0
	mov	w0, #1
	bl	__aarch64_ldadd8_rel
	.cfi_def_cfa wsp, 16
	ldp	x29, x30, [sp], #16             // 16-byte Folded Reload
	.cfi_def_cfa_offset 0
	.cfi_restore w30
	.cfi_restore w29
	ret
.Lfunc_end13:
	.size	_Z19occ_write_unlock_fnR8occ_lock, .Lfunc_end13-_Z19occ_write_unlock_fnR8occ_lock
	.cfi_endproc
                                        // -- End function
	.globl	_Z17occ_read_begin_fnRK8occ_lock // -- Begin function _Z17occ_read_begin_fnRK8occ_lock
	.p2align	2
	.type	_Z17occ_read_begin_fnRK8occ_lock,@function
_Z17occ_read_begin_fnRK8occ_lock:       // @_Z17occ_read_begin_fnRK8occ_lock
	.cfi_startproc
// %bb.0:
	mov	x8, x0
	ldar	x0, [x0]
	tbz	w0, #0, .LBB14_2
.LBB14_1:                               // =>This Inner Loop Header: Depth=1
	//APP
	yield
	//NO_APP
	ldar	x0, [x8]
	tbnz	w0, #0, .LBB14_1
.LBB14_2:
	ret
.Lfunc_end14:
	.size	_Z17occ_read_begin_fnRK8occ_lock, .Lfunc_end14-_Z17occ_read_begin_fnRK8occ_lock
	.cfi_endproc
                                        // -- End function
	.globl	_Z20occ_read_validate_fnRK8occ_lockm // -- Begin function _Z20occ_read_validate_fnRK8occ_lockm
	.p2align	2
	.type	_Z20occ_read_validate_fnRK8occ_lockm,@function
_Z20occ_read_validate_fnRK8occ_lockm:   // @_Z20occ_read_validate_fnRK8occ_lockm
	.cfi_startproc
// %bb.0:
	dmb	ishld
	ldar	x8, [x0]
	cmp	x8, x1
	cset	w0, eq
	ret
.Lfunc_end15:
	.size	_Z20occ_read_validate_fnRK8occ_lockm, .Lfunc_end15-_Z20occ_read_validate_fnRK8occ_lockm
	.cfi_endproc
                                        // -- End function
	.section	".linker-options","e",@llvm_linker_options
	.ident	"clang version 15.0.7 (AWS 15.0.7-3.amzn2023.0.4)"
	.section	".note.GNU-stack","",@progbits
	.addrsig
	.addrsig_sym __gxx_personality_v0
