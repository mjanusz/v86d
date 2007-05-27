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
#include <asm/mtrr.h>

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

	return call_usermodehelper(uvesafb_path, argv, envp, 1);
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

	/* FIXME */
	if (!err)
		wait_for_completion_timeout(tsk->done, msecs_to_jiffies(10000));

	uvfb_tasks[seq] = NULL;

	seq++;
	if (seq >= TASKS_MAX)
		seq = 0;

	return (tsk->done->done >= 0) ? 0 : 1;
}

static int uvesafb_vbe_find_mode(struct uvesafb_par *par,
	int xres, int yres, int depth, unsigned char flags)
{
	int i, match = -1, h = 0, d = 0x7fffffff;

	for (i = 0; i < par->vbe_modes_cnt; i++) {
		h = abs(par->vbe_modes[i].x_res - xres) +
		    abs(par->vbe_modes[i].y_res - yres) +
		    abs(depth - par->vbe_modes[i].depth);

		/* We have an exact match in the terms of resolution
		 * and depth. */
		if (h == 0)
			return i;

		if (h < d || (h == d && par->vbe_modes[i].depth > depth)) {
			d = h;
			match = i;
		}
	}
	i = 1;

	if (flags & UVESAFB_NEED_EXACT_DEPTH &&
		par->vbe_modes[match].depth != depth)
		i = 0;
	if (flags & UVESAFB_NEED_EXACT_RES && d > 24)
		i = 0;
	if (i != 0)
		return match;
	else
		return -1;
}

static int uvesafb_setpalette(struct uvesafb_pal_entry *entries, int count,
			     int start, struct fb_info *info)
{
	struct uvesafb_ktask *task;
	struct uvesafb_par *par = info->par;
	int i = par->mode_idx;
	int err = 0;

	/* We support palette modifications for 8 bpp modes only, so
	 * there can never be more than 256 entries. */
	if (start + count > 256)
		return -EINVAL;

	/* Use VGA registers if mode is VGA-compatible. */
	if (i >= 0 && i < par->vbe_modes_cnt &&
	    par->vbe_modes[i].mode_attr & VBE_MODE_VGACOMPAT) {
		for (i = 0; i < count; i++) {
			outb_p(start + i,        dac_reg);
			outb_p(entries[i].red,   dac_val);
			outb_p(entries[i].green, dac_val);
			outb_p(entries[i].blue,  dac_val);
		}
	} else if (par->pmi_setpal) {
		__asm__ __volatile__(
		"call *(%%esi)"
		: /* no return value */
		: "a" (0x4f09),         /* EAX */
		  "b" (0),              /* EBX */
		  "c" (count),          /* ECX */
		  "d" (start),          /* EDX */
		  "D" (entries),        /* EDI */
		  "S" (&par->pmi_pal)); /* ESI */
	} else {
		uvesafb_prep(task);
		task->t.regs.eax = 0x4f09;
		task->t.regs.ebx = 0x0;
		task->t.regs.ecx = count;
		task->t.regs.edx = start;
		task->t.flags = TF_CALL | TF_BUF_ESDI;
		task->t.buf_len = sizeof(struct uvesafb_pal_entry) * count;
		task->buf = (u8*) entries;

		err = uvesafb_exec(task);
		if ((task->t.regs.eax & 0xffff) != 0x004f)
			err = 1;

		uvesafb_free(task);
	}
	return err;
}

static int uvesafb_setcolreg(unsigned regno, unsigned red, unsigned green,
			    unsigned blue, unsigned transp,
			    struct fb_info *info)
{
	struct uvesafb_pal_entry entry;
	int shift = 16 - info->var.green.length;
	int err = 0;

	if (regno >= info->cmap.len)
		return -EINVAL;

	if (info->var.bits_per_pixel == 8) {
		entry.red   = red   >> shift;
		entry.green = green >> shift;
		entry.blue  = blue  >> shift;
		entry.pad   = 0;

		err = uvesafb_setpalette(&entry, 1, regno, info);
	} else if (regno < 16) {
		switch (info->var.bits_per_pixel) {
		case 16:
			if (info->var.red.offset == 10) {
				/* 1:5:5:5 */
				((u32*) (info->pseudo_palette))[regno] =
						((red   & 0xf800) >>  1) |
						((green & 0xf800) >>  6) |
						((blue  & 0xf800) >> 11);
			} else {
				/* 0:5:6:5 */
				((u32*) (info->pseudo_palette))[regno] =
						((red   & 0xf800)      ) |
						((green & 0xfc00) >>  5) |
						((blue  & 0xf800) >> 11);
			}
			break;

		case 24:
		case 32:
			red   >>= 8;
			green >>= 8;
			blue  >>= 8;
			((u32 *)(info->pseudo_palette))[regno] =
				(red   << info->var.red.offset)   |
				(green << info->var.green.offset) |
				(blue  << info->var.blue.offset);
			break;
		}
	}
	return err;
}

