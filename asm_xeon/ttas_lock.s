_Z12ttas_lock_fnR9ttas_lock:            # @_Z12ttas_lock_fnR9ttas_lock
# %bb.0:
	jmp	.LBB2_2
	pause
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
                                        # -- End function
