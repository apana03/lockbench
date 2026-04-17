	.text
	.file	"lock_asm.cpp"
	.globl	_Z11tas_lock_fnR8tas_lock       # -- Begin function _Z11tas_lock_fnR8tas_lock
	.p2align	4, 0x90
	.type	_Z11tas_lock_fnR8tas_lock,@function
_Z11tas_lock_fnR8tas_lock:              # @_Z11tas_lock_fnR8tas_lock
	.cfi_startproc
# %bb.0:
	movb	$1, %al
	xchgb	%al, (%rdi)
	testb	%al, %al
	je	.LBB0_3
	.p2align	4, 0x90
.LBB0_1:                                # =>This Inner Loop Header: Depth=1
	pause
	movb	$1, %al
	xchgb	%al, (%rdi)
	testb	%al, %al
	jne	.LBB0_1
.LBB0_3:
	retq
.Lfunc_end0:
	.size	_Z11tas_lock_fnR8tas_lock, .Lfunc_end0-_Z11tas_lock_fnR8tas_lock
	.cfi_endproc
                                        # -- End function
	.globl	_Z13tas_unlock_fnR8tas_lock     # -- Begin function _Z13tas_unlock_fnR8tas_lock
	.p2align	4, 0x90
	.type	_Z13tas_unlock_fnR8tas_lock,@function
_Z13tas_unlock_fnR8tas_lock:            # @_Z13tas_unlock_fnR8tas_lock
	.cfi_startproc
# %bb.0:
	movb	$0, (%rdi)
	retq
.Lfunc_end1:
	.size	_Z13tas_unlock_fnR8tas_lock, .Lfunc_end1-_Z13tas_unlock_fnR8tas_lock
	.cfi_endproc
                                        # -- End function
	.globl	_Z12ttas_lock_fnR9ttas_lock     # -- Begin function _Z12ttas_lock_fnR9ttas_lock
	.p2align	4, 0x90
	.type	_Z12ttas_lock_fnR9ttas_lock,@function
_Z12ttas_lock_fnR9ttas_lock:            # @_Z12ttas_lock_fnR9ttas_lock
	.cfi_startproc
# %bb.0:
	jmp	.LBB2_2
	.p2align	4, 0x90
.LBB2_1:                                #   in Loop: Header=BB2_2 Depth=1
	pause
.LBB2_2:                                # =>This Inner Loop Header: Depth=1
	movzbl	(%rdi), %eax
	testb	$1, %al
	jne	.LBB2_1
# %bb.3:                                #   in Loop: Header=BB2_2 Depth=1
	movb	$1, %al
	xchgb	%al, (%rdi)
	testb	$1, %al
	jne	.LBB2_2
# %bb.4:
	retq
.Lfunc_end2:
	.size	_Z12ttas_lock_fnR9ttas_lock, .Lfunc_end2-_Z12ttas_lock_fnR9ttas_lock
	.cfi_endproc
                                        # -- End function
	.globl	_Z14ttas_unlock_fnR9ttas_lock   # -- Begin function _Z14ttas_unlock_fnR9ttas_lock
	.p2align	4, 0x90
	.type	_Z14ttas_unlock_fnR9ttas_lock,@function
_Z14ttas_unlock_fnR9ttas_lock:          # @_Z14ttas_unlock_fnR9ttas_lock
	.cfi_startproc
# %bb.0:
	movb	$0, (%rdi)
	retq
.Lfunc_end3:
	.size	_Z14ttas_unlock_fnR9ttas_lock, .Lfunc_end3-_Z14ttas_unlock_fnR9ttas_lock
	.cfi_endproc
                                        # -- End function
	.globl	_Z11cas_lock_fnR8cas_lock       # -- Begin function _Z11cas_lock_fnR8cas_lock
	.p2align	4, 0x90
	.type	_Z11cas_lock_fnR8cas_lock,@function
_Z11cas_lock_fnR8cas_lock:              # @_Z11cas_lock_fnR8cas_lock
	.cfi_startproc
# %bb.0:
	movb	$1, %cl
	.p2align	4, 0x90
.LBB4_1:                                # =>This Loop Header: Depth=1
                                        #     Child Loop BB4_3 Depth 2
	xorl	%eax, %eax
	lock		cmpxchgb	%cl, (%rdi)
	je	.LBB4_4
	.p2align	4, 0x90
# %bb.2:                                #   in Loop: Header=BB4_1 Depth=1
	movb	(%rdi), %al
	testb	$1, %al
	je	.LBB4_1
.LBB4_3:                                #   Parent Loop BB4_1 Depth=1
                                        # =>  This Inner Loop Header: Depth=2
	pause
	movzbl	(%rdi), %eax
	testb	$1, %al
	jne	.LBB4_3
	jmp	.LBB4_1
.LBB4_4:
	retq
