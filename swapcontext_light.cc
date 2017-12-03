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

// FIXME: this has totally screwed up unwind info (same holds for
// glibc's swapcontext)
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

	"\tmovq     oRSP(%rsi), %rdx\n"
	"\tsubq     $8, %rdx\n"
	"\tmovq     oRIP(%rsi), %rcx\n"
	"\tmovq     %rcx, (%rdx)\n"

	"\tmovq     oRBX(%rsi), %rbx\n"
	"\tmovq     oRBP(%rsi), %rbp\n"
	"\tmovq     oR12(%rsi), %r12\n"
	"\tmovq     oR13(%rsi), %r13\n"
	"\tmovq     oR14(%rsi), %r14\n"
	"\tmovq     oR15(%rsi), %r15\n"
	"\tmovq     %rdx, %rsp\n"

	"\txorl     %eax, %eax\n"
	"\tret\n"
	".cfi_endproc\n"
	".size swapcontext_light, .-swapcontext_light\n"
	".popsection\n"
	);

#else

extern "C" void swapcontext_light(ucontext_t *ctx, ucontext_t *ctx2) {
  swapcontext(ctx, ctx2);
}

#endif // ! gcc & x86-64
