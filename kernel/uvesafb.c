#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/skbuff.h>
#include <linux/timer.h>
#include <linux/completion.h>
#include <linux/connector.h>
#include <linux/random.h>

#include "uvesafb.h"

static struct cb_id uvesafb_cn_id = { .idx = CN_IDX_UVESAFB, .val = CN_VAL_UVESAFB };
static struct sock *nls;
static char uvesafb_path[] = "/devel/fbdev/uvesafb/v86d";

#define TASKS_MAX 1024
static struct uvesafb_ktask* uvfb_tasks[TASKS_MAX];

static void uvesafb_cn_callback(void *data)
{
	struct cn_msg *msg = (struct cn_msg *)data;
	struct uvesafb_task *utsk = (struct uvesafb_task *)msg->data;
	struct uvesafb_ktask *tsk;

	printk("%s: %lu: idx=%x, val=%x, seq=%u, ack=%u, len=%d.\n",
	       __func__, jiffies, msg->id.idx, msg->id.val, msg->seq, msg->ack, msg->len);

	if (msg->seq >= TASKS_MAX)
		return;

	tsk = uvfb_tasks[msg->seq];

	if (!tsk || msg->ack != tsk->ack)
		return;

	memcpy(&tsk->t, utsk, sizeof(struct uvesafb_task));

	if (tsk->t.buf_len && tsk->buf)
		memcpy(tsk->buf, ((u8*)utsk) + sizeof(struct uvesafb_task), tsk->t.buf_len);

	complete(tsk->done);
	return;
}

static int uvesafb_helper_start(void)
{
	char *envp[] = {
		"HOME=/",
		"PATH=/sbin:/bin",
		NULL,
	};

	char *argv[] = {
		uvesafb_path,
		NULL,
	};

	return call_usermodehelper(uvesafb_path, argv, envp, 0);
}

static int uvesafb_exec(struct uvesafb_ktask *tsk)
{
	static int seq = 0;
	struct cn_msg *m;
	int err;
	int len = sizeof(struct uvesafb_task) + tsk->t.buf_len;

	m = kmalloc(sizeof(*m) + len, GFP_ATOMIC);
	if (!m)
		return -ENOMEM;

	init_completion(tsk->done);

	memset(m, 0, sizeof(*m) + len);
	memcpy(&m->id, &uvesafb_cn_id, sizeof(m->id));
	m->seq = seq;
	m->len = len;
	m->ack = random32();

	/* uvesafb_task structure */
	memcpy(m + 1, tsk, sizeof(struct uvesafb_task));

	/* buffer */
	memcpy(((u8*)m) + (sizeof(struct uvesafb_task) + sizeof(*m)), tsk->buf, tsk->t.buf_len);

	/* Save the message ack number so that we can find the kernel
	 * part of this task when a reply is received from userspace. */
	tsk->ack = m->ack;
	uvfb_tasks[seq] = tsk;

	err = cn_netlink_send(m, 0, gfp_any());
	if (err == -ESRCH) {
		uvesafb_helper_start();
		err = cn_netlink_send(m, 0, gfp_any());
	}
	kfree(m);

	if (!err)
		wait_for_completion_timeout(tsk->done, msecs_to_jiffies(10000));

	printk("pre-wait-for-completion err: %x\n", err);
	uvfb_tasks[seq] = NULL;

	seq++;
	if (seq >= TASKS_MAX)
		seq = 0;

	printk("done: %x\n", tsk->done->done);

	return (tsk->done->done >= 0) ? 0 : 1;
}

static int __init inline uvesafb_vbe_getinfo(struct uvesafb_ktask *tsk,
	struct uvesafb_par* par)
{
	int err;

	tsk->t.regs.eax = 0x4f00;
	tsk->t.flags = TF_VBEIB;
	tsk->t.buf_len = sizeof(struct vbe_ib);
	tsk->buf = (u8*) &par->vbe_ib;
	strncpy(par->vbe_ib.vbe_signature, "VBE2", 4);

	err = uvesafb_exec(tsk);
	if (err || (tsk->t.regs.eax & 0xffff) != 0x004f) {
		printk(KERN_ERR "uvesafb: Getting VBE info block failed "
				"(eax=0x%x, err=%x)\n", (u32)tsk->t.regs.eax,
				err);
		return -EINVAL;
	}

	if (par->vbe_ib.vbe_version < 0x0200) {
		printk(KERN_ERR "uvesafb: Sorry, pre-VBE 2.0 cards are "
				"not supported.\n");
		return -EINVAL;
	}

	if (!par->vbe_ib.mode_list_ptr) {
	    printk(KERN_ERR "uvesafb: Missing mode list!\n");
	    return -EINVAL;
	}

	printk(KERN_INFO "uvesafb: ");

	/* Convert string pointers and the mode list pointer into
	 * usable addresses. Print informational messages about the
	 * video adapter and its vendor. */
	if (par->vbe_ib.oem_vendor_name_ptr) {
		par->vbe_ib.oem_vendor_name_ptr = (u32)tsk->buf +
			par->vbe_ib.oem_vendor_name_ptr;
		printk("%s, ", (char*)par->vbe_ib.oem_vendor_name_ptr);
	}

