#define _POSIX_C_SOURCE 200809L

#include "dlock.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#define DEFAULT_TIMEOUT_SEC 10
#define DEFAULT_MAX_RETRIES 3
#define DEFAULT_BACKOFF_MS 1000
#define MAX_BACKOFF_MS 8000

static volatile sig_atomic_t stop_requested = 0;
static unsigned long long logical_clock = 0;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static long long monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;

    return (long long)ts.tv_sec * 1000LL +
           ts.tv_nsec / 1000000LL;
}

static void update_clock(unsigned long long remote_clock)
{
    if (remote_clock > logical_clock)
        logical_clock = remote_clock;

    logical_clock++;
}

static int send_clocked_message(int fd, const char *kind, int client_id)
{
    char message[BUFFER_SIZE];
    int length;

    logical_clock++;

    length = snprintf(message, sizeof(message),
                      "%s %d %llu\n",
                      kind, client_id, logical_clock);

    if (length < 0 || (size_t)length >= sizeof(message))
        return -1;

    send_message(fd, message);
    return 0;
}

static int wait_for_grant(int fd, int client_id, int timeout_sec)
{
    char buffer[BUFFER_SIZE];
    long long start = monotonic_ms();

    if (start < 0)
        return -1;

    while (!stop_requested) {
        long long now = monotonic_ms();
        long long elapsed;
        long long remaining;
        int wait_ms;
        struct pollfd pfd;

        if (now < 0)
            return -1;

        elapsed = now - start;
        remaining = (long long)timeout_sec * 1000LL - elapsed;

        if (remaining <= 0) {
            printf("[RETRY %d] Lock request timed out.\n", client_id);
            return 0;
        }

        wait_ms = remaining > 1000 ? 1000 : (int)remaining;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ready = poll(&pfd, 1, wait_ms);

        if (ready < 0) {
            if (errno == EINTR)
                continue;

            perror("poll");
            return -1;
        }

        if (ready == 0)
            continue;

        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            fprintf(stderr, "[RETRY %d] Server connection lost.\n",
                    client_id);
            return -1;
        }

        if (pfd.revents & POLLIN) {
            memset(buffer, 0, sizeof(buffer));

            int received = receive_message(fd, buffer, sizeof(buffer));

            if (received <= 0) {
                fprintf(stderr, "[RETRY %d] Server disconnected.\n",
                        client_id);
                return -1;
            }

            unsigned long long server_clock = 0;

            if (sscanf(buffer, "GRANT %llu", &server_clock) == 1) {
                update_clock(server_clock);
                printf("[RETRY %d] Lock granted! Lamport=%llu\n",
                       client_id, logical_clock);
                return 1;
            }

            if (sscanf(buffer, "QUEUED %llu", &server_clock) == 1) {
                update_clock(server_clock);
                printf("[RETRY %d] Waiting in server queue...\n",
                       client_id);
                continue;
            }

            if (strncmp(buffer, "REJECT", 6) == 0) {
                printf("[RETRY %d] Server rejected request: %s",
                       client_id, buffer);
                return -1;
            }

            fprintf(stderr, "[RETRY %d] Unexpected server response: %s",
                    client_id, buffer);
            return -1;
        }
    }

    return -1;
}

static void backoff(int attempt)
{
    int delay = DEFAULT_BACKOFF_MS;

    for (int i = 1; i < attempt && delay < MAX_BACKOFF_MS; i++) {
        if (delay > MAX_BACKOFF_MS / 2) {
            delay = MAX_BACKOFF_MS;
            break;
        }

        delay *= 2;
    }

    if (delay > MAX_BACKOFF_MS)
        delay = MAX_BACKOFF_MS;

    printf("[RETRY] Waiting %d ms before next attempt.\n", delay);

    struct timespec remaining = {
        .tv_sec = delay / 1000,
        .tv_nsec = (long)(delay % 1000) * 1000000L
    };

    while (!stop_requested &&
           nanosleep(&remaining, &remaining) < 0 &&
           errno == EINTR) {
        /* Continue sleeping unless interrupted by shutdown. */
    }
}

