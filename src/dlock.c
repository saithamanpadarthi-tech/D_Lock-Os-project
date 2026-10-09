#include "dlock.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int create_server_socket(int port)
{
    int server_fd;
    int option = 1;
    struct sockaddr_in address;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) < 0) {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    return server_fd;
}

int connect_to_server(const char *ip, int port)
{
    int socket_fd;
    struct sockaddr_in address;

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);

    if (inet_pton(AF_INET, ip, &address.sin_addr) <= 0) {
        perror("inet_pton");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("connect");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    return socket_fd;
}

void send_message(int socket_fd, const char *message)
{
    size_t sent = 0;
    size_t length = strlen(message);

    while (sent < length) {
        ssize_t result = send(socket_fd, message + sent, length - sent, 0);
        if (result < 0) {
            if (errno == EINTR) continue;
            perror("send");
            return;
        }
        if (result == 0) return;
        sent += (size_t)result;
    }
}

/* Read exactly one newline-terminated protocol message. */
int receive_message(int socket_fd, char *buffer, int size)
{
    int used = 0;

    if (size < 2) return -1;

    while (used < size - 1) {
        char character;
        ssize_t result = recv(socket_fd, &character, 1, 0);

        if (result == 0) {
            if (used == 0) return 0;
            break;
        }
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        buffer[used++] = character;
        if (character == '\n') break;
    }

    buffer[used] = '\0';
    return used;
}
