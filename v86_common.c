#include "v86.h"

#define addr(t) (((t & 0xffff0000) >> 12) + (t & 0x0000ffff))

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

