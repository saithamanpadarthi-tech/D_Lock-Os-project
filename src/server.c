#define _POSIX_C_SOURCE 200809L
#include "dlock.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

#define QUEUE_CAPACITY MAX_CLIENTS

typedef struct {
    int client_id;
    int slot;
} QueueEntry;

static struct pollfd poll_fds[MAX_CLIENTS + 1];
static int client_ids[MAX_CLIENTS];
static bool active[MAX_CLIENTS];
static bool requested[MAX_CLIENTS];
static time_t last_heartbeat[MAX_CLIENTS];
static QueueEntry queue_entries[QUEUE_CAPACITY];
static int queue_head = 0;
static int queue_tail = 0;
static int queue_count = 0;
static int lock_holder_slot = -1;
static unsigned long long lamport_clock = 0;
static FILE *log_file = NULL;
static volatile sig_atomic_t stop_requested = 0;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void tick_clock(void)
{
    lamport_clock++;
}

static void receive_clock(unsigned long long remote_clock)
{
    if (remote_clock > lamport_clock) lamport_clock = remote_clock;
    lamport_clock++;
}

static void log_event(const char *event, int client_id)
{
    time_t now = time(NULL);
    struct tm time_info;
    char timestamp[32];

    localtime_r(&now, &time_info);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &time_info);

    if (log_file != NULL) {
        fprintf(log_file, "%s LAMPORT=%llu EVENT=%s CLIENT=%d\n",
                timestamp, lamport_clock, event, client_id);
        fflush(log_file);
    }
    printf("[SERVER] LAMPORT=%llu EVENT=%s CLIENT=%d\n",
           lamport_clock, event, client_id);
}

static void load_last_clock(void)
{
    char line[512];
    FILE *input = fopen("logs/dlock.log", "r");
    unsigned long long value;

    if (input == NULL) return;
    while (fgets(line, sizeof(line), input) != NULL) {
        char *position = strstr(line, "LAMPORT=");
        if (position != NULL && sscanf(position, "LAMPORT=%llu", &value) == 1 && value > lamport_clock)
            lamport_clock = value;
    }
    fclose(input);
}

static int queue_push(int slot)
{
    if (queue_count >= QUEUE_CAPACITY) return -1;
    queue_entries[queue_tail].slot = slot;
    queue_entries[queue_tail].client_id = client_ids[slot];
    queue_tail = (queue_tail + 1) % QUEUE_CAPACITY;
    queue_count++;
    return 0;
}

static QueueEntry queue_pop(void)
{
    QueueEntry entry = {-1, -1};
    if (queue_count == 0) return entry;
    entry = queue_entries[queue_head];
    queue_head = (queue_head + 1) % QUEUE_CAPACITY;
    queue_count--;
    return entry;
}

static void grant_next_request(void)
{
    while (lock_holder_slot == -1 && queue_count > 0) {
        QueueEntry entry = queue_pop();
        int slot = entry.slot;
        char message[BUFFER_SIZE];

        if (slot < 0 || slot >= MAX_CLIENTS || !active[slot] || !requested[slot] ||
            client_ids[slot] < 0 || client_ids[slot] != entry.client_id ||
            poll_fds[slot + 1].fd < 0)
            continue;

        lock_holder_slot = slot;
        last_heartbeat[slot] = time(NULL);
        tick_clock();
        log_event("GRANT", client_ids[slot]);
        snprintf(message, sizeof(message), "GRANT %llu\n", lamport_clock);
        send_message(poll_fds[slot + 1].fd, message);
    }
}

static void close_client(int slot, const char *reason)
{
    int id;
    if (slot < 0 || slot >= MAX_CLIENTS || !active[slot]) return;
    id = client_ids[slot];

    if (lock_holder_slot == slot) {
        lock_holder_slot = -1;
        tick_clock();
        log_event(reason, id);
        grant_next_request();
    } else if (id >= 0) {
        tick_clock();
        log_event("CLIENT_DISCONNECTED", id);
    }

    if (poll_fds[slot + 1].fd >= 0) close(poll_fds[slot + 1].fd);
    poll_fds[slot + 1].fd = -1;
    poll_fds[slot + 1].events = POLLIN;
    client_ids[slot] = -1;
    active[slot] = false;
    requested[slot] = false;
    last_heartbeat[slot] = 0;
}

static int find_free_slot(void)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) if (!active[i]) return i;
    return -1;
}

static void check_expired_lease(void)
{
    time_t now = time(NULL);
    int slot = lock_holder_slot;

    if (slot >= 0 && active[slot] && now - last_heartbeat[slot] >= LEASE_TIMEOUT) {
        int fd = poll_fds[slot + 1].fd;
        char message[BUFFER_SIZE];
        tick_clock();
        log_event("LEASE_EXPIRED", client_ids[slot]);
        snprintf(message, sizeof(message), "REVOKED %llu\n", lamport_clock);
        if (fd >= 0) send_message(fd, message);
        lock_holder_slot = -1;
        close_client(slot, "LEASE_EXPIRED");
        grant_next_request();
    }
}

