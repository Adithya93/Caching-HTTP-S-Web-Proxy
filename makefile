CC=gcc
CFLAGS=-I.

threadPool: threadPool.c
	$(CC) -o threadPool threadPool.c -pthread; sudo chown root threadPool; sudo chmod +s threadPool
