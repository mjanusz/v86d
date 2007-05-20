#include <string.h>
#include <lrmi.h>
#include "v86.h"

#define addr(t) (((t & 0xffff0000) >> 12) + (t & 0x0000ffff))

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

		lbuf = LRMI_alloc_real(tsk->buf_len);
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
		LRMI_free_real(lbuf);
	} else {
		if (tsk->buf_len) {
			lbuf = LRMI_alloc_real(tsk->buf_len);
			memcpy(lbuf, buf, tsk->buf_len);
		}

		if (tsk->flags & TF_BUF_ESDI) {
			tsk->regs.es = (u32)lbuf >> 4;
			tsk->regs.edi = 0x0000;
		}

		if (v86_int(0x10, &tsk->regs) || (tsk->regs.eax & 0xffff) != 0x004f)
			goto out;

		if (tsk->buf_len && tsk->flags & TF_BUF_RET) {
			memcpy(buf, lbuf, tsk->buf_len);
		}
out:
		if (tsk->buf_len)
			LRMI_free_real(lbuf);
	}

	return 0;
}