static int run_attempt(int client_id, int timeout_sec)
{
    int fd = connect_to_server("127.0.0.1", SERVER_PORT);

    if (fd < 0) {
        perror("[RETRY] connect_to_server");
        return -1;
    }

    printf("[RETRY %d] Connected; requesting lock.\n", client_id);

    if (send_clocked_message(fd, "REQUEST", client_id) < 0) {
        close(fd);
        return -1;
    }

    int result = wait_for_grant(fd, client_id, timeout_sec);

    if (result == 1) {
        printf("[RETRY %d] Entering critical section.\n", client_id);

        /*
         * Keep the demo critical section short.
         * Do not sleep longer than the server's lease timeout.
         */
        for (int elapsed = 0; elapsed < 2 && !stop_requested; elapsed++) {
            struct timespec delay = { .tv_sec = 1, .tv_nsec = 0 };

            if (nanosleep(&delay, NULL) < 0 && errno == EINTR)
                break;

            if (!stop_requested &&
                send_clocked_message(fd, "HEARTBEAT", client_id) < 0) {
                result = -1;
                break;
            }
        }

        if (result == 1 && !stop_requested) {
            if (send_clocked_message(fd, "RELEASE", client_id) == 0)
                printf("[RETRY %d] Lock released.\n", client_id);
            else
                result = -1;
        }
    }

    /*
     * Closing a connection cancels a waiting request in the current
     * server implementation. If the lock was granted, RELEASE above
     * tells the server to hand it to the next queued client.
     */
    shutdown(fd, SHUT_RDWR);
    close(fd);

    return result;
}

int main(int argc, char *argv[])
{
    int client_id;
    int timeout_sec = DEFAULT_TIMEOUT_SEC;
    int max_retries = DEFAULT_MAX_RETRIES;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (argc < 2 || argc > 4) {
        fprintf(stderr,
                "Usage: %s <client_id> [timeout_seconds] [max_retries]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    char *end = NULL;
    long parsed_id = strtol(argv[1], &end, 10);

    if (end == argv[1] || *end != '\0' ||
        parsed_id < 0 || parsed_id > 2147483647L) {
        fprintf(stderr, "Invalid client ID.\n");
        return EXIT_FAILURE;
    }

    client_id = (int)parsed_id;

    if (argc >= 3) {
        long parsed_timeout = strtol(argv[2], &end, 10);

        if (end == argv[2] || *end != '\0' ||
            parsed_timeout < 1 || parsed_timeout > 3600) {
            fprintf(stderr, "Timeout must be between 1 and 3600 seconds.\n");
            return EXIT_FAILURE;
        }

        timeout_sec = (int)parsed_timeout;
    }

    if (argc >= 4) {
        long parsed_retries = strtol(argv[3], &end, 10);

        if (end == argv[3] || *end != '\0' ||
            parsed_retries < 0 || parsed_retries > 100) {
            fprintf(stderr, "Max retries must be between 0 and 100.\n");
            return EXIT_FAILURE;
        }

        max_retries = (int)parsed_retries;
    }

    printf("=== DLock Module 5 ===\n");
    printf("Client ID: %d\n", client_id);
    printf("Request timeout: %d seconds\n", timeout_sec);
    printf("Maximum retries after initial attempt: %d\n\n", max_retries);

    for (int attempt = 0;
         attempt <= max_retries && !stop_requested;
         attempt++) {
        printf("\n[RETRY] Attempt %d of %d\n",
               attempt + 1, max_retries + 1);

        int result = run_attempt(client_id, timeout_sec);

        if (result == 1) {
            printf("[RETRY] Completed successfully.\n");
            return EXIT_SUCCESS;
        }

        if (stop_requested)
            break;

        if (attempt == max_retries)
            break;

        backoff(attempt + 1);
    }

    if (stop_requested) {
        printf("\n[RETRY] Interrupted by user.\n");
    } else {
        fprintf(stderr,
                "\n[RETRY] Unable to acquire the lock after %d attempt(s).\n",
                max_retries + 1);
    }

    return EXIT_FAILURE;
}