.Lfunc_end4:
	.size	_Z11cas_lock_fnR8cas_lock, .Lfunc_end4-_Z11cas_lock_fnR8cas_lock
	.cfi_endproc
                                        # -- End function
	.globl	_Z13cas_unlock_fnR8cas_lock     # -- Begin function _Z13cas_unlock_fnR8cas_lock
	.p2align	4, 0x90
	.type	_Z13cas_unlock_fnR8cas_lock,@function
_Z13cas_unlock_fnR8cas_lock:            # @_Z13cas_unlock_fnR8cas_lock
	.cfi_startproc
# %bb.0:
	movb	$0, (%rdi)
	retq
.Lfunc_end5:
	.size	_Z13cas_unlock_fnR8cas_lock, .Lfunc_end5-_Z13cas_unlock_fnR8cas_lock
	.cfi_endproc
                                        # -- End function
	.globl	_Z14ticket_lock_fnR11ticket_lock # -- Begin function _Z14ticket_lock_fnR11ticket_lock
	.p2align	4, 0x90
	.type	_Z14ticket_lock_fnR11ticket_lock,@function
_Z14ticket_lock_fnR11ticket_lock:       # @_Z14ticket_lock_fnR11ticket_lock
	.cfi_startproc
# %bb.0:
	movl	$1, %eax
	lock		xaddl	%eax, (%rdi)
	movl	64(%rdi), %ecx
	cmpl	%eax, %ecx
	je	.LBB6_3
	.p2align	4, 0x90
.LBB6_1:                                # =>This Inner Loop Header: Depth=1
	pause
	movl	64(%rdi), %ecx
	cmpl	%eax, %ecx
	jne	.LBB6_1
.LBB6_3:
	retq
.Lfunc_end6:
	.size	_Z14ticket_lock_fnR11ticket_lock, .Lfunc_end6-_Z14ticket_lock_fnR11ticket_lock
	.cfi_endproc
                                        # -- End function
	.globl	_Z16ticket_unlock_fnR11ticket_lock # -- Begin function _Z16ticket_unlock_fnR11ticket_lock
	.p2align	4, 0x90
	.type	_Z16ticket_unlock_fnR11ticket_lock,@function
_Z16ticket_unlock_fnR11ticket_lock:     # @_Z16ticket_unlock_fnR11ticket_lock
	.cfi_startproc
# %bb.0:
	incl	64(%rdi)
	retq
.Lfunc_end7:
	.size	_Z16ticket_unlock_fnR11ticket_lock, .Lfunc_end7-_Z16ticket_unlock_fnR11ticket_lock
	.cfi_endproc
                                        # -- End function
	.globl	_Z15rw_read_lock_fnR7rw_lock    # -- Begin function _Z15rw_read_lock_fnR7rw_lock
	.p2align	4, 0x90
	.type	_Z15rw_read_lock_fnR7rw_lock,@function
_Z15rw_read_lock_fnR7rw_lock:           # @_Z15rw_read_lock_fnR7rw_lock
	.cfi_startproc
# %bb.0:
	jmp	.LBB8_1
	.p2align	4, 0x90
.LBB8_4:                                #   in Loop: Header=BB8_1 Depth=1
	pause
.LBB8_1:                                # =>This Inner Loop Header: Depth=1
	movl	(%rdi), %eax
	testl	%eax, %eax
	js	.LBB8_4
# %bb.2:                                #   in Loop: Header=BB8_1 Depth=1
	leal	1(%rax), %ecx
                                        # kill: def $eax killed $eax killed $rax
	lock		cmpxchgl	%ecx, (%rdi)
	jne	.LBB8_1
# %bb.3:
	retq
.Lfunc_end8:
	.size	_Z15rw_read_lock_fnR7rw_lock, .Lfunc_end8-_Z15rw_read_lock_fnR7rw_lock
	.cfi_endproc
                                        # -- End function
	.globl	_Z17rw_read_unlock_fnR7rw_lock  # -- Begin function _Z17rw_read_unlock_fnR7rw_lock
	.p2align	4, 0x90
	.type	_Z17rw_read_unlock_fnR7rw_lock,@function
_Z17rw_read_unlock_fnR7rw_lock:         # @_Z17rw_read_unlock_fnR7rw_lock
	.cfi_startproc
# %bb.0:
	lock		decl	(%rdi)
	retq
.Lfunc_end9:
	.size	_Z17rw_read_unlock_fnR7rw_lock, .Lfunc_end9-_Z17rw_read_unlock_fnR7rw_lock
	.cfi_endproc
                                        # -- End function
	.globl	_Z16rw_write_lock_fnR7rw_lock   # -- Begin function _Z16rw_write_lock_fnR7rw_lock
	.p2align	4, 0x90
	.type	_Z16rw_write_lock_fnR7rw_lock,@function
_Z16rw_write_lock_fnR7rw_lock:          # @_Z16rw_write_lock_fnR7rw_lock
	.cfi_startproc
# %bb.0:
	movl	$-1, %ecx
	xorl	%eax, %eax
	lock		cmpxchgl	%ecx, (%rdi)
	je	.LBB10_3
	.p2align	4, 0x90
