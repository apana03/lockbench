_Z20occ_read_validate_fnRK8occ_lockm:   # @_Z20occ_read_validate_fnRK8occ_lockm
# %bb.0:
	#MEMBARRIER
	movq	(%rdi), %rax
	cmpq	%rsi, %rax
	sete	%al
	retq
                                        # -- End function
