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

	printk("complete\n");
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
	memcpy(m + 1, tsk, sizeof(struct uvesafb_task));
	memcpy(((u8*)m) + (sizeof(struct uvesafb_task) + sizeof(*m)), tsk->buf, tsk->t.buf_len);

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

	uvfb_tasks[seq] = NULL;

	seq++;
	if (seq >= TASKS_MAX)
		seq = 0;

	printk("done: %x\n", tsk->done->done);

	return !tsk->done->done;
}

static int __init uvesafb_init(void)
{
	int err;
	struct uvesafb_ktask *tsk = NULL;
	struct vbe_ib ib;

	printk("uvesafb: init\n");

	uvesafb_prep(tsk);

	err = cn_add_callback(&uvesafb_cn_id, "uvesafb", uvesafb_cn_callback);
	if (err)
		goto err_out;

	tsk->t.regs.eax = 0x4f00;
	tsk->t.flags = TF_VBEIB;
	tsk->t.buf_len = sizeof(struct vbe_ib);
	tsk->buf = (u8*)&ib;

	if (!uvesafb_exec(tsk)) {
	    printk("uvesafb: got version %x", ib.vbe_version);
	}

	return 0;
err_out:
	if (nls && nls->sk_socket)
		sock_release(nls->sk_socket);
	return err;
}

static void __exit uvesafb_exit(void)
{
	printk("uvesafb: exit\n");

	cn_del_callback(&uvesafb_cn_id);

	if (nls && nls->sk_socket)
		sock_release(nls->sk_socket);
}

module_init(uvesafb_init);
module_exit(uvesafb_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michal Januszewski <spock@gentoo.org>");
MODULE_DESCRIPTION("Framebuffer driver for VBE2.0+ compliant graphics boards");

