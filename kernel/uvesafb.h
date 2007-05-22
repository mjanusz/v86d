#define TF_VBEIB    0x01
#define TF_BUF_ESDI 0x02
#define TF_BUF_ESBX 0x04
#define TF_BUF_RET  0x08

struct uvesafb_task {
	u8 flags;
	int buf_len;
	struct vm86_regs regs;
};

#define VBE_CAP_CAN_SWITCH_DAC	0x01
#define VBE_CAP_VGACOMPAT	0x02

/* This struct is 512 bytes long */
struct vbe_ib {
	char vbe_signature[4];
	u16  vbe_version;
	u32  oem_string_ptr;
	u32  capabilities;
	u32  mode_list_ptr;
	u16  total_memory;
	u16  oem_software_rev;
	u32  oem_vendor_name_ptr;
	u32  oem_product_name_ptr;
	u32  oem_product_rev_ptr;
	u8   reserved[222];
	char oem_data[256];
} __attribute__ ((packed));

struct vbe_crtc_ib {
	u16 horiz_total;
	u16 horiz_start;
	u16 horiz_end;
	u16 vert_total;
	u16 vert_start;
	u16 vert_end;
	u8  flags;
	u32 pixel_clock;
	u16 refresh_rate;
	u8  reserved[40];
} __attribute__ ((packed));

#define VBE_MODE_VGACOMPAT	0x20
#define VBE_MODE_COLOR		0x08
#define VBE_MODE_SUPPORTEDHW	0x01
#define VBE_MODE_GRAPHICS	0x10
#define VBE_MODE_LFB		0x80

#define VBE_MODE_MASK		(VBE_MODE_COLOR | VBE_MODE_SUPPORTEDHW | \
				VBE_MODE_GRAPHICS | VBE_MODE_LFB)

struct vbe_mode_ib {
	/* for all VBE revisions */
	u16 mode_attr;
	u8  winA_attr;
	u8  winB_attr;
	u16 win_granularity;
	u16 win_size;
	u16 winA_seg;
	u16 winB_seg;
	u32 win_func_ptr;
	u16 bytes_per_scan_line;

	/* for VBE 1.2+ */
	u16 x_res;
	u16 y_res;
	u8  x_char_size;
	u8  y_char_size;
	u8  planes;
	u8  bits_per_pixel;
	u8  banks;
	u8  memory_model;
	u8  bank_size;
	u8  image_pages;
	u8  reserved1;

	/* Direct color fields for direct/6 and YUV/7 memory models. */
	/* Offsets are bit positions of lsb in the mask. */
	u8  red_len;
	u8  red_off;
	u8  green_len;
	u8  green_off;
	u8  blue_len;
	u8  blue_off;
	u8  rsvd_len;
	u8  rsvd_off;
	u8  direct_color_info;	/* direct color mode attributes */

	/* for VBE 2.0+ */
	u32 phys_base_ptr;
	u8  reserved2[6];

	/* for VBE 3.0+ */
	u16 lin_bytes_per_scan_line;
	u8  bnk_image_pages;
	u8  lin_image_pages;
	u8  lin_red_len;
	u8  lin_red_off;
	u8  lin_green_len;
	u8  lin_green_off;
	u8  lin_blue_len;
	u8  lin_blue_off;
	u8  lin_rsvd_len;
	u8  lin_rsvd_off;
	u32 max_pixel_clock;
	u16 mode_id;
	u8  depth;
} __attribute__ ((packed));

#ifdef __KERNEL__

#define dac_reg	(0x3c8)
#define dac_val	(0x3c9)

struct uvesafb_pal_entry {
	u_char blue, green, red, pad;
} __attribute__ ((packed));

struct uvesafb_ktask {
	struct uvesafb_task t;
	u8 *buf;
	struct completion *done;
	u32 ack;
};

/* Macros and functions for manipulating uvesafb tasks. */
#define uvesafb_prep(task)					\
do {								\
	task = kmalloc(sizeof(struct uvesafb_ktask), GFP_ATOMIC);\
	if (task) {						\
		memset(task, 0, sizeof(struct uvesafb_ktask));	\
		task->done = kmalloc(sizeof(struct completion), GFP_ATOMIC);\
	}							\
} while (0)

#define uvesafb_reset(task)					\
do {								\
	struct completion *cpl = task->done;			\
	memset(task, 0, sizeof(struct uvesafb_ktask));		\
	task->done = cpl;					\
} while(0)							\

#define uvesafb_free(task)					\
do {								\
	if (task && task->done)					\
		kfree(task->done);				\
	if (task) {						\
		kfree(task);					\
		task = NULL;					\
	}							\
} while(0)

static int uvesafb_exec(struct uvesafb_ktask *tsk);

#define UVESAFB_NEED_EXACT_RES		1
#define UVESAFB_NEED_EXACT_DEPTH	2

struct uvesafb_par {
	struct vbe_ib vbe_ib;
	struct vbe_mode_ib *vbe_modes;
	int vbe_modes_cnt;

	u8 nocrtc;
	u8 ypan;			/* 0 - nothing, 1 - ypan, 2 - ywrap */
	u8 pmi_setpal;	/* pmi for palette changes */
	u16  *pmi_base;		/* protected mode interface location */
	void (*pmi_start)(void);
	void (*pmi_pal)(void);

	u8 *vbe_state;
	int vbe_state_size;
	atomic_t ref_count;

	int mode_idx;
	struct vbe_crtc_ib crtc;
};

#endif
