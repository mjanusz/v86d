/* vim: ts=8 sts=8 sw=8:
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/skbuff.h>
#include <linux/timer.h>
#include <linux/completion.h>
#include <linux/connector.h>
#include <linux/random.h>
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <video/edid.h>
#include <video/vesa.h>
#include <video/vga.h>
#include <asm/io.h>

#include "edid.h"
#include "uvesafb.h"

static struct cb_id uvesafb_cn_id = { .idx = CN_IDX_UVESAFB, .val = CN_VAL_UVESAFB };
static struct sock *nls;
static char uvesafb_path[] = "/devel/fbdev/uvesafb/v86d";

static struct fb_var_screeninfo uvesafb_defined __devinitdata = {
	.activate	= FB_ACTIVATE_NOW,
	.height		= 0,
	.width		= 0,
	.right_margin	= 32,
	.upper_margin	= 16,
	.lower_margin	= 4,
	.vsync_len	= 4,
	.vmode		= FB_VMODE_NONINTERLACED,
};

static struct fb_fix_screeninfo uvesafb_fix __devinitdata = {
	.id	= "VESA VGA",
	.type	= FB_TYPE_PACKED_PIXELS,
	.accel	= FB_ACCEL_NONE,
};


static int mtrr		__devinitdata = 0; /* disable mtrr by default */
static int blank	__devinitdata = 1; /* enable blanking by default */
static int ypan		__devinitdata = 0; /* 0 - nothing, 1 - ypan, 2 - ywrap */
static int pmi_setpal	__devinitdata = 1; /* use PMI for palette changes */
static int nocrtc	__devinitdata = 0; /* ignore CRTC settings */
static int noedid       __devinitdata = 0; /* don't try DDC transfers */
static int vram_remap   __devinitdata = 0; /* set amount of memory to be used */
static int vram_total   __devinitdata = 0; /* set total amount of memory */
static u16 maxclk       __devinitdata = 0; /* maximum pixel clock */
static u16 maxvf        __devinitdata = 0; /* maximum vertical frequency */
static u16 maxhf        __devinitdata = 0; /* maximum horizontal frequency */
static int gtf          __devinitdata = 0; /* force use of the GTF */
static u16 vbemode      __devinitdata = 0;
static char *mode_option __devinitdata = NULL;

#define TASKS_MAX 1024
static struct uvesafb_ktask* uvfb_tasks[TASKS_MAX];

static void uvesafb_cn_callback(void *data)
{
	struct cn_msg *msg = (struct cn_msg *)data;
	struct uvesafb_task *utsk = (struct uvesafb_task *)msg->data;
	struct uvesafb_ktask *tsk;

//	printk("%s: %lu: idx=%x, val=%x, seq=%u, ack=%u, len=%d.\n",
//	       __func__, jiffies, msg->id.idx, msg->id.val, msg->seq, msg->ack, msg->len);

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

//	printk("pre-wait-for-completion err: %x\n", err);
	uvfb_tasks[seq] = NULL;

	seq++;
	if (seq >= TASKS_MAX)
		seq = 0;

//	printk("done: %x\n", tsk->done->done);

	return (tsk->done->done >= 0) ? 0 : 1;
}

