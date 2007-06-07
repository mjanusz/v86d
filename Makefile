ifeq ($(strip $(LIBC)),klibc)
	CC = klcc
	LDFLAGS += -Llibs/lrmi-0.10 -static
	CFLAGS += -Ilibs/lrmi-0.10
else
	CFLAGS += -I/lib/modules/$(shell uname -r)/source/include
endif

INSTALL = install

all: v86d

v86d: main.o v86_lrmi.o
	$(CC) $(LDFLAGS) $+ -llrmi -o $@

clean:
	rm -rf *.o v86d

install:
	$(INSTALL) -D v86d $(DESTDIR)/sbin/v86d

