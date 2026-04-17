_Z15rw_read_lock_fnR7rw_lock:           # @_Z15rw_read_lock_fnR7rw_lock
# %bb.0:
	jmp	.LBB8_1
	pause
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
                                        # -- End function