static int uvesafb_setcmap(struct fb_cmap *cmap, struct fb_info *info)
{
	struct uvesafb_pal_entry *entries;
	int shift = 16 - info->var.green.length;
	int i, err = 0;

	if (info->var.bits_per_pixel == 8) {
		if (cmap->start + cmap->len > info->cmap.start +
		    info->cmap.len || cmap->start < info->cmap.start)
			return -EINVAL;

		entries = vmalloc(sizeof(struct vesafb_pal_entry) * cmap->len);
		if (!entries)
			return -ENOMEM;
		for (i = 0; i < cmap->len; i++) {
			entries[i].red   = cmap->red[i]   >> shift;
			entries[i].green = cmap->green[i] >> shift;
			entries[i].blue  = cmap->blue[i]  >> shift;
			entries[i].pad   = 0;
		}
		err = uvesafb_setpalette(entries, cmap->len, cmap->start, info);
		vfree(entries);
	} else {
		/* For modes with bpp > 8, we only set the pseudo palette in
		 * the fb_info struct. We rely on vesafb_setcolreg to do all
		 * sanity checking. */
		for (i = 0; i < cmap->len; i++) {
			err |= uvesafb_setcolreg(cmap->start + i, cmap->red[i],
				cmap->green[i], cmap->blue[i],
				0, info);
		}
	}
	return err;
}

static int uvesafb_set_par(struct fb_info *info)
{
	struct uvesafb_par *par = info->par;
	struct uvesafb_ktask *task = NULL;
	struct vbe_crtc_ib *crtc = NULL;
	struct vbe_mode_ib *mode = NULL;
	int i, err = 0, depth = info->var.bits_per_pixel;

	if (depth > 8 && depth != 32)
		depth = info->var.red.length + info->var.green.length +
			info->var.blue.length;

	i = uvesafb_vbe_find_mode(par, info->var.xres, info->var.yres, depth,
				 UVESAFB_NEED_EXACT_RES |
				 UVESAFB_NEED_EXACT_DEPTH);
	if (i >= 0)
		mode = &par->vbe_modes[i];
	else
		return -EINVAL;

	uvesafb_prep(task);
	task->t.regs.eax = 0x4f02;
	task->t.regs.ebx = mode->mode_id | 0x4000;	/* use LFB */
	task->t.flags = 0;

	if (par->vbe_ib.vbe_version >= 0x0300 && !par->nocrtc &&
	    info->var.pixclock != 0) {
		task->t.regs.ebx |= 0x0800;		/* use CRTC data */
		task->t.flags = TF_BUF_ESDI;
		crtc = kzalloc(sizeof(struct vbe_crtc_ib), GFP_KERNEL);
		if (!crtc) {
			err = -ENOMEM;
			goto out;
		}
		crtc->horiz_start = info->var.xres + info->var.right_margin;
		crtc->horiz_end	  = crtc->horiz_start + info->var.hsync_len;
		crtc->horiz_total = crtc->horiz_end + info->var.left_margin;

		crtc->vert_start  = info->var.yres + info->var.lower_margin;
		crtc->vert_end    = crtc->vert_start + info->var.vsync_len;
		crtc->vert_total  = crtc->vert_end + info->var.upper_margin;

		crtc->pixel_clock = PICOS2KHZ(info->var.pixclock) * 1000;
		crtc->refresh_rate = (u16)(100 * (crtc->pixel_clock /
				     (crtc->vert_total * crtc->horiz_total)));
		crtc->flags = 0;

		if (info->var.vmode & FB_VMODE_DOUBLE)
			crtc->flags |= 0x1;
		if (info->var.vmode & FB_VMODE_INTERLACED)
			crtc->flags |= 0x2;
		if (!(info->var.sync & FB_SYNC_HOR_HIGH_ACT))
			crtc->flags |= 0x4;
		if (!(info->var.sync & FB_SYNC_VERT_HIGH_ACT))
			crtc->flags |= 0x8;
		memcpy(&par->crtc, crtc, sizeof(struct vbe_crtc_ib));
	} else
		memset(&par->crtc, 0, sizeof(struct vbe_crtc_ib));

	task->t.buf_len = sizeof(struct vbe_crtc_ib);
	task->buf = (u8*) &par->crtc;

	err = uvesafb_exec(task);
	if (err || (task->t.regs.eax & 0xffff) != 0x004f) {
		printk(KERN_ERR "uvesafb: mode switch failed (eax: 0x%lx, err %x)\n",
				task->t.regs.eax, err);
		err = -EINVAL;
		goto out;
	}
	par->mode_idx = i;

	/* For 8bpp modes, always try to set the DAC to 8 bits. */
	if (par->vbe_ib.capabilities & VBE_CAP_CAN_SWITCH_DAC &&
	    mode->bits_per_pixel <= 8) {
		uvesafb_reset(task);
		task->t.regs.eax = 0x4f08;
		task->t.regs.ebx = 0x0800;

		err = uvesafb_exec(task);
		if (err || (task->t.regs.eax & 0xffff) != 0x004f ||
		    ((task->t.regs.ebx & 0xff00) >> 8) != 8) {
			/* We've failed to set the DAC palette format -
			 * time to correct var. */
			info->var.red.length    = 6;
			info->var.green.length  = 6;
			info->var.blue.length   = 6;
		}
	}

	info->fix.visual = (info->var.bits_per_pixel == 8) ?
		           FB_VISUAL_PSEUDOCOLOR : FB_VISUAL_TRUECOLOR;
	info->fix.line_length = mode->bytes_per_scan_line;

out:	if (crtc != NULL)
		kfree(crtc);
	uvesafb_free(task);

	return err;
}

