CC = gcc
CFLAGS = -O2 -Wall

servidor: server.c
	$(CC) $(CFLAGS) -o servidor server.c

clean:
	rm -f servidor
	rm -rf data