.LBB10_1:                               # =>This Inner Loop Header: Depth=1
	pause
	xorl	%eax, %eax
	lock		cmpxchgl	%ecx, (%rdi)
	jne	.LBB10_1
.LBB10_3:
	retq
.Lfunc_end10:
	.size	_Z16rw_write_lock_fnR7rw_lock, .Lfunc_end10-_Z16rw_write_lock_fnR7rw_lock
	.cfi_endproc
                                        # -- End function
	.globl	_Z18rw_write_unlock_fnR7rw_lock # -- Begin function _Z18rw_write_unlock_fnR7rw_lock
	.p2align	4, 0x90
	.type	_Z18rw_write_unlock_fnR7rw_lock,@function
_Z18rw_write_unlock_fnR7rw_lock:        # @_Z18rw_write_unlock_fnR7rw_lock
	.cfi_startproc
# %bb.0:
	movl	$0, (%rdi)
	retq
.Lfunc_end11:
	.size	_Z18rw_write_unlock_fnR7rw_lock, .Lfunc_end11-_Z18rw_write_unlock_fnR7rw_lock
	.cfi_endproc
                                        # -- End function
	.globl	_Z17occ_write_lock_fnR8occ_lock # -- Begin function _Z17occ_write_lock_fnR8occ_lock
	.p2align	4, 0x90
	.type	_Z17occ_write_lock_fnR8occ_lock,@function
_Z17occ_write_lock_fnR8occ_lock:        # @_Z17occ_write_lock_fnR8occ_lock
	.cfi_startproc
# %bb.0:
	jmp	.LBB12_1
	.p2align	4, 0x90
.LBB12_2:                               #   in Loop: Header=BB12_1 Depth=1
	pause
.LBB12_1:                               # =>This Inner Loop Header: Depth=1
	movq	(%rdi), %rax
	testb	$1, %al
	jne	.LBB12_2
# %bb.3:                                #   in Loop: Header=BB12_1 Depth=1
	leaq	1(%rax), %rcx
	lock		cmpxchgq	%rcx, (%rdi)
	jne	.LBB12_1
# %bb.4:
	retq
.Lfunc_end12:
	.size	_Z17occ_write_lock_fnR8occ_lock, .Lfunc_end12-_Z17occ_write_lock_fnR8occ_lock
	.cfi_endproc
                                        # -- End function
	.globl	_Z19occ_write_unlock_fnR8occ_lock # -- Begin function _Z19occ_write_unlock_fnR8occ_lock
	.p2align	4, 0x90
	.type	_Z19occ_write_unlock_fnR8occ_lock,@function
_Z19occ_write_unlock_fnR8occ_lock:      # @_Z19occ_write_unlock_fnR8occ_lock
	.cfi_startproc
# %bb.0:
	lock		incq	(%rdi)
	retq
.Lfunc_end13:
	.size	_Z19occ_write_unlock_fnR8occ_lock, .Lfunc_end13-_Z19occ_write_unlock_fnR8occ_lock
	.cfi_endproc
                                        # -- End function
	.globl	_Z17occ_read_begin_fnRK8occ_lock # -- Begin function _Z17occ_read_begin_fnRK8occ_lock
	.p2align	4, 0x90
	.type	_Z17occ_read_begin_fnRK8occ_lock,@function
_Z17occ_read_begin_fnRK8occ_lock:       # @_Z17occ_read_begin_fnRK8occ_lock
	.cfi_startproc
# %bb.0:
	movq	(%rdi), %rax
	testb	$1, %al
	je	.LBB14_3
	.p2align	4, 0x90
.LBB14_1:                               # =>This Inner Loop Header: Depth=1
	pause
	movq	(%rdi), %rax
	testb	$1, %al
	jne	.LBB14_1
.LBB14_3:
	retq
.Lfunc_end14:
	.size	_Z17occ_read_begin_fnRK8occ_lock, .Lfunc_end14-_Z17occ_read_begin_fnRK8occ_lock
	.cfi_endproc
                                        # -- End function
	.globl	_Z20occ_read_validate_fnRK8occ_lockm # -- Begin function _Z20occ_read_validate_fnRK8occ_lockm
	.p2align	4, 0x90
	.type	_Z20occ_read_validate_fnRK8occ_lockm,@function
_Z20occ_read_validate_fnRK8occ_lockm:   # @_Z20occ_read_validate_fnRK8occ_lockm
	.cfi_startproc
# %bb.0:
	#MEMBARRIER
	movq	(%rdi), %rax
	cmpq	%rsi, %rax
	sete	%al
	retq
.Lfunc_end15:
	.size	_Z20occ_read_validate_fnRK8occ_lockm, .Lfunc_end15-_Z20occ_read_validate_fnRK8occ_lockm
	.cfi_endproc
                                        # -- End function
	.section	".linker-options","e",@llvm_linker_options
	.ident	"Ubuntu clang version 14.0.0-1ubuntu1.1"
	.section	".note.GNU-stack","",@progbits
	.addrsig
	.addrsig_sym __gxx_personality_v0
