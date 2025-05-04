CC = gcc
CFLAGS = -Wall -Wextra -O2

all: myServer myClient

myServer: myServer.c
	$(CC) $(CFLAGS) -o myServer myServer.c

myClient: myClient.c
	$(CC) $(CFLAGS) -o myClient myClient.c

clean:
	rm -f myServer myClient
