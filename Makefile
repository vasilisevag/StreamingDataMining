all: client

client: client.c
	gcc -g  -O3  -Wall   $< -o $@ `pkg-config libwebsockets --libs --cflags` -lpthread
	
clean:
	rm -f client
	rm -rf client.dSYM
