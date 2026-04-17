_Z16rw_write_lock_fnR7rw_lock:          # @_Z16rw_write_lock_fnR7rw_lock
# %bb.0:
	movl	$-1, %ecx
	xorl	%eax, %eax
	lock		cmpxchgl	%ecx, (%rdi)
	je	.LBB10_3
	pause
	xorl	%eax, %eax
	lock		cmpxchgl	%ecx, (%rdi)
	jne	.LBB10_1
	retq
                                        # -- End function
