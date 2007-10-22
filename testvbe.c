#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#include "v86.h"
#include "testvbe.h"

int main(int argc, char *argv[])
{
	struct uvesafb_task tsk;
	struct vbe_ib ib;
	u16 *s;
	u8 *t;

	if (v86_init())
		return -1;
	tsk.regs.eax = 0x4f00;
	tsk.flags = TF_VBEIB;
	tsk.buf_len = sizeof(ib);
	strncpy(&ib.vbe_signature, "VBE2", 4);

	v86_task(&tsk, &ib);

	t = &ib;

	printf("VBE Version:     %x\n", ib.vbe_version);
	printf("OEM String:      %s\n", ib.oem_string_ptr + t);
	printf("OEM Vendor Name: %s\n", ib.oem_vendor_name_ptr + t);
	printf("OEM Prod. Name:  %s\n", ib.oem_product_name_ptr + t);
	printf("OEM Prod. Rev:   %s\n", ib.oem_product_rev_ptr + t);

	for (s = ib.mode_list_ptr + t; *s != 0xffff; s++) {
		struct vbe_mode_ib mib;

		tsk.regs.eax = 0x4f01;
		tsk.regs.ecx = *s;
		tsk.flags = TF_BUF_RET | TF_BUF_ESDI;
		tsk.buf_len = sizeof(mib);

		v86_task(&tsk, &mib);

		printf("%6.4x %6.4x %dx%d-%d\n", *s, mib.mode_attr,
				mib.x_res, mib.y_res, mib.bits_per_pixel);
	}

/*	tsk.regs.eax = 0x4f02;
	tsk.regs.ebx = 0xc161;
	tsk.buf_len = 0;
	tsk.flags = 0;

	v86_task(&tsk, buf);

	printf("got eax = %x\n", tsk.regs.eax);
*/
	v86_cleanup();

	return 0;
}
