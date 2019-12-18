#
# Makefile for buddy allocator
#

OBJS := buddy_alloc.o
EXEC := buddy_alloc

all: $(OBJS)
	$(CC) $(OBJS) -o $(EXEC)

%.o : %.c
	$(CC) -g $(CFLAGS) -c -o $@ $<

clean:
	rm -rf *.o $(EXEC)
