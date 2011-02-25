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

static int need_exit;
static __u32 seq;

static int netlink_send(int s, struct cn_msg *msg)
{
	struct nlmsghdr *nlh;
	unsigned int size;
	int err;
	char buf[CONNECTOR_MAX_MSG_SIZE];
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
		ulog(LOG_ERR, "Failed to send: %s [%d].\n", strerror(errno), errno);

	return err;
}

int req_exec(int s, struct cn_msg *msg)
{
	struct uvesafb_task *tsk = (struct uvesafb_task*)(msg + 1);
	u8 *buf = (u8*)tsk + sizeof(struct uvesafb_task);

	if (tsk->flags & TF_EXIT)
		return 1;

	if (v86_task(tsk, buf))
		return 2;

	netlink_send(s, msg);

	return 0;
}


int main(int argc, char *argv[])
{
	char buf[CONNECTOR_MAX_MSG_SIZE];
	int len, i, err = 0, s;
	struct nlmsghdr *reply;
	struct sockaddr_nl l_local;
	struct cn_msg *data;
	struct pollfd pfd;

	s = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
	if (s == -1) {
		perror("socket");
		return -1;
	}

	l_local.nl_family = AF_NETLINK;
	l_local.nl_groups = 1 << (CN_IDX_V86D-1); /* bitmask of requested groups */
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

	openlog("v86d", 0, LOG_KERN);

	if (v86_init())
		return -1;

	memset(buf, 0, sizeof(buf));
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
			err = -1;
			goto out;
		}

		reply = (struct nlmsghdr *)buf;

		/* Ignore requests coming from outside the kernel. */
		if (reply->nlmsg_pid != 0) {
			continue;
		}

		switch (reply->nlmsg_type) {
		case NLMSG_ERROR:
			ulog(LOG_ERR, "Error message received.\n");
			break;

		case NLMSG_DONE:
			data = (struct cn_msg *)NLMSG_DATA(reply);
			if (req_exec(s, data))
				goto out;
			break;
		default:
			break;
		}
	}

out:
	v86_cleanup();

	closelog();
	close(s);
	return err;
}
