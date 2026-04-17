_Z11tas_lock_fnR8tas_lock:              # @_Z11tas_lock_fnR8tas_lock
# %bb.0:
	movb	$1, %al
	xchgb	%al, (%rdi)
	testb	%al, %al
	je	.LBB0_3
	pause
	movb	$1, %al
	xchgb	%al, (%rdi)
	testb	%al, %al
	jne	.LBB0_1
	retq
                                        # -- End function
