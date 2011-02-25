#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#include "v86.h"
#include "testvbe.h"

#define reg16(reg) (reg & 0xffff)
#define failed(tsk) (reg16(tsk.regs.eax) != 0x004f)

int main(int argc, char *argv[])
{
	struct uvesafb_task tsk;
	struct vbe_ib ib;
	u16 *s;
	u8 *t;

	if (v86_init())
		return -1;

	memset(&tsk, 0, sizeof(tsk));
	tsk.regs.eax = 0x4f00;
	tsk.flags = TF_VBEIB;
	tsk.buf_len = sizeof(ib);
	strncpy((char*)&ib.vbe_signature, "VBE2", 4);

	if (v86_task(&tsk, (u8*)&ib))
		return -1;

	if (failed(tsk)) {
		fprintf(stderr, "Getting VBE Info Block failed with eax = %.4x\n",
				reg16(tsk.regs.eax));
		return -1;
	}

	t = (u8*)&ib;

	printf("VBE Version:     %x.%.2x\n", ((ib.vbe_version & 0xf00) >> 8), (ib.vbe_version & 0xff));
	printf("OEM String:      %s\n", ib.oem_string_ptr + t);
	printf("OEM Vendor Name: %s\n", ib.oem_vendor_name_ptr + t);
	printf("OEM Prod. Name:  %s\n", ib.oem_product_name_ptr + t);
	printf("OEM Prod. Rev:   %s\n", ib.oem_product_rev_ptr + t);

	printf("\n%-6s %-6s mode\n", "ID", "attr");
	printf("---------------------------\n");

	for (s = t + ib.mode_list_ptr; *s != 0xffff; s++) {
		struct vbe_mode_ib mib;

		memset(&tsk, 0, sizeof(tsk));
		tsk.regs.eax = 0x4f01;
		tsk.regs.ecx = *s;
		tsk.flags = TF_BUF_RET | TF_BUF_ESDI;
		tsk.buf_len = sizeof(mib);

		if (v86_task(&tsk, (u8*)&mib))
			return -1;

		if (failed(tsk)) {
			fprintf(stderr, "Getting Mode Info Block for mode %.4x "
					"failed with eax = %.4x\n",	*s, reg16(tsk.regs.eax));
			return -1;
		}

		printf("%-6.4x %-6.4x %dx%d-%d %x\n", *s, mib.mode_attr,
				mib.x_res, mib.y_res, mib.bits_per_pixel, mib.phys_base_ptr);
	}

	v86_cleanup();

	return 0;
}
