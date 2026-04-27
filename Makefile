CC     = gcc
CFLAGS = -Wall -Wextra -g
SRCS   = main.c scheduler.c heap.c
TARGET = scheduler

$(TARGET): $(SRCS) scheduler.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) -lpthread

xv6:
	@echo "xv6 build:"
	@echo "  1. Copy scheduler.h heap.c scheduler.c main.c into xv6 source dir"
	@echo "  2. Add to UPROGS in xv6 Makefile:  _scheduler\\"
	@echo "  3. make CFLAGS+='-DXV6' qemu-nox"

clean:
	rm -f $(TARGET) *.o metrics.csv state_log.csv
