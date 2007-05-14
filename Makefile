CFLAGS += -I/lib/modules/$(shell uname -r)/source/include

all: v86d

v86d: main.o
	$(CC) $(LDFLAGS) $+ -o $@

clean:
	rm -rf *.o v86d

