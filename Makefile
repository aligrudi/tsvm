CC = cc
CFLAGS = -Wall -O2
LDFLAGS =

OBJS = tsvm.o

all: tsvm
%.o: %.c
	$(CC) -c $(CFLAGS) $<
tsvm: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)
clean:
	rm -f *.o tsvm
