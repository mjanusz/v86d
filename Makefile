config_opt = $(shell if [ -e config.h -a -n "`egrep '^\#define[[:space:]]+$(1)([[:space:]]+|$$)' config.h 2>/dev/null`" ]; then echo true ; fi)

.PHONY: clean install install_testvbe x86emu lrmi

INSTALL = install
KDIR   ?= /lib/modules/$(shell uname -r)/source

ifeq ($(call config_opt,CONFIG_KLIBC),true)
	export CC = klcc
endif

CFLAGS ?= -Wall -g -O2
CFLAGS += -I$(KDIR)/include

ifeq ($(call config_opt,CONFIG_X86EMU),true)
	CFLAGS += -Ilibs/x86emu
	LDFLAGS += -Llibs/x86emu
	LDLIBS += -lx86emu
	V86OBJS = v86_x86emu.o v86_mem.o v86_common.o 
	V86LIB = x86emu
else
	CFLAGS += -Ilibs/lrmi-0.10
	LDFLAGS += -Llibs/lrmi-0.10 -static -Wl,--section-start,vm86_ret=0x9000
	LDLIBS += -llrmi
	V86OBJS = v86_lrmi.o v86_common.o
	V86LIB = lrmi
endif

DEBUG_BUILD =
DEBUG_INSTALL =

ifeq ($(call config_opt,CONFIG_DEBUG),true)
	DEBUG_BUILD += testvbe
	DEBUG_INSTALL += install_testvbe
endif

all: $(V86LIB) v86d $(DEBUG_BUILD)

%.o: %.c v86.h
	$(CC) $(CFLAGS) -c -o $@ $<

v86d: $(V86OBJS) $(V86LIB) v86.o
	$(CC) $(LDFLAGS) $(V86OBJS) v86.o $(LDLIBS) -o $@

testvbe: $(V86OBJS) $(V86LIB) testvbe.o
	$(CC) $(LDFLAGS) $(V86OBJS) testvbe.o $(LDLIBS) -o $@

x86emu:
	$(MAKE) -w -C libs/x86emu

lrmi:
	$(MAKE) -e -w -C libs/lrmi-0.10 liblrmi.a

clean:
	rm -rf *.o v86d testvbe
	$(MAKE) -w -C libs/lrmi-0.10 clean
	$(MAKE) -w -C libs/x86emu clean

distclean: clean
	rm -rf config.h

install: $(DEBUG_INSTALL)
	$(INSTALL) -D v86d $(DESTDIR)/sbin/v86d

install_testvbe:
	$(INSTALL) -D testvbe $(DESTDIR)/sbin/testvbe
