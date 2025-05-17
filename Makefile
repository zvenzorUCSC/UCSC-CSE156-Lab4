CC = gcc
CFLAGS = -Wall -Wextra -O2

all: bin/myserver bin/myclient

bin/myserver: src/myserver.c
	gcc -Wall -Wextra -O2 -o bin/myserver src/myserver.c

bin/myclient: src/myclient.c
	gcc -Wall -Wextra -O2 -o bin/myclient src/myclient.c

clean:
	rm -f bin/myserver bin/myclient
