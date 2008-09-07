#ifndef __H_V86
#define __H_V86

#include <stdio.h>
#include <syslog.h>
#include <sys/types.h>
#include <linux/connector.h>
#include "config.h"

#undef u8
#undef u16
#undef u32
#undef u64

#define u8 __u8
#define u16 __u16
#define u32 __u32
#define u64 __u64

struct completion;

#include <video/uvesafb.h>

//#define ulog(args...)	do {} while (0)
//#define ulog(args...)		fprintf(stdout, ##args)

#ifdef CONFIG_DEBUG
	#define MAX_LOG_LEVEL	LOG_DEBUG
#else
	#define MAX_LOG_LEVEL	LOG_WARNING
#endif

/* klibc doesn't provide setlogmask(), so simulate it here */
#define ulog(level, args...)   if (level <= MAX_LOG_LEVEL) { syslog(level, ##args); }

int v86_init();
int v86_int(int num, struct v86_regs *regs);
int v86_task(struct uvesafb_task *tsk, u8 *buf);
void v86_cleanup();

#define IVTBDA_BASE			0x00000
#define IVTBDA_SIZE			0x01000
#define DEFAULT_STACK_SIZE	0x02000
#define REAL_MEM_BASE		0x10000
#define REAL_MEM_SIZE		(0x30000 - REAL_MEM_BASE)
#define EBDA_BASE			0x9fc00
#define VRAM_BASE			0xa0000
#define VRAM_SIZE			0x20000
#define SBIOS_SIZE			0x20000
#define SBIOS_BASE			0xe0000
#define VBIOS_BASE			0xc0000

u32 v86_mem_alloc(int size);
void v86_mem_free(u32 m);
int v86_mem_init(void);
void v86_mem_cleanup(void);

u8 v_rdb(u32 addr);
u16 v_rdw(u32 addr);
u32 v_rdl(u32 addr);
void v_wrb(u32 addr, u8 val);
void v_wrw(u32 addr, u16 val);
void v_wrl(u32 addr, u32 val);
void *vptr(u32 addr);

extern int iopl (int __level);
extern int ioperm (unsigned long int __from, unsigned long int __num,
					int __turn_on);

#endif /* __H_V86 */
