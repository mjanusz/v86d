ifeq ($(strip $(LIBC)),klibc)
	CC = klcc
	LDFLAGS += -Llibs/lrmi-0.10 -static
	CFLAGS += -Ilibs/lrmi-0.10
else
	CFLAGS += -I/lib/modules/$(shell uname -r)/source/include
endif

CFLAGS += -Ilibs/x86emu

INSTALL = install

all: v86d v86d_x86emu testvbe

v86d: main.o v86_lrmi.o
	$(CC) $(LDFLAGS) $+ -llrmi -o $@

v86d_x86emu: main.o v86_x86emu.o v86_mem.o
	$(CC) $(LDFLAGS) -Llibs/x86emu $+ -lx86emu -o $@

testvbe: testvbe.o v86_x86emu.o v86_mem.o
	$(CC) $(LDFLAGS) -Llibs/x86emu $+ -lx86emu -o $@

clean:
	rm -rf *.o v86d

install:
	$(INSTALL) -D v86d $(DESTDIR)/sbin/v86d

