#include <stdarg.h>
#include <string.h>
#include <x86emu.h>
/*
  This is header file for x86_64 is broken in current versions
  of klibc, so we temporarily comment it out.
  #include <sys/io.h>
*/
#include "v86.h"
#include "v86_x86emu.h"

u8 *stack;
u8 *halt;

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
	wrw(((u32) X86_SS << 4) + X86_SP, val);
}

static void x86emu_do_int(int num)
{
	u32 eflags;

	eflags = X86_EFLAGS;
	eflags = eflags | X86_IF_MASK;

	/* Return address and flags */
	pushw(eflags);
	pushw(X86_CS);
	pushw(X86_IP);
	X86_EFLAGS = X86_EFLAGS & ~(X86_VIF_MASK | X86_TF_MASK);

	X86_CS = rdw((num << 2) + 2);
	X86_IP = rdw((num << 2));
}

int v86_init()
{
	X86EMU_intrFuncs intFuncs[256];
	X86EMU_pioFuncs pioFuncs = {
		.inb = x_inb,
		.inw = x_inw,
		.inl = x_inl,
		.outb = x_outb,
		.outw = x_outw,
		.outl = x_outl,
	};
	int i;

	v86_mem_init();

	stack = v86_mem_alloc(DEFAULT_STACK_SIZE);
	X86_SS = (u32)stack >> 4;
	X86_ESP = 0xfffe;

	halt = v86_mem_alloc(0x100);
	*halt = 0xF4;

	/* Setup x86emu I/O functions */
	X86EMU_setupPioFuncs(&pioFuncs);

	/* Setup interrupt handlers */
	for (i = 0; i < 256; i++) {
		intFuncs[i] = x86emu_do_int;
	}
	X86EMU_setupIntrFuncs(intFuncs);

	/* Set the default flags */
	X86_EFLAGS = X86_IF_MASK | X86_IOPL_MASK;

	/* M is a macro poiting to the global virtual machine state
	 * for x86emu. */
	M.mem_base = 0x0;
	M.mem_size = 0x100000;

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

	X86_DS = 0x0040;
	X86_CS  = get_int_seg(num);
	X86_EIP = get_int_off(num);
	X86_SS = (u32)stack >> 4;
	X86_ESP = 0xffff;
	X86_EFLAGS = DEFAULT_V86_FLAGS | X86_IF_MASK;
	X86_EFLAGS &= ~(X86_VIF_MASK | X86_TF_MASK | X86_IF_MASK | X86_NT_MASK);

	pushw(DEFAULT_V86_FLAGS);
	pushw(((u32)halt >> 4));
	pushw(0x0);

	X86EMU_exec();

	rconv_x86emu_to_v86(regs);
	return 0;
}

