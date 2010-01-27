#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "v86.h"

#define REAL_MEM_BLOCKS	0x100

u8 *mem_low;		/* 0x000000 - 0x001000 */
u8 *mem_real;		/* 0x010000 - 0x09ffff */
u8 *mem_vbios;		/* 0x0c0000 - 0x0cxxxx */
u8 *mem_sbios;		/* 0x0f0000 - 0x0fffff */
u8 *mem_vram;		/* 0x0a0000 - 0xbfffff */
u8 *mem_ebda;		/* usually: 0x9fc00 - 0x9ffff */

static u32 ebda_start;
static u32 ebda_size;
static u32 ebda_diff;
static u32 vbios_size;

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

	/* Order the if's in the expected probability of access to the
	 * given region of memory. */
	if (addr >= REAL_MEM_BASE && addr < REAL_MEM_BASE + REAL_MEM_SIZE)
		return (mem_real + addr - REAL_MEM_BASE);
	else if (addr >= VBIOS_BASE && addr < VBIOS_BASE + vbios_size)
		return (mem_vbios + addr - VBIOS_BASE);
	else if (addr >= SBIOS_BASE && addr < SBIOS_BASE + SBIOS_SIZE)
		return (mem_sbios + addr - SBIOS_BASE);
	else if (addr >= VRAM_BASE && addr < VRAM_BASE + VRAM_SIZE)
		return (mem_vram + addr - VRAM_BASE);
	else if (addr < IVTBDA_SIZE)
		return (mem_low + addr);
	else if (mem_ebda && addr >= ebda_start && addr < ebda_start + ebda_size)
		return (mem_ebda + addr - ebda_start + ebda_diff);
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

	mem_real = map_file(NULL, REAL_MEM_SIZE, PROT_READ | PROT_WRITE,
						MAP_PRIVATE, "/dev/zero", 0);
	if (!mem_real)
		return 1;

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

static int get_bytes_from_phys(u32 addr, int num_bytes, void *dest)
{
	u8 *mem_tmp;
	int size = num_bytes;
	u32 diff = 0;
	u32 t;

	t = addr & -getpagesize();
	if (t) {
		diff = addr - t;
		addr = t;
		size += diff;
	}

	mem_tmp = map_file(NULL, size, PROT_READ | PROT_WRITE,
					MAP_SHARED, "/dev/mem", addr);
	if (!mem_tmp)
		return -1;

	memcpy(dest, mem_tmp + diff, num_bytes);
	munmap(mem_tmp, size);
	return 0;
}

int v86_mem_init(void)
{
	u8 tmp[4];

	if (real_mem_init())
		return 1;

	/*
	 * We have to map the IVTBDA as shared.  Without it, setting video
	 * modes will not work correctly on some cards (e.g. nVidia GeForce
	 * 8600M, PCI ID 10de:0425).
	 */
	mem_low = map_file(NULL, IVTBDA_SIZE, PROT_READ | PROT_WRITE,
					MAP_SHARED, "/dev/mem", IVTBDA_BASE);
	if (!mem_low) {
		real_mem_deinit();
		return 1;
	}

	/* Try to find the start of the EBDA */
	ebda_start = (*(u16*)(mem_low + 0x40e)) << 4;
	if (!ebda_start || ebda_start > EBDA_BASE)
		ebda_start = EBDA_BASE;

	if (get_bytes_from_phys(ebda_start, 1, tmp)) {
		ulog(LOG_WARNING, "Failed to read EBDA size from %x. Ignoring EBDA.", ebda_start);
	} else {
		/* The first byte in the EBDA is its size in kB */
		ebda_size = ((u32) tmp[0]) << 10;
		if (ebda_start + ebda_size > VRAM_BASE) {
			ulog(LOG_WARNING, "EBDA too big (%x), truncating.", ebda_size);
			ebda_size = VRAM_BASE - ebda_start;
		}

		/* Map the EBDA */
		ulog(LOG_DEBUG, "EBDA at %5x-%5x\n", ebda_start, ebda_start + ebda_size - 1);
		u32 t = ebda_start & -getpagesize();

		if (t) {
			ebda_diff = ebda_start - t;
		}

		mem_ebda = map_file(NULL, ebda_size + ebda_diff, PROT_READ | PROT_WRITE, MAP_SHARED, "/dev/mem", ebda_start - ebda_diff);
		if (!mem_ebda) {
			ulog(LOG_WARNING, "Failed to mmap EBDA.  Proceeding without it.");
		}
	}

	/* Map the Video RAM */
	mem_vram = map_file(NULL, VRAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, "/dev/mem", VRAM_BASE);
	if (!mem_vram) {
		ulog(LOG_ERR, "Failed to mmap the Video RAM.");
		v86_mem_cleanup();
		return 1;
	}

	/* Map the Video BIOS */
	get_bytes_from_phys(VBIOS_BASE, 4, tmp);
	if (tmp[0] != 0x55 || tmp[1] != 0xAA) {
		ulog(LOG_ERR, "Video BIOS not found at %x.", VBIOS_BASE);
		v86_mem_cleanup();
		return 1;
	}
	vbios_size = tmp[2] * 0x200;
	ulog(LOG_DEBUG, "VBIOS at %5x-%5x\n", VBIOS_BASE, VBIOS_BASE + vbios_size - 1);

	/*
	 * The Video BIOS and the System BIOS have to be mapped with PROT_WRITE.
	 * There is at least one case where mapping them without this flag causes
	 * a segfault during the emulation: https://bugs.gentoo.org/show_bug.cgi?id=245254
	 */
	mem_vbios = map_file(NULL, vbios_size, PROT_READ | PROT_WRITE,
							MAP_SHARED, "/dev/mem", VBIOS_BASE);

	if (!mem_vbios) {
		ulog(LOG_ERR, "Failed to mmap the Video BIOS.");
		v86_mem_cleanup();
		return 1;
	}

	/* Map the system BIOS */
	mem_sbios = map_file(NULL, SBIOS_SIZE, PROT_READ | PROT_WRITE,
					MAP_SHARED, "/dev/mem", SBIOS_BASE);
	if (!mem_sbios) {
		ulog(LOG_ERR, "Failed to mmap the System BIOS as %5x.", SBIOS_BASE);
		v86_mem_cleanup();
		return 1;
	}

	return 0;
}

void v86_mem_cleanup(void)
{
	if (mem_low)
		munmap(mem_low, IVTBDA_SIZE);

	if (mem_ebda)
		munmap(mem_ebda, ebda_size + ebda_diff);

	if (mem_vram)
		munmap(mem_vram, VRAM_SIZE);

	if (mem_vbios)
		munmap(mem_vbios, vbios_size);

	if (mem_sbios)
		munmap(mem_sbios, SBIOS_SIZE);

	real_mem_deinit();
}


