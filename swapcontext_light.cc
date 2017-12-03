#include <ucontext.h>
#include <stddef.h>

extern "C" int swapcontext_light(ucontext_t *, ucontext_t *);

// clang's built-in asm cannot handle .set directives
#if defined(__GNUC__) && !defined(__llvm__) && defined(__x86_64) && defined(_LP64)

#define R(r) offsetof(ucontext_t, uc_mcontext.gregs[r])

static __attribute__((used))
void context_stuff_declaration(ucontext_t *ctx) {
	__asm__ __volatile__(".set oRBX, %c0\n"
			     ".set oRBP, %c1\n"
			     ".set oR12, %c2\n"
			     ".set oR13, %c3\n"
			     ".set oR14, %c4\n"
			     ".set oR15, %c5\n"
			     ".set oRIP, %c6\n"
			     ".set oRSP, %c7\n"
			     : :
			       "p" (R(REG_RBX)),
			       "p" (R(REG_RBP)),
			       "p" (R(REG_R12)),
			       "p" (R(REG_R13)),
			       "p" (R(REG_R14)),
			       "p" (R(REG_R15)),
			       "p" (R(REG_RIP)),
			       "p" (R(REG_RSP)));
	swapcontext_light(ctx, nullptr);
}

__asm__(".pushsection .text; .globl swapcontext_light\n"
	".type swapcontext_light, @function\n"
"swapcontext_light:\n"
	"\t.cfi_startproc\n"
	"\tmovq     %rbx, oRBX(%rdi)\n"
	"\tmovq     %rbp, oRBP(%rdi)\n"
	"\tmovq     %r12, oR12(%rdi)\n"
	"\tmovq     %r13, oR13(%rdi)\n"
	"\tmovq     %r14, oR14(%rdi)\n"
	"\tmovq     %r15, oR15(%rdi)\n"

	"\tmovq     (%rsp), %rcx\n"
	"\tmovq     %rcx, oRIP(%rdi)\n"
	"\tleaq     8(%rsp), %rcx\n"                /* Exclude the return address.  */
	"\tmovq     %rcx, oRSP(%rdi)\n"

	// to have decent and still simple unwind info we first 'push'
	// all registers into target stack, then switch stack, then
	// pop everything
	"\tmovq     oRSP(%rsi), %rdx\n"
	"\tsubq     $56, %rdx\n"

	"\tmovq     oRIP(%rsi), %rcx\n"
	"\tmovq     %rcx, 48(%rdx)\n"

#define P(reg, offset) \
	"\tmovq     oR" #reg "(%rsi), %rcx\n" \
	"\tmovq     %rcx, " #offset "(%rdx)\n"

	P(BP, 40)
	P(BX, 32)
	P(12, 24)
	P(13, 16)
	P(14, 8)
	P(15, 0)

#undef P

	"\tmovq     %rdx, %rsp\n"
	"\t.cfi_def_cfa_offset 56\n"
	"\t.cfi_offset 15, -56\n"
	"\t.cfi_offset 14, -48\n"
	"\t.cfi_offset 13, -40\n"
	"\t.cfi_offset 12, -32\n"
	"\t.cfi_offset 3, -24\n"
	"\t.cfi_offset 6, -16\n"

	"\tpopq %r15\n"
	"\t.cfi_def_cfa_offset 48\n"
	"\tpopq %r14\n"
	"\t.cfi_def_cfa_offset 40\n"
	"\tpopq %r13\n"
	"\t.cfi_def_cfa_offset 32\n"
	"\tpopq %r12\n"
	"\t.cfi_def_cfa_offset 24\n"
	"\tpopq %rbx\n"
	"\t.cfi_def_cfa_offset 16\n"
	"\tpopq %rbp\n"
	"\t.cfi_def_cfa_offset 8\n"
	"\txorl %eax, %eax\n"
	"\tret\n"

	".cfi_endproc\n"
	".size swapcontext_light, .-swapcontext_light\n"
	".popsection\n"
	);

#else

extern "C" int swapcontext_light(ucontext_t *ctx, ucontext_t *ctx2) {
	return swapcontext(ctx, ctx2);
}

#endif // ! gcc & x86-64
