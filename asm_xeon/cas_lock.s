_Z11cas_lock_fnR8cas_lock:              # @_Z11cas_lock_fnR8cas_lock
# %bb.0:
	movb	$1, %cl
                                        #     Child Loop BB4_3 Depth 2
	xorl	%eax, %eax
	lock		cmpxchgb	%cl, (%rdi)
	je	.LBB4_4
# %bb.2:                                #   in Loop: Header=BB4_1 Depth=1
	movb	(%rdi), %al
	testb	$1, %al
	je	.LBB4_1
                                        # =>  This Inner Loop Header: Depth=2
	pause
	movzbl	(%rdi), %eax
	testb	$1, %al
	jne	.LBB4_3
	jmp	.LBB4_1
	retq
                                        # -- End function