static void uvesafb_setup_var(struct fb_var_screeninfo *var, struct fb_info *info,
	struct vbe_mode_ib *mode)
{
	struct uvesafb_par *par = (struct uvesafb_par*)info->par;

	var->xres = mode->x_res;
	var->yres = mode->y_res;
	var->xres_virtual = mode->x_res;
	var->yres_virtual = (par->ypan) ?
			      info->fix.smem_len / mode->bytes_per_scan_line :
			      mode->y_res;
	var->xoffset = 0;
	var->yoffset = 0;
	var->bits_per_pixel = mode->bits_per_pixel;

	if (var->bits_per_pixel == 15)
		var->bits_per_pixel = 16;

	if (var->bits_per_pixel > 8) {
		var->red.offset    = mode->red_off;
		var->red.length    = mode->red_len;
		var->green.offset  = mode->green_off;
		var->green.length  = mode->green_len;
		var->blue.offset   = mode->blue_off;
		var->blue.length   = mode->blue_len;
		var->transp.offset = mode->rsvd_off;
		var->transp.length = mode->rsvd_len;

		DPRINTK("directcolor: size=%d:%d:%d:%d, shift=%d:%d:%d:%d\n",
			mode->rsvd_len,
			mode->red_len,
			mode->green_len,
			mode->blue_len,
			mode->rsvd_off,
			mode->red_off,
			mode->green_off,
			mode->blue_off);
	} else {
		var->red.offset    = 0;
		var->green.offset  = 0;
		var->blue.offset   = 0;
		var->transp.offset = 0;

		/* We're assuming that we can switch the DAC to 8 bits. If
		 * this proves to be incorrect, we'll update the fields
		 * later in set_par(). */
		if (par->vbe_ib.capabilities & VBE_CAP_CAN_SWITCH_DAC) {
			var->red.length    = 8;
			var->green.length  = 8;
			var->blue.length   = 8;
			var->transp.length = 0;
		} else {
			var->red.length    = 6;
			var->green.length  = 6;
			var->blue.length   = 6;
			var->transp.length = 0;
		}
	}
}

static void inline uvesafb_check_limits(struct fb_var_screeninfo *var,
	struct fb_info *info)
{
	const struct fb_videomode *mode;
	struct uvesafb_par *par = info->par;

