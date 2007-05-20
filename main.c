#include <sys/socket.h>
#include <sys/poll.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#include "v86.h"

static int need_exit;
static __u32 seq;

static int netlink_send(int s, struct cn_msg *msg)
{
	struct nlmsghdr *nlh;
	unsigned int size;
	int err;
	char buf[1024];
	struct cn_msg *m;

	size = NLMSG_SPACE(sizeof(struct cn_msg) + msg->len);

	nlh = (struct nlmsghdr *)buf;
	nlh->nlmsg_seq = seq++;
	nlh->nlmsg_pid = getpid();
	nlh->nlmsg_type = NLMSG_DONE;
	nlh->nlmsg_len = NLMSG_LENGTH(size - sizeof(*nlh));
	nlh->nlmsg_flags = 0;

	m = NLMSG_DATA(nlh);
	memcpy(m, msg, sizeof(*m) + msg->len);

	err = send(s, nlh, size, 0);
	if (err == -1)
		ulog("Failed to send: %s [%d].\n", strerror(errno), errno);

	return err;
}

int req_exec(int s, struct cn_msg *msg)
{
	struct uvesafb_task *tsk = (struct uvesafb_task*)(msg + 1);
	u8 *buf = (u8*)tsk + sizeof(struct uvesafb_task);
	int i;

	ulog("performing request\n");
	v86_task(tsk, buf);
	ulog("request done\n");
	netlink_send(s, msg);

	return 0;
}


int main(int argc, char *argv[])
{
	int s;
	char buf[1024];
	int len, i;
	struct nlmsghdr *reply;
	struct sockaddr_nl l_local;
	struct cn_msg *data;
	time_t tm;
	struct pollfd pfd;

	i = open("/dev/tty1", O_RDWR);
	dup2(i, 0);
	dup2(i, 1);
	dup2(i, 2);

	s = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
	if (s == -1) {
		perror("socket");
		return -1;
	}

	l_local.nl_family = AF_NETLINK;
	l_local.nl_groups = 1 << (CN_IDX_UVESAFB-1); /* bitmask of requested groups */
	l_local.nl_pid = 0;

	if (bind(s, (struct sockaddr *)&l_local, sizeof(struct sockaddr_nl)) == -1) {
		perror("bind");
		close(s);
		return -1;
	}

	i = fork();
	if (i) {
		exit(0);
	}

	setsid();
	chdir("/");

	if (v86_init())
		return -1;

	ulog("it's alive %d!\n", s);
	memset(buf, 0, sizeof(buf));

/*	data = (struct cn_msg *)buf;
	data->id.idx = CN_IDX_UVESAFB;
	data->id.val = CN_VAL_UVESAFB;
	data->seq = seq++;
	data->ack = 0;
	data->len = 0;
	netlink_send(s, data);
*/
	pfd.fd = s;

	while (!need_exit) {
		pfd.events = POLLIN;
		pfd.revents = 0;
		switch (poll(&pfd, 1, -1)) {
			case 0:
				need_exit = 1;
				continue;
			case -1:
				if (errno != EINTR) {
					need_exit = 1;
					break;
				}
				continue;
		}

		memset(buf, 0, sizeof(buf));
		len = recv(s, buf, sizeof(buf), 0);
		if (len == -1) {
			perror("recv buf");
			close(s);
			return -1;
		}

		reply = (struct nlmsghdr *)buf;

		switch (reply->nlmsg_type) {
		case NLMSG_ERROR:
			ulog("Error message received.\n");
			break;

		case NLMSG_DONE:
			data = (struct cn_msg *)NLMSG_DATA(reply);
			req_exec(s, data);
			break;
		default:
			break;
		}
	}

	close(s);
	return 0;
}
