#include <string.h>
#include <lrmi.h>
#include "v86.h"

/* Memory access functions */
u8 v_rdb(u32 addr) {
	return *(u8*)(addr);
}

u16 v_rdw(u32 addr) {
	return *(u16*)(addr);
}

u32 v_rdl(u32 addr) {
	return *(u32*)(addr);
}

void v_wrb(u32 addr, u8 val) {
	*(u8*)(addr) = val;
}

void v_wrw(u32 addr, u16 val) {
	*(u16*)(addr) = val;
}

void v_wrl(u32 addr, u32 val) {
	*(u32*)(addr) = val;
}

void *vptr(u32 addr) {
	return (u8*)addr;
}

void rconv_v86_to_LRMI(struct v86_regs *rs, struct LRMI_regs *rd)
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

void rconv_LRMI_to_v86(struct LRMI_regs *rs, struct v86_regs *rd)
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
int v86_int(int num, struct v86_regs *regs)
{
	struct LRMI_regs r;
	int err;

	rconv_v86_to_LRMI(regs, &r);
	err = LRMI_int(num, &r);
	rconv_LRMI_to_v86(&r, regs);

	return (err == 1) ? 0 : 1;
}

inline void v86_mem_free(u32 m) {
	LRMI_free_real((void*)m);
}

inline u32 v86_mem_alloc(int size) {
	return (u32)LRMI_alloc_real(size);
}
