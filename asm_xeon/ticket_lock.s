_Z14ticket_lock_fnR11ticket_lock:       # @_Z14ticket_lock_fnR11ticket_lock
# %bb.0:
	movl	$1, %eax
	lock		xaddl	%eax, (%rdi)
	movl	64(%rdi), %ecx
	cmpl	%eax, %ecx
	je	.LBB6_3
	pause
	movl	64(%rdi), %ecx
	cmpl	%eax, %ecx
	jne	.LBB6_1
	retq
                                        # -- End function
