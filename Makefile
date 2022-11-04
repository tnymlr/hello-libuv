
CC = cc `pkg-config --libs --cflags libuv` 

all: main
	./main

main: main.c
	$(CC) -o main main.c