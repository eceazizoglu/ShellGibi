all: main.c
	gcc -o main main.c

run: all
	./main