static int __devinit inline uvesafb_vbe_getinfo(struct uvesafb_ktask *tsk,
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

static int __devinit inline uvesafb_vbe_getmodes(struct uvesafb_ktask *tsk,
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

static int __devinit inline uvesafb_vbe_getpmi(struct uvesafb_ktask *tsk,
		struct uvesafb_par *par)
{
	int i, err;

	tsk->t.regs.eax = 0x4f0a;
	tsk->t.regs.ebx = 0x0;
	tsk->t.flags = 0;
	tsk->t.buf_len = 0;

	err = uvesafb_exec(tsk);

	if ((tsk->t.regs.eax & 0xffff) != 0x004f || tsk->t.regs.es < 0xc000) {
		par->pmi_setpal = par->ypan = 0;
	} else {
		par->pmi_base  = (u16*)phys_to_virt(((u32)tsk->t.regs.es << 4) +
			     tsk->t.regs.edi);
		par->pmi_start = (void*)((char*)par->pmi_base + par->pmi_base[1]);
		par->pmi_pal   = (void*)((char*)par->pmi_base + par->pmi_base[2]);
		printk(KERN_INFO "uvesafb: protected mode interface info at "
				 "%04x:%04x\n",
				 (u16)tsk->t.regs.es, (u16)tsk->t.regs.edi);
		printk(KERN_INFO "uvesafb: pmi: set display start = %p, "
				 "set palette = %p\n", par->pmi_start, par->pmi_pal);

		if (par->pmi_base[3]) {
			printk(KERN_INFO "uvesafb: pmi: ports = ");
			for (i = par->pmi_base[3]/2; par->pmi_base[i] != 0xffff; i++)
				printk("%x ", par->pmi_base[i]);
			printk("\n");

			if (par->pmi_base[i] != 0xffff) {
				printk(KERN_INFO "uvesafb: can't handle memory "
						 "requests, pmi disabled\n");
				par->ypan = par->pmi_setpal = 0;
			}
		}
	}
	return 0;
}

static int __devinit inline uvesafb_vbe_getedid(struct uvesafb_ktask *tsk,
						struct fb_info *info)
{
	struct uvesafb_par *par = (struct uvesafb_par *)info->par;
	int err = 0;

	if (noedid || par->vbe_ib.vbe_version < 0x0300)
		return -EINVAL;

	tsk->t.regs.eax = 0x4f15;
	tsk->t.regs.ebx = 0;
	tsk->t.regs.ecx = 0;
	tsk->t.buf_len = 0;
	tsk->t.flags = 0;

	err = uvesafb_exec(tsk);

	if ((tsk->t.regs.eax & 0xffff) != 0x004f || err)
		return -EINVAL;

	if ((tsk->t.regs.ebx & 0x3) == 3) {
		printk(KERN_INFO "uvesafb: VBIOS/hardware supports both "
				 "DDC1 and DDC2 transfers\n");
	} else if ((tsk->t.regs.ebx & 0x3) == 2) {
		printk(KERN_INFO "uvesafb: VBIOS/hardware supports DDC2 "
				 "transfers\n");
	} else if ((tsk->t.regs.ebx & 0x3) == 1) {
		printk(KERN_INFO "uvesafb: VBIOS/hardware supports DDC1 "
				 "transfers\n");
	} else {
		printk(KERN_INFO "uvesafb: VBIOS/hardware doesn't support "
				 "DDC transfers\n");
		return -EINVAL;
	}

	tsk->t.regs.eax = 0x4f15;
	tsk->t.regs.ebx = 1;
	tsk->t.regs.ecx = tsk->t.regs.edx = 0;
	tsk->t.flags = TF_BUF_RET | TF_BUF_ESDI;
	tsk->t.buf_len = EDID_LENGTH;
	tsk->buf = kzalloc(EDID_LENGTH, GFP_KERNEL);

	err = uvesafb_exec(tsk);

	if ((tsk->t.regs.eax & 0xffff) == 0x004f && !err) {
		fb_edid_to_monspecs(tsk->buf, &info->monspecs);
		fb_videomode_to_modelist(info->monspecs.modedb,
				info->monspecs.modedb_len, &info->modelist);
		if (info->monspecs.vfmax && info->monspecs.hfmax) {
			/* If the maximum pixel clock wasn't specified in
			 * the EDID block, set it to 300 MHz. */
			if (info->monspecs.dclkmax == 0)
				info->monspecs.dclkmax = 300 * 1000000;
			info->monspecs.gtf = 1;
		} else {
			err = -EINVAL;
		}
	}

	kfree(tsk->buf);
	return err;
}

static void __devinit inline
	vesafb_vbe_getmonspecs(struct uvesafb_ktask *tsk, struct fb_info *info)
{
	struct fb_var_screeninfo var;
	int i;
	memset(&info->monspecs, 0, sizeof(struct fb_monspecs));

	/* If we don't get all necessary data from the EDID block,
	 * mark it as incompatible with the GTF. */
	if (uvesafb_vbe_getedid(tsk, info))
		info->monspecs.gtf = 0;

	/* Kernel command line overrides. */
	if (maxclk)
		info->monspecs.dclkmax = maxclk * 1000000;
	if (maxvf)
		info->monspecs.vfmax = maxvf;
	if (maxhf)
		info->monspecs.hfmax = maxhf * 1000;

	/* In case DDC transfers are not supported the user can provide
	 * monitor limits manually. Lower limits are set to "safe" values. */
	if (info->monspecs.gtf == 0 && maxclk && maxvf && maxhf) {
		info->monspecs.dclkmin = 0;
		info->monspecs.vfmin = 60;
		info->monspecs.hfmin = 29000;
		info->monspecs.gtf = 1;
	}

	if (info->monspecs.gtf) {
		printk(KERN_INFO
			"uvesafb: monitor limits: vf = %d Hz, hf = %d kHz, "
			"clk = %d MHz\n", info->monspecs.vfmax,
			(int)(info->monspecs.hfmax / 1000),
			(int)(info->monspecs.dclkmax / 1000000));
		/* Add valid VESA video modes to our modelist. */
		for (i = 0; i < VESA_MODEDB_SIZE; i++) {
			fb_videomode_to_var(&var, (struct fb_videomode *)
					    &vesa_modes[i]);
			if (!fb_validate_mode(&var, info))
				fb_add_videomode((struct fb_videomode *)
						 &vesa_modes[i],
						 &info->modelist);
		}
	} else {
		/* Add all VESA video modes to our modelist. */
		fb_videomode_to_modelist((struct fb_videomode *)vesa_modes,
					 VESA_MODEDB_SIZE, &info->modelist);
		printk(KERN_INFO "vesafb: no monitor limits have been set\n");
	}
	return;
}

static int __devinit inline vesafb_vbe_init(struct fb_info *info)
{
	struct uvesafb_ktask *task = NULL;
	struct uvesafb_par *par = (struct uvesafb_par *)info->par;
	int err;

	uvesafb_prep(task);

	if ((err = uvesafb_vbe_getinfo(task, par)))
		goto out;
	if ((err = uvesafb_vbe_getmodes(task, par)))
		goto out;
#ifdef __i386__
	par->pmi_setpal = pmi_setpal;
	par->ypan = ypan;

	if (par->pmi_setpal || par->ypan) {
		uvesafb_vbe_getpmi(task, par);
	}
#else
	/* The protected mode interface is not available on non-x86. */
	par->pmi_setpal = par->ypan = 0;
#endif

	INIT_LIST_HEAD(&info->modelist);
	vesafb_vbe_getmonspecs(task, info);

out:	uvesafb_free(task);
	return err;
}

static struct fb_ops uvesafb_ops = {
	.owner		= THIS_MODULE,
/*	.fb_open	= uvesafb_open,
	.fb_release	= uvesafb_release,
	.fb_setcolreg	= uvesafb_setcolreg,
	.fb_setcmap	= uvesafb_setcmap,
	.fb_pan_display	= uvesafb_pan_display,
	.fb_blank       = uvesafb_blank,
*/	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
/*	.fb_check_var	= uvesafb_check_var,
	.fb_set_par	= uvesafb_set_par*/
};

static int __devinit uvesafb_probe(struct platform_device *dev)
{
	struct fb_info *info;
	struct uvesafb_par *par;
	int err = 0;
	unsigned int size_vmode;
	unsigned int size_remap;
	unsigned int size_total;

	info = framebuffer_alloc(sizeof(struct uvesafb_par) +
				sizeof(u32) * 256, &dev->dev);
	if (!info)
		return -ENOMEM;

	par = (struct uvesafb_par*)info->par;

	if ((err = vesafb_vbe_init(info))) {
		printk(KERN_ERR "uvesafb: vbe_init() with %d\n", err);
		goto out;
	}

	uvesafb_fix.ypanstep  = par->ypan ? 1 : 0;
	uvesafb_fix.ywrapstep = (par->ypan > 1) ? 1 : 0;

	info->pseudo_palette = ((u8*)info->par + sizeof(struct uvesafb_par));
	info->fbops = &uvesafb_ops;
	info->var = uvesafb_defined;
	info->fix = uvesafb_fix;

	if (fb_alloc_cmap(&info->cmap, 256, 0) < 0) {
		err = -ENXIO;
		goto out;
	}

	platform_set_drvdata(dev, info);
	return 0;

out:
	if (par->vbe_modes)
		kfree(par->vbe_modes);

	framebuffer_release(info);
	return err;
}

static int uvesafb_remove(struct platform_device *dev)
{
	struct fb_info *info = platform_get_drvdata(dev);

	if (info) {
		struct uvesafb_par *par = (struct uvesafb_par*) info->par;
//		unregister_framebuffer(info);
		if (par && par->vbe_modes)
			kfree(par->vbe_modes);

		framebuffer_release(info);
	}
	return 0;
}

static struct platform_driver uvesafb_driver = {
	.probe	= uvesafb_probe,
	.remove = uvesafb_remove,
	.driver	= {
		.name	= "uvesafb",
	},
};

static struct platform_device *uvesafb_device;

#ifndef MODULE
static int __devinit uvesafb_setup(char *options)
{
	char *this_opt;

	if (!options || !*options)
		return 0;

	while ((this_opt = strsep(&options, ",")) != NULL) {
		if (!*this_opt) continue;

		if (!strcmp(this_opt, "redraw"))
			ypan = 0;
		else if (!strcmp(this_opt, "ypan"))
			ypan = 1;
		else if (!strcmp(this_opt, "ywrap"))
			ypan = 2;
		else if (!strcmp(this_opt, "vgapal"))
			pmi_setpal = 0;
		else if (!strcmp(this_opt, "pmipal"))
			pmi_setpal = 1;
		else if (!strncmp(this_opt, "mtrr:", 5))
			mtrr = simple_strtoul(this_opt+5, NULL, 0);
		else if (!strcmp(this_opt, "nomtrr"))
			mtrr = 0;
		else if (!strcmp(this_opt, "nocrtc"))
			nocrtc = 1;
		else if (!strcmp(this_opt, "noedid"))
			noedid = 1;
		else if (!strcmp(this_opt, "noblank"))
			blank = 0;
		else if (!strcmp(this_opt, "gtf"))
			gtf = 1;
		else if (!strncmp(this_opt, "vtotal:", 7))
			vram_total = simple_strtoul(this_opt + 7, NULL, 0);
		else if (!strncmp(this_opt, "vremap:", 7))
			vram_remap = simple_strtoul(this_opt + 7, NULL, 0);
		else if (!strncmp(this_opt, "maxhf:", 6))
			maxhf = simple_strtoul(this_opt + 6, NULL, 0);
		else if (!strncmp(this_opt, "maxvf:", 6))
			maxvf = simple_strtoul(this_opt + 6, NULL, 0);
		else if (!strncmp(this_opt, "maxclk:", 7))
			maxclk = simple_strtoul(this_opt + 7, NULL, 0);
		else if (!strncmp(this_opt, "vbemode:", 8))
			vbemode = simple_strtoul(this_opt + 8, NULL,0);
		else if (this_opt[0] >= '0' && this_opt[0] <= '9') {
			mode_option = this_opt;
		} else {
			printk(KERN_WARNING
			       "uvesafb: unrecognized option %s\n", this_opt);
		}
	}

	return 0;
}
#endif /* !MODULE */

static int __devinit uvesafb_init(void)
{
	int err;

#ifndef MODULE
	char *option = NULL;

	if (fb_get_options("uvesafb", &option))
		return -ENODEV;
	uvesafb_setup(option);
#endif
	err = cn_add_callback(&uvesafb_cn_id, "uvesafb", uvesafb_cn_callback);
	if (err)
		goto err_out;

	err = platform_driver_register(&uvesafb_driver);

	if (!err) {
		uvesafb_device = platform_device_alloc("uvesafb", 0);
		if (uvesafb_device)
			err = platform_device_add(uvesafb_device);
		else
			err = -ENOMEM;

		if (err) {
			platform_device_put(uvesafb_device);
			platform_driver_unregister(&uvesafb_driver);
		}
	}
	return err;

err_out:
	if (nls && nls->sk_socket)
		sock_release(nls->sk_socket);
	return err;
}

module_init(uvesafb_init);

#ifdef MODULE
static void __devexit uvesafb_exit(void)
{
	cn_del_callback(&uvesafb_cn_id);

	platform_device_unregister(uvesafb_device);
	platform_driver_unregister(&uvesafb_driver);

	if (nls && nls->sk_socket)
		sock_release(nls->sk_socket);
}

module_exit(uvesafb_exit);

static inline int param_get_scroll(char *buffer, struct kernel_param *kp)
{
	return 0;
}
static inline int param_set_scroll(const char *val, struct kernel_param *kp)
{
	ypan = 0;

	if (! strcmp(val, "redraw"))
		ypan = 0;
	else if (! strcmp(val, "ypan"))
		ypan = 1;
	else if (! strcmp(val, "ywrap"))
		ypan = 2;

	return 0;
}

#define param_check_scroll(name, p) __param_check(name, p, void);

module_param_named(scroll, ypan, scroll, 0);
MODULE_PARM_DESC(scroll, "Scrolling mode, set to 'redraw', "
	"'ypan' or 'ywrap'");
module_param_named(vgapal, pmi_setpal, invbool, 0);
MODULE_PARM_DESC(vgapal, "Set palette using VGA registers");
module_param_named(pmipal, pmi_setpal, bool, 0);
MODULE_PARM_DESC(pmipal, "Set palette using PMI calls");
module_param(mtrr, uint, 0);
MODULE_PARM_DESC(mtrr, "Memory Type Range Registers setting. "
	"Use 0 to disable.");
module_param(blank, bool, 1);
MODULE_PARM_DESC(blank,"Enable hardware blanking");
module_param(nocrtc, bool, 0);
MODULE_PARM_DESC(nocrtc, "Ignore CRTC timings when setting modes");
module_param(noedid, bool, 0);
MODULE_PARM_DESC(noedid, "Ignore EDID-provided monitor limits "
	"when setting modes");
module_param(gtf, bool, 0);
MODULE_PARM_DESC(gtf,"Force use of the VESA GTF to calculate mode timings");
module_param(vram_remap, uint, 0);
MODULE_PARM_DESC(vram_remap,"Set amount of video memory to be used [MiB]");
module_param(vram_total, uint, 0);
MODULE_PARM_DESC(vram_total,"Set total amount of video memoery [MiB]");
module_param(maxclk, ushort, 0);
MODULE_PARM_DESC(maxclk,"Maximum pixelclock [MHz], overrides EDID data");
module_param(maxhf, ushort, 0);
MODULE_PARM_DESC(maxhf, "Maximum horizontal frequency [kHz], "
	"overrides EDID data");
module_param(maxvf, ushort, 0);
MODULE_PARM_DESC(maxvf, "Maximum vertical frequency [Hz], "
	"overrides EDID data");
module_param_named(mode, mode_option, charp, 0);
MODULE_PARM_DESC(mode, "Specify initial video mode as "
	"\"<xres>x<yres>[-<bpp>][@<refresh>]\"");
module_param(vbemode, ushort, 0);
MODULE_PARM_DESC(vbemode, "VBE mode number to set, "
	"overrides 'mode' setting");

#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michal Januszewski <spock@gentoo.org>");
MODULE_DESCRIPTION("Framebuffer driver for VBE2.0+ compliant graphics boards");

