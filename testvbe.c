#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/poll.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <arpa/inet.h>

#include "v86.h"

int main(int argc, char *argv[])
{
	struct uvesafb_task tsk;
	u8 buf[4096];

	if (v86_init())
		return -1;

	tsk.regs.eax = 0x4f00;
	tsk.flags = TF_VBEIB;
	tsk.buf_len = sizeof(struct vbe_ib);
	strncpy(&(((struct vbe_ib*)buf)->vbe_signature), "VBE2", 4);

	v86_task(&tsk, buf);

	printf("%s\n", ((struct vbe_ib*)buf)->oem_vendor_name_ptr + buf);

	v86_cleanup();

	return 0;
}
