#include <stdarg.h>
#include <string.h>
#include <x86emu.h>
#include "v86.h"
#include "v86_x86emu.h"

u32 stack;
u32 halt;

__BUILDIO(b,b,u8);
__BUILDIO(w,w,u16);
__BUILDIO(l,,u32);

void printk(const char *fmt, ...)
{
	va_list argptr;
	va_start(argptr, fmt);
	vsyslog(LOG_INFO, fmt, argptr);
	va_end(argptr);
}

void pushw(u16 val)
{
	X86_ESP -= 2;
	v_wrw(((u32) X86_SS << 4) + X86_SP, val);
}

static void x86emu_do_int(int num)
{
	u32 eflags;

	eflags = X86_EFLAGS;

	/* Return address and flags */
	pushw(eflags);
	pushw(X86_CS);
	pushw(X86_IP);

	X86_EFLAGS = X86_EFLAGS & ~(X86_VIF_MASK | X86_TF_MASK);
	X86_CS = v_rdw((num << 2) + 2);
	X86_IP = v_rdw((num << 2));
}

int v86_init()
{
	X86EMU_intrFuncs intFuncs[256];
	X86EMU_pioFuncs pioFuncs = {
		.inb = &x_inb,
		.inw = &x_inw,
		.inl = &x_inl,
		.outb = &x_outb,
		.outw = &x_outw,
		.outl = &x_outl,
	};

	X86EMU_memFuncs memFuncs = {
		.rdb = &v_rdb,
		.rdw = &v_rdw,
		.rdl = &v_rdl,
		.wrb = &v_wrb,
		.wrw = &v_wrw,
		.wrl = &v_wrl,
	};

	int i;

	if (v86_mem_init()) {
		ulog(LOG_ERR, "v86 memory initialization failed.");
		return -1;
	}

	stack = v86_mem_alloc(DEFAULT_STACK_SIZE);
	if (!stack) {
		ulog(LOG_ERR, "v86 memory allocation failed.");
		return -1;
	}

	X86_SS = stack >> 4;
	X86_ESP = DEFAULT_STACK_SIZE;

	halt = v86_mem_alloc(0x100);
	if (!halt) {
		ulog(LOG_ERR, "v86 memory alocation failed.");
		return -1;
	}
	v_wrb(halt, 0xF4);

	X86EMU_setupPioFuncs(&pioFuncs);
	X86EMU_setupMemFuncs(&memFuncs);

	/* Setup interrupt handlers */
	for (i = 0; i < 256; i++) {
		intFuncs[i] = x86emu_do_int;
	}
	X86EMU_setupIntrFuncs(intFuncs);

	/* Set the default flags */
	X86_EFLAGS = X86_IF_MASK | X86_IOPL_MASK;

	ioperm(0, 1024, 1);
	iopl(3);

	return 0;
}

void v86_cleanup()
{
	v86_mem_cleanup();
}

void rconv_v86_to_x86emu(struct v86_regs *rs)
{
	X86_EAX = rs->eax;
	X86_EBX = rs->ebx;
	X86_ECX = rs->ecx;
	X86_EDX = rs->edx;
	X86_EDI = rs->edi;
	X86_ESI = rs->esi;
	X86_EBP = rs->ebp;
	X86_ESP = rs->esp;
	X86_EFLAGS = rs->eflags;
	X86_EIP = rs->eip;
	X86_CS  = rs->cs;
	X86_DS  = rs->ds;
	X86_ES  = rs->es;
	X86_FS  = rs->fs;
	X86_GS  = rs->gs;
}

void rconv_x86emu_to_v86(struct v86_regs *rd)
{
	rd->eax = X86_EAX;
	rd->ebx = X86_EBX;
	rd->ecx = X86_ECX;
	rd->edx = X86_EDX;
	rd->edi = X86_EDI;
	rd->esi = X86_ESI;
	rd->ebp = X86_EBP;
	rd->esp = X86_ESP;
	rd->eflags = X86_EFLAGS;
	rd->eip = X86_EIP;
	rd->cs  = X86_CS;
	rd->ds  = X86_DS;
	rd->es  = X86_ES;
	rd->fs  = X86_FS;
	rd->gs  = X86_GS;
}

/*
 * Perform a simulated interrupt call.
 */
int v86_int(int num, struct v86_regs *regs)
{
	rconv_v86_to_x86emu(regs);

	X86_GS = 0;
	X86_FS = 0;
	X86_DS = 0x0040;
	X86_CS  = v_rdw((num << 2) + 2);
	X86_EIP = v_rdw((num << 2));
	X86_SS = stack >> 4;
	X86_ESP = DEFAULT_STACK_SIZE;
	X86_EFLAGS = X86_IF_MASK | X86_IOPL_MASK;

	pushw(X86_EFLAGS);
	pushw((halt >> 4));
	pushw(0x0);

	X86EMU_exec();

	rconv_x86emu_to_v86(regs);
	return 0;
}

void v86_dump_regs()
{
	ulog(LOG_DEBUG,
		"EAX=0x%8.8lx, EBX=0x%8.8lx, ECX=0x%8.8lx, EDX=0x%8.8lx\n",
		(unsigned long)X86_EAX, (unsigned long)X86_EBX,
		(unsigned long)X86_ECX, (unsigned long)X86_EDX);
	ulog(LOG_DEBUG,
		"ESP=0x%8.8lx, EBP=0x%8.8lx, ESI=0x%8.8lx, EDI=0x%8.8lx\n",
		(unsigned long)X86_ESP, (unsigned long)X86_EBP,
		(unsigned long)X86_ESI, (unsigned long)X86_EDI);
    ulog(LOG_DEBUG,
		"CS=0x%4.4x, SS=0x%4.4x,"
		" DS=0x%4.4x, ES=0x%4.4x, FS=0x%4.4x, GS=0x%4.4x\n",
		X86_CS, X86_SS, X86_DS, X86_ES, X86_FS, X86_GS);
    ulog(LOG_DEBUG,
		"EIP=0x%8.8lx, EFLAGS=0x%8.8lx\n",
		(unsigned long)X86_EIP, (unsigned long)X86_EFLAGS);
}