	/* If pixclock is set to 0, then we're using default BIOS timings
	 * and thus don't have to perform any checks here. */
	if (!var->pixclock)
		return;
	if (par->vbe_ib.vbe_version < 0x0300) {
		fb_get_mode(FB_VSYNCTIMINGS | FB_IGNOREMON, 60, var, info);
		return;
	}
	if (!fb_validate_mode(var, info))
		return;
	mode = fb_find_best_mode(var, &info->modelist);
	if (mode) {
		if (mode->xres == var->xres && mode->yres == var->yres &&
		    !(mode->vmode & (FB_VMODE_INTERLACED | FB_VMODE_DOUBLE))) {
			fb_videomode_to_var(var, mode);
			return;
		}
	}
	if (info->monspecs.gtf && !fb_get_mode(FB_MAXTIMINGS, 0, var, info))
		return;
	/* Use default refresh rate */
	var->pixclock = 0;
}

static int uvesafb_check_var(struct fb_var_screeninfo *var,
			    struct fb_info *info)
{
	struct uvesafb_par *par = info->par;
	int match = -1;
	int depth = var->red.length + var->green.length + var->blue.length;

	/* Various apps will use bits_per_pixel to set the color depth,
	 * which is theoretically incorrect, but which we'll try to handle
	 * here. */
	if (depth == 0 || abs(depth - var->bits_per_pixel) >= 8)
		depth = var->bits_per_pixel;
	match = uvesafb_vbe_find_mode(par, var->xres, var->yres, depth,
			UVESAFB_NEED_EXACT_RES);

	if (match == -1)
		return -EINVAL;

	uvesafb_setup_var(var, info, &par->vbe_modes[match]);

	/* Check whether we have remapped enough memory for this mode. */
	if (var->yres * par->vbe_modes[match].bytes_per_scan_line >
	    info->fix.smem_len) {
		return -EINVAL;
	}

	if ((var->vmode & FB_VMODE_DOUBLE) &&
	    !(par->vbe_modes[match].mode_attr & 0x100))
		var->vmode &= ~FB_VMODE_DOUBLE;
	if ((var->vmode & FB_VMODE_INTERLACED) &&
	    !(par->vbe_modes[match].mode_attr & 0x200))
		var->vmode &= ~FB_VMODE_INTERLACED;
	uvesafb_check_limits(var, info);
	return 0;
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
		tsk->buf = (u8*) (par->vbe_modes + off);

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
		par->pmi_base  = (u16*)phys_to_virt(((u32)tsk->t.regs.es << 4) 
				+ tsk->t.regs.edi);
		par->pmi_start = (void*)((char*)par->pmi_base +
				par->pmi_base[1]);
		par->pmi_pal   = (void*)((char*)par->pmi_base +
				par->pmi_base[2]);
		printk(KERN_INFO "uvesafb: protected mode interface info at "
				 "%04x:%04x\n",
				 (u16)tsk->t.regs.es, (u16)tsk->t.regs.edi);
		printk(KERN_INFO "uvesafb: pmi: set display start = %p, "
				 "set palette = %p\n", par->pmi_start,
				 par->pmi_pal);

