#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include "v86.h"

#define REAL_MEM_BLOCKS	0x100

u8 *real_mem = NULL;

struct mem_block {
	unsigned int size : 20;
	unsigned int free : 1;
};

static struct {
	int ready;
	int count;
	struct mem_block blocks[REAL_MEM_BLOCKS];
} mem_info = { 0 };

static int read_file(char *name, void *p, size_t n)
{
	int fd;

	fd = open(name, O_RDONLY);

	if (fd == -1) {
		perror("open");
		return 0;
	}

	if (read(fd, p, n) != n) {
		perror("read");
		close(fd);
		return 0;
	}

	close(fd);

	return 1;
}

static int map_file(void *start, size_t length, int prot, int flags, char *name, long offset)
{
	void *m;
	int fd;

	fd = open(name, (flags & MAP_SHARED) ? O_RDWR : O_RDONLY);

	if (fd == -1) {
		perror("open");
		return 0;
	}

	m = mmap(start, length, prot, flags, fd, offset);

	if (m == (void *)-1) {
		perror("mmap");
		close(fd);
		return 0;
	}

	close(fd);
	return 1;
}

static int real_mem_init(void)
{
	if (mem_info.ready)
		return 0;

	if (!map_file((void *)REAL_MEM_BASE, REAL_MEM_SIZE,
		 PROT_READ | PROT_WRITE | PROT_EXEC,
		 MAP_FIXED | MAP_PRIVATE, "/dev/zero", 0))
		return 0;

	real_mem = (u8*)0;

	mem_info.ready = 1;
	mem_info.count = 1;
	mem_info.blocks[0].size = REAL_MEM_SIZE;
	mem_info.blocks[0].free = 1;

	return 0;
}

static void real_mem_deinit(void)
{
	if (mem_info.ready) {
		munmap((void *)REAL_MEM_BASE, REAL_MEM_SIZE);
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

void *v86_mem_alloc(int size)
{
	int i;
	char *r = (char *)REAL_MEM_BASE;

	if (!mem_info.ready)
		return NULL;

	if (mem_info.count == REAL_MEM_BLOCKS)
		return NULL;

	size = (size + 15) & ~15;

	for (i = 0; i < mem_info.count; i++) {
		if (mem_info.blocks[i].free && size < mem_info.blocks[i].size) {
			insert_block(i);

			mem_info.blocks[i].size = size;
			mem_info.blocks[i].free = 0;
			mem_info.blocks[i + 1].size -= size;

			return (void *)r;
		}

		r += mem_info.blocks[i].size;
	}

	return NULL;
}

void v86_mem_free(void *m)
{
	int i;
	char *r = (char *)REAL_MEM_BASE;

	if (!mem_info.ready)
		return;

	i = 0;
	while (m != (void *)r) {
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

static inline void set_bit(unsigned int bit, void *array)
{
	unsigned char *a = array;
	a[bit / 8] |= (1 << (bit % 8));
}

inline u16 get_int_seg(int i)
{
	return *(u16 *)(i * 4 + 2);
}

inline u16 get_int_off(int i)
{
	return *(u16 *)(i * 4);
}

int v86_mem_init(void)
{
	void *m;
	int fd_mem;

	if (real_mem_init())
		return 1;

	if (!map_file((void *)IVTBDA_BASE, IVTBDA_SIZE,
		PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_FIXED | MAP_PRIVATE, "/dev/zero", 0))
	{
		real_mem_deinit();
		return 1;
	}

	if (!read_file("/dev/mem", (void *)IVTBDA_BASE, IVTBDA_SIZE)) {
		munmap((void *)IVTBDA_BASE, IVTBDA_SIZE);
		real_mem_deinit();
		return 1;
	}

	if (!map_file((void *)0xa0000, 0x100000 - 0xa0000,
		PROT_READ | PROT_WRITE,
		MAP_FIXED | MAP_SHARED, "/dev/mem", 0xa0000))
	{
		munmap((void *)IVTBDA_BASE, IVTBDA_SIZE);
		real_mem_deinit();
		return 1;
	}

	return 0;
}

void v86_mem_cleanup(void)
{
	munmap((void *)IVTBDA_BASE, IVTBDA_SIZE);
	munmap((void *)0xa0000, 0x100000 - 0xa0000);

	real_mem_deinit();
}


