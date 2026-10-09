#ifndef DLOCK_H
#define DLOCK_H

#define SERVER_PORT 8080
#define MAX_CLIENTS 100
#define BUFFER_SIZE 1024
#define HEARTBEAT_INTERVAL 1
#define LEASE_TIMEOUT 5

int create_server_socket(int port);
int connect_to_server(const char *ip, int port);
void send_message(int socket_fd, const char *message);
int receive_message(int socket_fd, char *buffer, int size);

#endif
