config_opt = $(shell if [ -e config.h -a -n "`egrep '^\#define[[:space:]]+$(1)([[:space:]]+|$$)' config.h 2>/dev/null`" ]; then echo true ; fi)

.PHONY: clean install x86emu lrmi

INSTALL = install
KDIR   ?= /lib/modules/$(shell uname -r)/source

ifeq ($(call config_opt,CONFIG_KLIBC),true)
	export CC = klcc
endif

CFLAGS += -I$(KDIR)/include

ifeq ($(call config_opt,CONFIG_X86EMU),true)
	CFLAGS += -Ilibs/x86emu
	LDFLAGS += -Llibs/x86emu
	LDLIBS += -lx86emu
	V86OBJS = v86_x86emu.o v86_mem.o
	V86LIB = x86emu
else
	CFLAGS += -Ilibs/lrmi-0.10
	LDFLAGS += -Llibs/lrmi-0.10 -static
	LDLIBS += -llrmi
	V86OBJS = v86_lrmi.o
	V86LIB = lrmi
endif

all: $(V86LIB) v86d

%.o: %.c v86.h
	$(CC) $(CFLAGS) -c -o $@ $<

v86d: v86.o v86_common.o $(V86OBJS)
	$(CC) $(LDFLAGS) $+ $(LDLIBS) -o $@

testvbe: testvbe.o v86_common.o $(V86OBJS)
	$(CC) $(LDFLAGS) $+ $(LDLIBS) -o $@

x86emu:
	make -w -C libs/x86emu

lrmi:
	make -e -w -C libs/lrmi-0.10 liblrmi.a

clean:
	rm -rf *.o v86d testvbe
	$(MAKE) -w -C libs/lrmi-0.10 clean
	$(MAKE) -w -C libs/x86emu clean

distclean: clean
	rm -rf config.h

install:
	$(INSTALL) -D v86d $(DESTDIR)/sbin/v86d

