#ifndef __H_V86
#define __H_V86

#include <stdio.h>
#include <syslog.h>
#include <sys/types.h>
#include <linux/connector.h>
#include <asm/vm86.h>

#undef u8
#undef u16
#undef u32

#define u8 __u8
#define u16 __u16
#define u32 __u32

struct completion;

#include <video/uvesafb.h>

//#define ulog(args...)	do {} while (0)
//#define ulog(args...)		fprintf(stdout, ##args)

#define ulog(args...)	syslog(LOG_INFO, ##args)

int v86_init();
int v86_int(int num, struct vm86_regs *regs);
int v86_task(struct uvesafb_task *tsk, u8 *buf);
void v86_cleanup();

#define IVTBDA_BASE			0x00000
#define IVTBDA_SIZE			0x01000
#define DEFAULT_STACK_SIZE	0x02000
#define REAL_MEM_BASE		0x10000
#define REAL_MEM_SIZE		0xa0000 - REAL_MEM_BASE
#define BIOS_MEM_SIZE		(0x100000 - REAL_MEM_BASE - REAL_MEM_SIZE)

void *v86_mem_alloc(int size);
void v86_mem_free(void *m);
int v86_mem_init(void);
void v86_mem_cleanup(void);

u16 get_int_seg(int i);
u16 get_int_off(int i);

extern u8 *real_mem;

#endif /* __H_V86 */
