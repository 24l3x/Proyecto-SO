
CC = gcc
CFLAGS = -Wall
LDFLAGS = -lpthread

TARGETS = servidor cliente

all: $(TARGETS)

servidor: servidor.c
	$(CC) $(CFLAGS) servidor.c -o servidor $(LDFLAGS)

cliente: cliente.c
	$(CC) $(CFLAGS) cliente.c -o cliente -lncurses -lcrypto

clean:
	rm -f $(TARGETS)
