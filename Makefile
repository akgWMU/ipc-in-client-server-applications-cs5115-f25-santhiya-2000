# Makefile for CS5115 PA6 (FIFO IPC with fork())
CC     = gcc
CFLAGS = -Wall -Wextra -O2 -std=c17

all: server client

server: server.c
	$(CC) $(CFLAGS) -o server server.c

client: client.c
	$(CC) $(CFLAGS) -o client client.c

run-server: server
	./server

run-client: client
	./client

clean:
	rm -f server client server.log
	# Optional FIFO cleanup:
	# rm -f /tmp/arith_req_fifo /tmp/arith_resp_*.fifo
