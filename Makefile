CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -Iinclude

SERVER = dlock_server
CLIENT = dlock_client
CHECKER = dlock_checker

all: $(SERVER) $(CLIENT) $(CHECKER)

$(SERVER): src/server.c src/dlock.c include/dlock.h
	$(CC) $(CFLAGS) src/server.c src/dlock.c -o $(SERVER)

$(CLIENT): src/client.c src/dlock.c include/dlock.h
	$(CC) $(CFLAGS) src/client.c src/dlock.c -o $(CLIENT)

$(CHECKER): src/checker.c
	$(CC) $(CFLAGS) src/checker.c -o $(CHECKER)

clean:
	rm -f $(SERVER) $(CLIENT) $(CHECKER)

run-server: $(SERVER)
	./$(SERVER)

run-client: $(CLIENT)
	./$(CLIENT) 1

check: $(CHECKER)
	./$(CHECKER) logs/dlock.log

.PHONY: all clean run-server run-client check
dlock_retry: src/module5/dlock_retry.c src/dlock.c include/dlock.h
	$(CC) $(CFLAGS) src/module5/dlock_retry.c src/dlock.c -o dlock_retry