		if (par->pmi_base[3]) {
			printk(KERN_INFO "uvesafb: pmi: ports = ");
			for (i = par->pmi_base[3]/2;
				par->pmi_base[i] != 0xffff; i++)
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
		printk(KERN_INFO "uvesafb: no monitor limits have been set\n");
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
	par->nocrtc = nocrtc;
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

/* FIXME: export decode_mode? from fbdev core */
static int __devinit decode_mode(u32 *xres, u32 *yres, u32 *bpp, u32 *refresh)
{
	int len = strlen(mode_option), i, err = 0;
	u8 res_specified = 0, bpp_specified = 0, refresh_specified = 0,
	   yres_specified = 0;

	for (i = len-1; i >= 0; i--) {
		switch (mode_option[i]) {
		case '@':
			len = i;
			if (!refresh_specified && !bpp_specified &&
			    !yres_specified) {
				*refresh = simple_strtoul(&mode_option[i+1],
							  NULL, 0);
				refresh_specified = 1;
			} else
				goto out;
			break;
		case '-':
			len = i;
			if (!bpp_specified && !yres_specified) {
				*bpp = simple_strtoul(&mode_option[i+1],
						      NULL, 0);
				bpp_specified = 1;
			} else
				goto out;
			break;
		case 'x':
			if (!yres_specified) {
				*yres = simple_strtoul(&mode_option[i+1],
						       NULL, 0);
				yres_specified = 1;
			} else
				goto out;
			break;
		case '0'...'9':
			break;
		default:
			goto out;
		}
	}

	if (i < 0 && yres_specified) {
		*xres = simple_strtoul(mode_option, NULL, 0);
		res_specified = 1;
	}

out:	if (!res_specified || !yres_specified) {
		printk(KERN_ERR "uvesafb: invalid resolution, "
				"%s not specified\n",
				(!res_specified) ? "width" : "height");
		err = -EINVAL;
	}

	return err;
}

static int __devinit uvesafb_vbe_init_mode(struct fb_info *info)
{
	struct fb_videomode mode;
	struct uvesafb_par *par = info->par;
	int i, modeid, refresh = 0;
	u8 refresh_specified = 0;

	if (!mode_option)
		mode_option = CONFIG_FB_VESA_DEFAULT_MODE;

	/* Has the user requested a specific VESA mode? */
	if (vbemode) {
		for (i = 0; i < par->vbe_modes_cnt; i++) {
			if (par->vbe_modes[i].mode_id == vbemode) {
				info->var.vmode = FB_VMODE_NONINTERLACED;
				info->var.sync = FB_SYNC_VERT_HIGH_ACT;
				uvesafb_setup_var(&info->var, info,
						 &par->vbe_modes[i]);
				fb_get_mode(FB_VSYNCTIMINGS | FB_IGNOREMON,
					    60, &info->var, info);
				/* With pixclock set to 0, the default BIOS
				 * timings will be used in set_par(). */
				info->var.pixclock = 0;
				modeid = i;
				goto out;
			}
		}
		printk(KERN_INFO "uvesafb: requested VBE mode 0x%x is unavailable\n",
				 vbemode);
		vbemode = 0;
	}

	/* Decode the mode specified on the kernel command line. We save
	 * the depth into bits_per_pixel, which is wrong, but will work
	 * anyway. */
	if (decode_mode(&info->var.xres, &info->var.yres,
			&info->var.bits_per_pixel, &refresh))
		return -EINVAL;
	if (refresh)
		refresh_specified = 1;
	else
		refresh = 60;

	/* Look for a matching VBE mode. We can live if an exact match
	 * cannot be found. */
	modeid = uvesafb_vbe_find_mode(par, info->var.xres, info->var.yres,
		info->var.bits_per_pixel, 0);

	if (modeid == -1) {
		return -EINVAL;
	} else {
		info->var.vmode = FB_VMODE_NONINTERLACED;
		info->var.sync = FB_SYNC_VERT_HIGH_ACT;
		uvesafb_setup_var(&info->var, info, &par->vbe_modes[modeid]);
	}

	/* If we are not VBE3.0+ compliant, we're done -- the BIOS will
	 * ignore our mode timings anyway. */
	if (par->vbe_ib.vbe_version < 0x0300) {
		fb_get_mode(FB_VSYNCTIMINGS | FB_IGNOREMON, 60,
			    &info->var, info);
		goto out;
	}

	/* If the user isn't forcing us to use the GTF, try to find mode
	 * timings in our database. */
	if (!gtf) {
		struct fb_videomode tmode;
		const struct fb_videomode *fbmode;

		/* Try to find a closest match if the user requested a
		 * specific refresh rate. Otherwise, just use the best
		 * refresh rate we have. */
		if (refresh_specified) {
			fb_var_to_videomode(&tmode, &info->var);
			tmode.refresh = refresh;
			fbmode = fb_find_nearest_mode(&tmode,
						      &info->modelist);
		} else {
			fbmode = fb_find_best_mode(&info->var,
						   &info->modelist);
		}

		/* If the mode we found has the same resolution and the
		 * difference between its refresh rate and the requested
		 * refresh rate is smaller than 5 Hz, we're done. */
		if (fbmode->xres == info->var.xres &&
		    fbmode->yres == info->var.yres &&
		    !(fbmode->vmode & (FB_VMODE_INTERLACED | FB_VMODE_DOUBLE))
		    && (!refresh_specified ||
			abs(refresh - fbmode->refresh) <= 5))
		{
			fb_videomode_to_var(&info->var, fbmode);
			return modeid;
		}
	}

	i = FB_MAXTIMINGS;
	if (!info->monspecs.gtf)
		i = FB_IGNOREMON | FB_VSYNCTIMINGS;
	else if (refresh_specified)
		i = FB_VSYNCTIMINGS;

	/* If GTF gets us a good mode, we're done. */
	if (!fb_get_mode(i, refresh, &info->var, info))
		goto out;

	/* Otherwise, just try a mode with the highest refresh
	 * rate compatible with the monitor limits. */
	if (info->monspecs.gtf &&
	    !fb_get_mode(FB_MAXTIMINGS, 0, &info->var, info))
		goto out;

	/* If all else failed, use the default refresh rate. */
	printk(KERN_WARNING "uvesafb: using default refresh rate from BIOS\n");
	info->var.pixclock = 0;
out:
	fb_var_to_videomode(&mode, &info->var);
	fb_add_videomode(&mode, &info->modelist);
	return modeid;
}

static struct fb_ops uvesafb_ops = {
	.owner		= THIS_MODULE,
/*	.fb_open	= uvesafb_open,
	.fb_release	= uvesafb_release,
*/	.fb_setcolreg	= uvesafb_setcolreg,
	.fb_setcmap	= uvesafb_setcmap,
/*	.fb_pan_display	= uvesafb_pan_display,
	.fb_blank       = uvesafb_blank,
*/	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
	.fb_check_var	= uvesafb_check_var,
	.fb_set_par	= uvesafb_set_par,
};

static void __devinit uvesafb_init_info(struct fb_info *info, struct vbe_mode_ib *mode)
{
	unsigned int size_vmode;
	unsigned int size_remap;
	unsigned int size_total;
	struct uvesafb_par *par = info->par;
	int i, h;

	info->pseudo_palette = ((u8*)info->par + sizeof(struct uvesafb_par));
	info->fbops = &uvesafb_ops;
	info->fix = uvesafb_fix;
	info->fix.ypanstep = par->ypan ? 1 : 0;
	info->fix.ywrapstep = (par->ypan > 1) ? 1 : 0;

	/* Disable blanking if the user requested so. */
	if (!blank) {
		info->fbops->fb_blank = NULL;
	}

	/* Find out how much IO memory is required for the mode with
	 * the highest resolution. */
	size_remap = 0;
	for (i = 0; i < par->vbe_modes_cnt; i++) {
		h = par->vbe_modes[i].bytes_per_scan_line * par->vbe_modes[i].y_res;
		if (h > size_remap)
			size_remap = h;
	}
	size_remap *= 2;

	/*   size_vmode -- that is the amount of memory needed for the
	 *                 used video mode, i.e. the minimum amount of
	 *                 memory we need. */
	if (mode != NULL) {
		size_vmode = info->var.yres * mode->bytes_per_scan_line;
	} else {
		size_vmode = info->var.yres * info->var.xres *
			     ((info->var.bits_per_pixel + 7) >> 3);
	}

	/*   size_total -- all video memory we have. Used for mtrr
	 *                 entries, resource allocation and bounds
	 *                 checking. */
	size_total = par->vbe_ib.total_memory * 65536;
	if (vram_total)
		size_total = vram_total * 1024 * 1024;
	if (size_total < size_vmode)
		size_total = size_vmode;

	/*   size_remap -- the amount of video memory we are going to
	 *                 use for vesafb.  With modern cards it is no
	 *                 option to simply use size_total as th
	 *                 wastes plenty of kernel address space. */
	if (vram_remap)
		size_remap = vram_remap * 1024 * 1024;
	if (size_remap < size_vmode)
		size_remap = size_vmode;
	if (size_remap > size_total)
		size_remap = size_total;

	info->fix.smem_len = size_remap;
	info->fix.smem_start = mode->phys_base_ptr;

	/* We have to set yres_virtual here because when setup_var() was
	 * called, smem_len wasn't defined yet. */

	info->var.yres_virtual = info->fix.smem_len /
				 mode->bytes_per_scan_line;

	if (par->ypan && info->var.yres_virtual > info->var.yres) {
		printk(KERN_INFO "uvesafb: scrolling: %s "
		       "using protected mode interface, "
		       "yres_virtual=%d\n",
		       (par->ypan > 1) ? "ywrap" : "ypan", info->var.yres_virtual);
	} else {
		printk(KERN_INFO "uvesafb: scrolling: redraw\n");
		info->var.yres_virtual = info->var.yres;
		par->ypan = 0;
	}

	info->flags = FBINFO_FLAG_DEFAULT | 
		(par->ypan) ? FBINFO_HWACCEL_YPAN : 0;

	if (!par->ypan)
		info->fbops->fb_pan_display = NULL;
}

static void uvesafb_init_mtrr(struct fb_info *info)
{
#ifdef CONFIG_MTRR
	if (mtrr && !(info->fix.smem_start & (PAGE_SIZE - 1))) {
		int temp_size = info->fix.smem_len;
		unsigned int type = 0;

		switch (mtrr) {
		case 1:
			type = MTRR_TYPE_UNCACHABLE;
			break;
		case 2:
			type = MTRR_TYPE_WRBACK;
			break;
		case 3:
			type = MTRR_TYPE_WRCOMB;
			break;
		case 4:
			type = MTRR_TYPE_WRTHROUGH;
			break;
		default:
			type = 0;
			break;
		}

		if (type) {
			int rc;

			/* Find the largest power-of-two */
			while (temp_size & (temp_size - 1))
				temp_size &= (temp_size - 1);

			/* Try and find a power of two to add */
			do {
				rc = mtrr_add(info->fix.smem_start,
					      temp_size, type, 1);
				temp_size >>= 1;
			} while (temp_size >= PAGE_SIZE && rc == -EINVAL);
		}
	}
#endif /* CONFIG_MTRR */
}

static int __devinit uvesafb_probe(struct platform_device *dev)
{
	struct fb_info *info;
	struct vbe_mode_ib *mode = NULL;
	struct uvesafb_par *par;
	int err = 0, i;

	info = framebuffer_alloc(sizeof(struct uvesafb_par) +
				sizeof(u32) * 256, &dev->dev);
	if (!info)
		return -ENOMEM;

	par = (struct uvesafb_par*)info->par;

	if ((err = vesafb_vbe_init(info))) {
		printk(KERN_ERR "uvesafb: vbe_init() failed with %d\n", err);
		goto out;
	}

	info->var = uvesafb_defined;
	i = uvesafb_vbe_init_mode(info);
	if (i < 0) {
		err = -EINVAL;
		goto out;
	} else
		mode = &par->vbe_modes[i];

	if (fb_alloc_cmap(&info->cmap, 256, 0) < 0) {
		err = -ENXIO;
		goto out;
	}

	uvesafb_init_info(info, mode);

	if (!request_mem_region(info->fix.smem_start, info->fix.smem_len,
		"uvesafb"))
	{
		printk(KERN_WARNING "uvesafb: cannot reserve video memory at "
		       "0x%lx\n", info->fix.smem_start);
		/* We cannot make this fatal. Sometimes this comes from magic
		   spaces our resource handlers simply don't know about. */
	}

	info->screen_base = ioremap(info->fix.smem_start, info->fix.smem_len);

	if (!info->screen_base) {
		printk(KERN_ERR
		       "uvesafb: abort, cannot ioremap 0x%x bytes of video "
		       "memory at 0x%lx\n",
		       info->fix.smem_len, info->fix.smem_start);
		err = -EIO;
		goto out_mem;
	}

	/* Request failure does not faze us, as vgacon probably has this
	   region already (FIXME) */
	request_region(0x3c0, 32, "uvesafb");

	uvesafb_init_mtrr(info);
	platform_set_drvdata(dev, info);

	if (register_framebuffer(info) < 0) {
		printk(KERN_ERR
		       "uvesafb: failed to register framebuffer device\n");
		err = -EINVAL;
		goto out_unmap;
	}

	printk(KERN_INFO "uvesafb: framebuffer at 0x%lx, mapped to 0x%p, "
	       "using %dk, total %dk\n", info->fix.smem_start,
	       info->screen_base, info->fix.smem_len/1024,
	       par->vbe_ib.total_memory * 64);
	printk(KERN_INFO "fb%d: %s frame buffer device\n", info->node,
	       info->fix.id);

	return 0;

out_unmap:
	iounmap(info->screen_base);
out_mem:
	release_mem_region(info->fix.smem_start, info->fix.smem_len);
	if (!list_empty(&info->modelist))
		fb_destroy_modelist(&info->modelist);
	fb_destroy_modedb(info->monspecs.modedb);
	fb_dealloc_cmap(&info->cmap);
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
		unregister_framebuffer(info);

		iounmap(info->screen_base);
		release_mem_region(info->fix.smem_start, info->fix.smem_len);
		if (!list_empty(&info->modelist))
			fb_destroy_modelist(&info->modelist);
		fb_destroy_modedb(info->monspecs.modedb);
		fb_dealloc_cmap(&info->cmap);

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

