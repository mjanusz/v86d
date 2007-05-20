CFLAGS += -I/lib/modules/$(shell uname -r)/source/include

all: v86d

v86d: main.o v86_lrmi.o
	$(CC) $(LDFLAGS) $+ -llrmi -o $@

clean:
	rm -rf *.o v86d

