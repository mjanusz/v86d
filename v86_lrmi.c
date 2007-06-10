#include <string.h>
#include <lrmi.h>
#include "v86.h"

void rconv_vm86_to_LRMI(struct vm86_regs *rs, struct LRMI_regs *rd)
{
	memset(rd, 0, sizeof(*rd));

	rd->eax = rs->eax;
	rd->ebx = rs->ebx;
	rd->ecx = rs->ecx;
	rd->edx = rs->edx;
	rd->edi = rs->edi;
	rd->esi = rs->esi;
	rd->ebp = rs->ebp;
	rd->sp  = rs->esp;
	rd->flags = rs->eflags;
	rd->ip  = rs->eip;
	rd->cs  = rs->cs;
	rd->ds  = rs->ds;
	rd->es  = rs->es;
	rd->fs  = rs->fs;
	rd->gs  = rs->gs;
}

void rconv_LRMI_to_vm86(struct LRMI_regs *rs, struct vm86_regs *rd)
{
	rd->eax = rs->eax;
	rd->ebx = rs->ebx;
	rd->ecx = rs->ecx;
	rd->edx = rs->edx;
	rd->edi = rs->edi;
	rd->esi = rs->esi;
	rd->ebp = rs->ebp;
	rd->esp = rs->sp;
	rd->eflags = rs->flags;
	rd->eip = rs->ip;
	rd->cs  = rs->cs;
	rd->ds  = rs->ds;
	rd->es  = rs->es;
	rd->fs  = rs->fs;
	rd->gs  = rs->gs;
}

int v86_init() {
	int err = LRMI_init();

	ioperm(0, 1024, 1);
	iopl(3);

	return (err == 1) ? 0 : 1;
}

void v86_cleanup()
{
	/* dummy function */
}

/*
 * Perform a simulated interrupt call.
 */
int v86_int(int num, struct vm86_regs *regs)
{
	struct LRMI_regs r;
	int err;

	rconv_vm86_to_LRMI(regs, &r);
	err = LRMI_int(num, &r);
	rconv_LRMI_to_vm86(&r, regs);

	return (err == 1) ? 0 : 1;
}

inline void v86_mem_free(void *m) {
	LRMI_free_real(m);
}

inline void *v86_mem_alloc(int size) {
	return LRMI_alloc_real(size);
}
