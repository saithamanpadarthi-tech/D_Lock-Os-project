#define _POSIX_C_SOURCE 200809L
#include "dlock.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned long long logical_clock = 0;

static void update_clock(unsigned long long server_clock)
{
    if (server_clock > logical_clock) logical_clock = server_clock;
    logical_clock++;
}

static void send_clocked_message(int socket_fd, const char *kind, int client_id)
{
    char message[BUFFER_SIZE];
    logical_clock++;
    snprintf(message, sizeof(message), "%s %d %llu\n", kind, client_id, logical_clock);
    send_message(socket_fd, message);
}

int main(int argc, char *argv[])
{
    int client_id;
    int socket_fd;
    int granted = 0;
    char buffer[BUFFER_SIZE];

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <client_id>\n", argv[0]);
        return EXIT_FAILURE;
    }

    client_id = atoi(argv[1]);
    if (client_id < 0) {
        fprintf(stderr, "Client ID must be non-negative.\n");
        return EXIT_FAILURE;
    }

    socket_fd = connect_to_server("127.0.0.1", SERVER_PORT);
    printf("[CLIENT %d] Connected to DLock server\n", client_id);
    send_clocked_message(socket_fd, "REQUEST", client_id);
    printf("[CLIENT %d] Lock requested (Lamport=%llu)\n", client_id, logical_clock);

    while (!granted) {
        int received = receive_message(socket_fd, buffer, sizeof(buffer));
        unsigned long long server_clock = 0;
        if (received <= 0) {
            printf("[CLIENT %d] Server disconnected before grant\n", client_id);
            close(socket_fd);
            return EXIT_FAILURE;
        }
        if (sscanf(buffer, "GRANT %llu", &server_clock) == 1) {
            update_clock(server_clock);
            granted = 1;
        } else if (sscanf(buffer, "QUEUED %llu", &server_clock) == 1) {
            update_clock(server_clock);
            printf("[CLIENT %d] Waiting in FIFO queue... (Lamport=%llu)\n", client_id, logical_clock);
        } else if (strncmp(buffer, "REJECT", 6) == 0) {
            printf("[CLIENT %d] Request rejected: %s", client_id, buffer);
            close(socket_fd);
            return EXIT_FAILURE;
        }
    }

    printf("\n========================================\n");
    printf("[CLIENT %d] ENTERING CRITICAL SECTION\n", client_id);
    printf("[CLIENT %d] Lamport clock: %llu\n", client_id, logical_clock);
    printf("========================================\n");

    /* Demo critical section: send a heartbeat every second for five seconds. */
    for (int elapsed = 0; elapsed < 5; elapsed++) {
        struct pollfd descriptor;
        descriptor.fd = socket_fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;

        int ready = poll(&descriptor, 1, HEARTBEAT_INTERVAL * 1000);
        if (ready > 0 && (descriptor.revents & (POLLIN | POLLHUP | POLLERR))) {
            int received = receive_message(socket_fd, buffer, sizeof(buffer));
            if (received <= 0 || strncmp(buffer, "REVOKED", 7) == 0) {
                printf("[CLIENT %d] Lock lease expired; leaving critical section early.\n", client_id);
                close(socket_fd);
                return EXIT_FAILURE;
            }
        }
        send_clocked_message(socket_fd, "HEARTBEAT", client_id);
        printf("[CLIENT %d] Heartbeat sent (Lamport=%llu)\n", client_id, logical_clock);
    }

    printf("[CLIENT %d] Leaving critical section\n", client_id);
    send_clocked_message(socket_fd, "RELEASE", client_id);
    printf("[CLIENT %d] Lock released (Lamport=%llu)\n", client_id, logical_clock);
    close(socket_fd);
    return EXIT_SUCCESS;
}
