_Z17occ_write_lock_fnR8occ_lock:        # @_Z17occ_write_lock_fnR8occ_lock
# %bb.0:
	jmp	.LBB12_1
	pause
	movq	(%rdi), %rax
	testb	$1, %al
	jne	.LBB12_2
# %bb.3:                                #   in Loop: Header=BB12_1 Depth=1
	leaq	1(%rax), %rcx
	lock		cmpxchgq	%rcx, (%rdi)
	jne	.LBB12_1
# %bb.4:
	retq
                                        # -- End function
