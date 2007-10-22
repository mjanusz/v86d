#ifndef __H_TESTVBE
#define __H_TESTVBE

#define VBE_MODE_VGACOMPAT	0x20
#define VBE_MODE_COLOR		0x08
#define VBE_MODE_SUPPORTEDHW	0x01
#define VBE_MODE_GRAPHICS	0x10
#define VBE_MODE_LFB		0x80

#define VBE_MODE_MASK		(VBE_MODE_COLOR | VBE_MODE_SUPPORTEDHW | \
				VBE_MODE_GRAPHICS | VBE_MODE_LFB)

/* VBE Mode Info Block */
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

#endif /* __H_TESTVBE */