	if (par->vbe_ib.oem_product_name_ptr) {
		par->vbe_ib.oem_product_name_ptr = (u32)tsk->buf +
			par->vbe_ib.oem_product_name_ptr;
		printk("%s, ", (char*)par->vbe_ib.oem_product_name_ptr);
	}

	if (par->vbe_ib.oem_product_rev_ptr) {
		par->vbe_ib.oem_product_rev_ptr = (u32)tsk->buf +
			par->vbe_ib.oem_product_rev_ptr;
		printk("%s, ", (char*)par->vbe_ib.oem_product_rev_ptr);
	}

	if (par->vbe_ib.oem_string_ptr) {
		par->vbe_ib.oem_string_ptr = (u32)tsk->buf +
			par->vbe_ib.oem_string_ptr;
		printk("OEM: %s, ", (char*)par->vbe_ib.oem_string_ptr);
	}

	printk("VBE v%d.%d\n", ((par->vbe_ib.vbe_version & 0xff00) >> 8),
		 par->vbe_ib.vbe_version & 0xff);

	if (par->vbe_ib.mode_list_ptr)
		par->vbe_ib.mode_list_ptr = (u32)tsk->buf +
			par->vbe_ib.mode_list_ptr;

	return 0;
}

static int __init inline uvesafb_vbe_getmodes(struct uvesafb_ktask *tsk,
		struct uvesafb_par *par)
{
	int off = 0, err;
	u16 *mode;

	par->vbe_modes_cnt = 0;

	/* Count available modes. */
	mode = (u16*)par->vbe_ib.mode_list_ptr;
	while (*mode != 0xffff) {
		par->vbe_modes_cnt++;
		mode++;
	}

	par->vbe_modes = kzalloc(sizeof(struct vbe_mode_ib) *
				par->vbe_modes_cnt, GFP_KERNEL);
	if (!par->vbe_modes)
		return -ENOMEM;

	/* Get mode info for all available modes. */
	mode = (u16*)par->vbe_ib.mode_list_ptr;

	while (*mode != 0xffff) {
		struct vbe_mode_ib *mib;

		tsk->t.regs.eax = 0x4f01;
		tsk->t.regs.ecx = (u32) *mode;
		tsk->t.flags = TF_BUF_RET | TF_BUF_ESDI;
		tsk->t.buf_len = sizeof(struct vbe_mode_ib);
		tsk->buf = (u8*) par->vbe_modes + off;

		err = uvesafb_exec(tsk);
		if (err || (tsk->t.regs.eax & 0xffff) != 0x004f) {
			printk(KERN_ERR "uvesafb: Getting mode info block "
				"for mode 0x%x failed (eax=0x%x, err=%x)\n",
				*mode, (u32)tsk->t.regs.eax, err);
			return -EINVAL;
		}

		mib = (struct vbe_mode_ib*)tsk->buf;
		mib->mode_id = *mode;

		/* We only want modes that are supported with the current
		 * hardware configuration, color, graphics and that have
		 * support for the LFB. */
		if ((mib->mode_attr & VBE_MODE_MASK) == VBE_MODE_MASK &&
		     mib->bits_per_pixel >= 8) {
			off++;
			printk("got %x: %dx%d\n", *mode, mib->x_res, mib->y_res);
		} else {
			par->vbe_modes_cnt--;
		}
		mode++;
		mib->depth = mib->red_len + mib->green_len + mib->blue_len;
		/* Handle 8bpp modes and modes with broken color component
		 * lengths. */
		if (mib->depth == 0 ||
		    (mib->depth == 24 && mib->bits_per_pixel == 32)) {
			mib->depth = mib->bits_per_pixel;
		}
	}

	return 0;
}

struct uvesafb_par *par;

static int __init uvesafb_init(void)
{
	int err;
	struct uvesafb_ktask *tsk = NULL;

	printk("uvesafb: init\n");

	par = kzalloc(sizeof(struct uvesafb_par), GFP_KERNEL);
	uvesafb_prep(tsk);

	err = cn_add_callback(&uvesafb_cn_id, "uvesafb", uvesafb_cn_callback);
	if (err)
		goto err_out;

	uvesafb_vbe_getinfo(tsk, par);
	uvesafb_vbe_getmodes(tsk, par);

	return 0;
err_out:
	if (nls && nls->sk_socket)
		sock_release(nls->sk_socket);
	return err;
}

static void __exit uvesafb_exit(void)
{
	printk("uvesafb: exit\n");

	if (par->vbe_modes)
		kfree(par->vbe_modes);

	kfree(par);

	cn_del_callback(&uvesafb_cn_id);

	if (nls && nls->sk_socket)
		sock_release(nls->sk_socket);
}

module_init(uvesafb_init);
module_exit(uvesafb_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michal Januszewski <spock@gentoo.org>");
MODULE_DESCRIPTION("Framebuffer driver for VBE2.0+ compliant graphics boards");

