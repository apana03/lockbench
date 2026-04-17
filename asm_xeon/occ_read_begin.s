_Z17occ_read_begin_fnRK8occ_lock:       # @_Z17occ_read_begin_fnRK8occ_lock
# %bb.0:
	movq	(%rdi), %rax
	testb	$1, %al
	je	.LBB14_3
	pause
	movq	(%rdi), %rax
	testb	$1, %al
	jne	.LBB14_1
	retq
                                        # -- End function