int main(void)
{
    int server_fd;
    int i;
    char buffer[BUFFER_SIZE];

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    mkdir("logs", 0755);
    load_last_clock();
    log_file = fopen("logs/dlock.log", "a");
    if (log_file == NULL) {
        perror("logs/dlock.log");
        return EXIT_FAILURE;
    }

    server_fd = create_server_socket(SERVER_PORT);
    poll_fds[0].fd = server_fd;
    poll_fds[0].events = POLLIN;
    poll_fds[0].revents = 0;
    for (i = 0; i < MAX_CLIENTS; i++) {
        poll_fds[i + 1].fd = -1;
        poll_fds[i + 1].events = POLLIN;
        client_ids[i] = -1;
        active[i] = false;
        requested[i] = false;
        last_heartbeat[i] = 0;
    }

    tick_clock();
    log_event("SERVER_START", -1);
    printf("[SERVER] Listening on TCP port %d; lease timeout=%d seconds\n",
           SERVER_PORT, LEASE_TIMEOUT);

    while (!stop_requested) {
        int ready = poll(poll_fds, MAX_CLIENTS + 1, 1000);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (poll_fds[0].revents & POLLIN) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd >= 0) {
                int slot = find_free_slot();
                if (slot < 0) {
                    send_message(client_fd, "REJECT server_full\n");
                    close(client_fd);
                } else {
                    poll_fds[slot + 1].fd = client_fd;
                    poll_fds[slot + 1].events = POLLIN;
                    poll_fds[slot + 1].revents = 0;
                    active[slot] = true;
                    requested[slot] = false;
                    client_ids[slot] = -1;
                    last_heartbeat[slot] = time(NULL);
                    printf("[SERVER] New client connected (slot %d)\n", slot);
                }
            } else if (errno != EINTR) {
                perror("accept");
            }
        }

        for (i = 0; i < MAX_CLIENTS; i++) {
            int fd = poll_fds[i + 1].fd;
            short events = poll_fds[i + 1].revents;
            if (fd < 0 || !active[i] || !(events & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) continue;

            if (!(events & POLLIN)) {
                close_client(i, "RECOVERY");
                continue;
            }

            memset(buffer, 0, sizeof(buffer));
            if (receive_message(fd, buffer, sizeof(buffer)) <= 0) {
                close_client(i, "RECOVERY");
                continue;
            }

            if (strncmp(buffer, "REQUEST", 7) == 0) {
                int id;
                unsigned long long remote_clock = 0;
                if (sscanf(buffer, "REQUEST %d %llu", &id, &remote_clock) < 1) continue;
                receive_clock(remote_clock);
                client_ids[i] = id;
                if (requested[i]) {
                    snprintf(buffer, sizeof(buffer), "QUEUED %llu\n", lamport_clock);
                    send_message(fd, buffer);
                    continue;
                }
                requested[i] = true;
                tick_clock();
                log_event("REQUEST", id);
                if (queue_push(i) < 0) {
                    requested[i] = false;
                    send_message(fd, "REJECT queue_full\n");
                } else if (lock_holder_slot == -1) {
                    grant_next_request();
                } else {
                    snprintf(buffer, sizeof(buffer), "QUEUED %llu\n", lamport_clock);
                    send_message(fd, buffer);
                }
            } else if (strncmp(buffer, "HEARTBEAT", 9) == 0) {
                int id;
                unsigned long long remote_clock = 0;
                if (sscanf(buffer, "HEARTBEAT %d %llu", &id, &remote_clock) >= 1 &&
                    lock_holder_slot == i && client_ids[i] == id) {
                    receive_clock(remote_clock);
                    last_heartbeat[i] = time(NULL);
                }
            } else if (strncmp(buffer, "RELEASE", 7) == 0) {
                int id;
                unsigned long long remote_clock = 0;
                if (sscanf(buffer, "RELEASE %d %llu", &id, &remote_clock) >= 1) {
                    receive_clock(remote_clock);
                    if (lock_holder_slot == i && client_ids[i] == id) {
                        tick_clock();
                        log_event("RELEASE", id);
                        lock_holder_slot = -1;
                        requested[i] = false;
                        grant_next_request();
                    }
                }
            }
        }
        check_expired_lease();
    }

    tick_clock();
    log_event("SERVER_STOP", -1);
    for (i = 0; i < MAX_CLIENTS; i++) if (active[i]) close(poll_fds[i + 1].fd);
    close(server_fd);
    if (log_file != NULL) fclose(log_file);
    printf("[SERVER] Stopped safely. Log saved to logs/dlock.log\n");
    return EXIT_SUCCESS;
}
