#include "v86.h"

#define addr(t) (((t & 0xffff0000) >> 12) + (t & 0x0000ffff))

#define vbeib_get_string(name)					\
{												\
	int l;										\
	t = addr(ib->name);							\
	if (t < bufend) {							\
		ib->name = t - (u32)lbuf;				\
	} else if (t > 0xa0000 && fsize > 0) {		\
		strncpy((char*)buf, (char*)t, fsize);	\
		ib->name = tsk->buf_len - fsize;		\
		l = strlen((char*)t);					\
		fsize -= l;								\
		buf += l;								\
		if (fsize < 0)							\
			fsize = 0;							\
	} else {									\
		ib->name = 0;							\
	}											\
}

int v86_task(struct uvesafb_task *tsk, u8 *buf)
{
	u8 *lbuf = NULL;

	/* Get the VBE Info Block */
	if (tsk->flags & TF_VBEIB) {
		struct vbe_ib *ib;
		int fsize;
		u32 t, bufend;
		u16 *ts, *td;

		lbuf = v86_mem_alloc(tsk->buf_len);
		memcpy(lbuf, buf, tsk->buf_len);
		tsk->regs.es  = (u32)lbuf >> 4;
		tsk->regs.edi = 0x0000;

		if (v86_int(0x10, &tsk->regs) || (tsk->regs.eax & 0xffff) != 0x004f)
			goto out_vbeib;

		ib = (struct vbe_ib*)buf;
		bufend = (u32)(lbuf + sizeof(*ib));
		memcpy(buf, lbuf, tsk->buf_len);

		/* The original VBE Info Block is 512 bytes long. */
		fsize = tsk->buf_len - 512;

		t = addr(ib->mode_list_ptr);
		/* Mode list is in the buffer, we're good. */
		if (t < bufend) {
			ib->mode_list_ptr = t - (u32)lbuf;

		/* Mode list is in the ROM. We copy as much of it as we can
		 * to the task buffer. */
		} else if (t > 0xa0000) {
			ts = (u16*) t;
			td = (u16*) (buf + 512);

			while (fsize > 2 && *ts != 0xffff) {
				fsize -= 2;
				*td = *ts;
				ts++;
				td++;
			}

			ib->mode_list_ptr = 512;
			*td = 0xffff;

		/* Mode list is somewhere else. We're seriously screwed. */
		} else {
			ulog("Can't retrieve mode list from %x\n", t);
			ib->mode_list_ptr = 0;
		}

		buf += 512;

		vbeib_get_string(oem_string_ptr);
		vbeib_get_string(oem_vendor_name_ptr);
		vbeib_get_string(oem_product_name_ptr);
		vbeib_get_string(oem_product_rev_ptr);
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

