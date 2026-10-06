CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -Iinclude

SERVER = dlock_server
CLIENT = dlock_client

all: $(SERVER) $(CLIENT)

$(SERVER):
	$(CC) $(CFLAGS) src/server.c src/dlock.c -o $(SERVER)

$(CLIENT):
	$(CC) $(CFLAGS) src/client.c src/dlock.c -o $(CLIENT)

clean:
	rm -f $(SERVER) $(CLIENT)

run-server:
	./$(SERVER)

run-client:
	./$(CLIENT) 1
