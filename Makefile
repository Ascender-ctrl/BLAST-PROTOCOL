CC = gcc
CFLAGS = -Wall -Wextra -g

all: sender receiver

sender: sender.c garbler.c protocol.h garbler.h
	$(CC) $(CFLAGS) -o sender sender.c garbler.c
receiver: receiver.c garbler.c protocol.h garbler.h
	$(CC) $(CFLAGS) -o receiver receiver.c garbler.c

clean:
	rm -f sender receiver *.o
