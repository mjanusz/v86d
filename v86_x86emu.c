#include <stdarg.h>
#include <string.h>
#include "v86.h"
#include "x86emu.h"

#define X86_EAX M.x86.R_EAX
#define X86_EBX M.x86.R_EBX
#define X86_ECX M.x86.R_ECX
#define X86_EDX M.x86.R_EDX
#define X86_ESI M.x86.R_ESI
#define X86_EDI M.x86.R_EDI
#define X86_EBP M.x86.R_EBP
#define X86_EIP M.x86.R_EIP
#define X86_ESP M.x86.R_ESP
#define X86_EFLAGS M.x86.R_EFLG

#define X86_FLAGS M.x86.R_FLG
#define X86_AX M.x86.R_AX
#define X86_BX M.x86.R_BX
#define X86_CX M.x86.R_CX
#define X86_DX M.x86.R_DX
#define X86_SI M.x86.R_SI
#define X86_DI M.x86.R_DI
#define X86_BP M.x86.R_BP
#define X86_IP M.x86.R_IP
#define X86_SP M.x86.R_SP
#define X86_CS M.x86.R_CS
#define X86_DS M.x86.R_DS
#define X86_ES M.x86.R_ES
#define X86_SS M.x86.R_SS
#define X86_FS M.x86.R_FS
#define X86_GS M.x86.R_GS

#define X86_AL M.x86.R_AL
#define X86_BL M.x86.R_BL
#define X86_CL M.x86.R_CL
#define X86_DL M.x86.R_DL

#define X86_AH M.x86.R_AH
#define X86_BH M.x86.R_BH
#define X86_CH M.x86.R_CH
#define X86_DH M.x86.R_DH

#define X86_TF_MASK		0x00000100
#define X86_IF_MASK		0x00000200
#define X86_IOPL_MASK		0x00003000
#define X86_NT_MASK		0x00004000
#define X86_VM_MASK		0x00020000
#define X86_AC_MASK		0x00040000
#define X86_VIF_MASK		0x00080000	/* virtual interrupt flag */
#define X86_VIP_MASK		0x00100000	/* virtual interrupt pending */
#define X86_ID_MASK		0x00200000

#define DEFAULT_V86_FLAGS  (X86_IF_MASK | X86_IOPL_MASK)

#define addr(t) (((t & 0xffff0000) >> 12) + (t & 0x0000ffff))

u8 *stack;
u8 *halt;

#define __BUILDIO(bwl,bw,type)									\
static void x_out ## bwl (u16 port, type value) {				\
	/*printf("out" #bwl " %x, %x\n", port, value);	*/			\
	__asm__ __volatile__("out" #bwl " %" #bw "0, %w1"			\
			: : "a"(value), "Nd"(port));						\
}																\
																\
static type x_in ## bwl (u16 port) {							\
	type value;													\
	__asm__ __volatile__("in" #bwl " %w1, %" #bw "0"			\
			: "=a"(value)										\
			: "Nd"(port));										\
	/*printf("in" #bwl " %x = %x\n", port, value);	*/			\
	return value;												\
}

__BUILDIO(b,b,u8);
__BUILDIO(w,w,u16);
__BUILDIO(l,,u32);

void printk(const char *fmt, ...)
{
	va_list argptr;
	va_start(argptr, fmt);
	vprintf(fmt, argptr);
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

void rconv_vm86_to_x86emu(struct vm86_regs *rs)
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

void rconv_x86emu_to_vm86(struct vm86_regs *rd)
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
int v86_int(int num, struct vm86_regs *regs)
{
	int err;

	rconv_vm86_to_x86emu(regs);

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

	rconv_x86emu_to_vm86(regs);
	return 0;
}

#define vbeib_get_string(name)			\
{										\
	t = addr(ib->name);					\
	ulog("%x %x\n", t, bufend);			\
	if (t < bufend) {					\
		ib->name = t - (u32)lbuf;		\
	} else {							\
		ib->name = 0;					\
	}									\
}

int v86_task(struct uvesafb_task *tsk, u8 *buf)
{
	u8 *lbuf;

	/* Get the VBE Info Block */
	if (tsk->flags & TF_VBEIB) {
		struct vbe_ib *ib;
		u32 t, bufend;

		lbuf = v86_mem_alloc(tsk->buf_len);
		memcpy(lbuf, buf, tsk->buf_len);
		tsk->regs.es  = (u32)lbuf >> 4;
		tsk->regs.edi = 0x0000;

		if (v86_int(0x10, &tsk->regs) || (tsk->regs.eax & 0xffff) != 0x004f)
			goto out_vbeib;

		ib = (struct vbe_ib*)buf;
		bufend = (u32)(lbuf + sizeof(struct vbe_ib));
		memcpy(buf, lbuf, tsk->buf_len);

		vbeib_get_string(oem_string_ptr);
		vbeib_get_string(oem_vendor_name_ptr);
		vbeib_get_string(oem_product_name_ptr);
		vbeib_get_string(oem_product_rev_ptr);
		vbeib_get_string(mode_list_ptr);
out_vbeib:
		v86_mem_free(lbuf);
	} else {
		if (tsk->buf_len) {
			lbuf = v86_mem_alloc(tsk->buf_len);
			memcpy(lbuf, buf, tsk->buf_len);
		}

		if (tsk->flags & TF_BUF_ESDI) {
			tsk->regs.es = (u32)lbuf >> 4;
			tsk->regs.edi = 0x0000;
		}

		if (tsk->flags & TF_BUF_ESBX) {
			tsk->regs.es = (u32)lbuf >> 4;
			tsk->regs.ebx = 0x0000;
		}

		if (v86_int(0x10, &tsk->regs) || (tsk->regs.eax & 0xffff) != 0x004f)
			goto out;

		if (tsk->buf_len && tsk->flags & TF_BUF_RET) {
			memcpy(buf, lbuf, tsk->buf_len);
		}
out:
		if (tsk->buf_len)
			v86_mem_free(lbuf);
	}

	return 0;
}

