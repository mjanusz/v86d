#ifndef __H_V86
#define __H_V86

#include <stdio.h>
#include <syslog.h>
#include <sys/types.h>
#include <linux/connector.h>
#include <asm/vm86.h>

#define u8 __u8
#define u16 __u16
#define u32 __u32

struct completion;

#include "kernel/uvesafb.h"

//#define ulog(args...)	do {} while (0)
//#define ulog(args...)		fprintf(stdout, ##args)

#define ulog(args...)	syslog(LOG_INFO, ##args)

int v86_init();
int v86_int(int num, struct vm86_regs *regs);
int v86_task(struct uvesafb_task *tsk, u8 *buf);

#endif /* __H_V86 */
