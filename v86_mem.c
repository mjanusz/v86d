#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "v86.h"

#define REAL_MEM_BLOCKS	0x100

u8 *mem_low;		/* 0x000000 - 0x001000 */
u8 *mem_real;		/* 0x010000 - 0x09ffff */
u8 *mem_bios;		/* 0x0a0000 - 0x10ffef */

struct mem_block {
	unsigned int size : 20;
	unsigned int free : 1;
};

static struct {
	int ready;
	int count;
	struct mem_block blocks[REAL_MEM_BLOCKS];
} mem_info = { 0 };

void *vptr(u32 addr) {
	if (addr < IVTBDA_SIZE)
		return (mem_low + addr);
	else if (addr >= REAL_MEM_BASE && addr < REAL_MEM_BASE + REAL_MEM_SIZE)
		return (mem_real + addr - REAL_MEM_BASE);
	else if (addr >= BIOS_BASE && addr < BIOS_BASE + BIOS_SIZE)
		return (mem_bios + addr - BIOS_BASE);
	else {
		ulog(LOG_WARNING, "Trying to access an unsupported memory region at %x", addr);
		return NULL;
	}
}

/* We don't care about memory accesses at boundaries of different memory
 * regions, since our v86 memory is non contiguous anyway. */
u8 v_rdb(u32 addr) {
	return *(u8*) vptr(addr);
}

u16 v_rdw(u32 addr) {
	return *(u16*) vptr(addr);
}

u32 v_rdl(u32 addr) {
	return *(u32*) vptr(addr);
}

void v_wrb(u32 addr, u8 val) {
	u8 *t = vptr(addr);
	*t = val;
}

void v_wrw(u32 addr, u16 val) {
	u16 *t = vptr(addr);
	*t = val;
}

void v_wrl(u32 addr, u32 val) {
	u32 *t = vptr(addr);
	*t = val;
}

static void *map_file(void *start, size_t length, int prot, int flags, char *name, long offset)
{
	void *m;
	int fd;

	fd = open(name, (flags & MAP_SHARED) ? O_RDWR : O_RDONLY);

	if (fd == -1) {
		ulog(LOG_ERR, "Open '%s' failed with: %s\n", name, strerror(errno));
		return NULL;
	}

	m = mmap(start, length, prot, flags, fd, offset);

	if (m == (void *)-1) {
		ulog(LOG_ERR, "mmap '%s' failed with: %s\n", name, strerror(errno));
		close(fd);
		return NULL;
	}

	close(fd);
	return m;
}

static int real_mem_init(void)
{
	if (mem_info.ready)
		return 0;

	mem_real = map_file(NULL, REAL_MEM_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
						MAP_PRIVATE, "/dev/zero", 0);
	if (!mem_real)
		return 0;

	mem_info.ready = 1;
	mem_info.count = 1;
	mem_info.blocks[0].size = REAL_MEM_SIZE;
	mem_info.blocks[0].free = 1;

	return 0;
}

static void real_mem_deinit(void)
{
	if (mem_info.ready) {
		munmap(mem_real, REAL_MEM_SIZE);
		mem_info.ready = 0;
	}
}

static void insert_block(int i)
{
	memmove(mem_info.blocks + i + 1, mem_info.blocks + i,
			(mem_info.count - i) * sizeof(struct mem_block));
	mem_info.count++;
}

static void delete_block(int i)
{
	mem_info.count--;
	memmove(mem_info.blocks + i, mem_info.blocks + i + 1,
		 (mem_info.count - i) * sizeof(struct mem_block));
}

u32 v86_mem_alloc(int size)
{
	int i;
	u32 r = REAL_MEM_BASE;

	if (!mem_info.ready)
		return 0;

	if (mem_info.count == REAL_MEM_BLOCKS)
		return 0;

	size = (size + 15) & ~15;

	for (i = 0; i < mem_info.count; i++) {
		if (mem_info.blocks[i].free && size < mem_info.blocks[i].size) {
			insert_block(i);

			mem_info.blocks[i].size = size;
			mem_info.blocks[i].free = 0;
			mem_info.blocks[i + 1].size -= size;

			return r;
		}

		r += mem_info.blocks[i].size;
	}

	return 0;
}

void v86_mem_free(u32 m)
{
	int i;
	u32 r = REAL_MEM_BASE;

	if (!mem_info.ready)
		return;

	i = 0;
	while (m != r) {
		r += mem_info.blocks[i].size;
		i++;
		if (i == mem_info.count)
			return;
	}

	mem_info.blocks[i].free = 1;

	if (i + 1 < mem_info.count && mem_info.blocks[i + 1].free) {
		mem_info.blocks[i].size += mem_info.blocks[i + 1].size;
		delete_block(i + 1);
	}

	if (i - 1 >= 0 && mem_info.blocks[i - 1].free) {
		mem_info.blocks[i - 1].size += mem_info.blocks[i].size;
		delete_block(i);
	}
}

int v86_mem_init(void)
{
	if (real_mem_init())
		return 1;

	/*
	 * We have to map the IVTBDA as shared.  Without it, setting video
	 * modes will not work correctly on some cards (e.g. nVidia GeForce
	 * 8600M, PCI ID 10de:0425).
	 */
	mem_low = map_file(NULL, IVTBDA_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
					MAP_SHARED, "/dev/mem", IVTBDA_BASE);
	if (!mem_low) {
		real_mem_deinit();
		return 1;
	}

	mem_bios = map_file(NULL, BIOS_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
					MAP_SHARED, "/dev/mem", BIOS_BASE);
	if (!mem_bios) {
		munmap(mem_low, IVTBDA_SIZE);
		real_mem_deinit();
		return 1;
	}

	return 0;
}

void v86_mem_cleanup(void)
{
	munmap(mem_low, IVTBDA_SIZE);
	munmap(mem_bios, BIOS_SIZE);

	real_mem_deinit();
}


