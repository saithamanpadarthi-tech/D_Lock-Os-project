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
#include <sys/stat.h>

#define QUEUE_CAPACITY MAX_CLIENTS
#define MODE_NONE 0
#define MODE_READ 'R'
#define MODE_WRITE 'W'

typedef struct { int client_id; int slot; char mode; } QueueEntry;
static struct pollfd poll_fds[MAX_CLIENTS + 1];
static int client_ids[MAX_CLIENTS];
static bool active[MAX_CLIENTS], requested[MAX_CLIENTS];
static char held_mode[MAX_CLIENTS];
static time_t last_heartbeat[MAX_CLIENTS];
static QueueEntry queue_entries[QUEUE_CAPACITY];
static int queue_head, queue_tail, queue_count;
static unsigned long long lamport_clock;
static FILE *log_file;
static volatile sig_atomic_t stop_requested;

static void handle_signal(int n) { (void)n; stop_requested = 1; }
static void tick_clock(void) { lamport_clock++; }
static void receive_clock(unsigned long long c) { if (c > lamport_clock) lamport_clock = c; lamport_clock++; }
static void log_event(const char *event, int id) {
    time_t now = time(NULL); struct tm ti; char stamp[32];
    localtime_r(&now, &ti); strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &ti);
    if (log_file) { fprintf(log_file, "%s LAMPORT=%llu EVENT=%s CLIENT=%d\n", stamp, lamport_clock, event, id); fflush(log_file); }
    printf("[SERVER] LAMPORT=%llu EVENT=%s CLIENT=%d\n", lamport_clock, event, id);
}
static void load_last_clock(void) {
    char line[512]; unsigned long long value; FILE *in = fopen("logs/dlock.log", "r"); if (!in) return;
    while (fgets(line, sizeof line, in)) { char *p = strstr(line, "LAMPORT="); if (p && sscanf(p, "LAMPORT=%llu", &value) == 1 && value > lamport_clock) lamport_clock = value; }
    fclose(in);
}
static int queue_push(int slot, char mode) {
    if (queue_count >= QUEUE_CAPACITY) return -1;
    queue_entries[queue_tail] = (QueueEntry){ client_ids[slot], slot, mode };
    queue_tail = (queue_tail + 1) % QUEUE_CAPACITY; queue_count++; return 0;
}
static QueueEntry queue_pop(void) {
    QueueEntry e = {-1, -1, MODE_NONE}; if (!queue_count) return e;
    e = queue_entries[queue_head]; queue_head = (queue_head + 1) % QUEUE_CAPACITY; queue_count--; return e;
}
static int lock_active(void) {
    /* DLock uses one exclusive lock for BOTH READ and WRITE operations. */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (active[i] && held_mode[i] != MODE_NONE) return 1;
    }
    return 0;
}
static int entry_valid(QueueEntry e) {
    return e.slot >= 0 && e.slot < MAX_CLIENTS && active[e.slot] && requested[e.slot] &&
           client_ids[e.slot] == e.client_id && poll_fds[e.slot+1].fd >= 0;
}
static void grant_next_requests(void) {
    /* Strict FIFO: never grant a second operation while any client holds the lock. */
    if (lock_active()) return;

    while (queue_count > 0) {
        QueueEntry e = queue_pop();
        if (!entry_valid(e)) continue;

        int slot = e.slot;
        char msg[BUFFER_SIZE];
        held_mode[slot] = e.mode;
        last_heartbeat[slot] = time(NULL);
        tick_clock();
        log_event(e.mode == MODE_READ ? "GRANT_READ" : "GRANT_WRITE", e.client_id);
        snprintf(msg, sizeof msg, "GRANT %c %llu\n", e.mode, lamport_clock);
        send_message(poll_fds[slot + 1].fd, msg);
        return; /* Exactly one active operation, regardless of READ/WRITE mode. */
    }
}
static void close_client(int slot, const char *reason) {
    if (slot < 0 || slot >= MAX_CLIENTS || !active[slot]) return;
    int id = client_ids[slot]; bool was_holder = held_mode[slot] != MODE_NONE;
    if (was_holder) { tick_clock(); log_event(reason, id); }
    else if (id >= 0) { tick_clock(); log_event("CLIENT_DISCONNECTED", id); }
    int fd = poll_fds[slot+1].fd; if (fd >= 0) close(fd);
    poll_fds[slot+1].fd = -1; poll_fds[slot+1].events = POLLIN;
    client_ids[slot] = -1; active[slot] = false; requested[slot] = false;
    held_mode[slot] = MODE_NONE; last_heartbeat[slot] = 0;
    grant_next_requests();
}
static int find_free_slot(void) { for (int i=0;i<MAX_CLIENTS;i++) if (!active[i]) return i; return -1; }
static void check_expired_leases(void) {
    time_t now = time(NULL);
    for (int i=0;i<MAX_CLIENTS;i++) {
        if (active[i] && held_mode[i] != MODE_NONE && now-last_heartbeat[i] >= LEASE_TIMEOUT) {
            char msg[BUFFER_SIZE]; tick_clock(); log_event("LEASE_EXPIRED", client_ids[i]);
            snprintf(msg, sizeof msg, "REVOKED %llu\n", lamport_clock);
            if (poll_fds[i+1].fd >= 0) send_message(poll_fds[i+1].fd, msg);
            close_client(i, "RECOVERY");
        }
    }
}
int main(void) {
    signal(SIGINT, handle_signal); signal(SIGTERM, handle_signal); mkdir("logs",0755); load_last_clock();
    log_file = fopen("logs/dlock.log", "a"); if (!log_file) { perror("logs/dlock.log"); return EXIT_FAILURE; }
    int server_fd = create_server_socket(SERVER_PORT); char buffer[BUFFER_SIZE];
    poll_fds[0] = (struct pollfd){ .fd=server_fd, .events=POLLIN };
    for (int i=0;i<MAX_CLIENTS;i++) { poll_fds[i+1].fd=-1; poll_fds[i+1].events=POLLIN; client_ids[i]=-1; }
    tick_clock(); log_event("SERVER_START", -1);
    printf("[SERVER] Strict FIFO exclusive read/write lock server listening on TCP port %d; lease timeout=%d seconds\n", SERVER_PORT, LEASE_TIMEOUT);
    while (!stop_requested) {
        int ready = poll(poll_fds, MAX_CLIENTS+1, 1000);
        if (ready < 0) { if (errno == EINTR) continue; perror("poll"); break; }
        if (poll_fds[0].revents & POLLIN) {
            int fd = accept(server_fd, NULL, NULL);
            if (fd >= 0) { int s=find_free_slot(); if (s<0) { send_message(fd,"REJECT server_full\n"); close(fd); }
                else { poll_fds[s+1]=(struct pollfd){.fd=fd,.events=POLLIN}; active[s]=true; requested[s]=false; held_mode[s]=MODE_NONE; client_ids[s]=-1; last_heartbeat[s]=time(NULL); printf("[SERVER] Client connected (slot %d)\n",s); } }
        }
        for (int i=0;i<MAX_CLIENTS;i++) {
            int fd=poll_fds[i+1].fd; short ev=poll_fds[i+1].revents;
            if (fd<0 || !active[i] || !(ev&(POLLIN|POLLHUP|POLLERR|POLLNVAL))) continue;
            if (!(ev&POLLIN)) { close_client(i,"RECOVERY"); continue; }
            memset(buffer,0,sizeof buffer); if (receive_message(fd,buffer,sizeof buffer)<=0) { close_client(i,"RECOVERY"); continue; }
            if (strncmp(buffer,"REQUEST",7)==0) {
                int id; char mode; unsigned long long remote=0;
                if (sscanf(buffer,"REQUEST %d %c %llu",&id,&mode,&remote)<2 || (mode!=MODE_READ && mode!=MODE_WRITE)) { send_message(fd,"REJECT invalid_request\n"); continue; }
                receive_clock(remote); client_ids[i]=id;
                if (requested[i] || held_mode[i]!=MODE_NONE) { send_message(fd,"REJECT duplicate_request\n"); continue; }
                requested[i]=true; tick_clock(); log_event(mode==MODE_READ?"REQUEST_READ":"REQUEST_WRITE",id);
                if (queue_push(i,mode)<0) { requested[i]=false; send_message(fd,"REJECT queue_full\n"); }
                else { int before=held_mode[i]; (void)before; grant_next_requests(); if (held_mode[i]==MODE_NONE) { snprintf(buffer,sizeof buffer,"QUEUED %llu\n",lamport_clock); send_message(fd,buffer); } }
            } else if (strncmp(buffer,"HEARTBEAT",9)==0) {
                int id; unsigned long long remote=0;
                if (sscanf(buffer,"HEARTBEAT %d %llu",&id,&remote)>=1 && held_mode[i]!=MODE_NONE && client_ids[i]==id) { receive_clock(remote); last_heartbeat[i]=time(NULL); }
            } else if (strncmp(buffer,"RELEASE",7)==0) {
                int id; unsigned long long remote=0;
                if (sscanf(buffer,"RELEASE %d %llu",&id,&remote)>=1) { receive_clock(remote);
                    if (held_mode[i]!=MODE_NONE && client_ids[i]==id) { tick_clock(); log_event(held_mode[i]==MODE_READ?"RELEASE_READ":"RELEASE_WRITE",id); held_mode[i]=MODE_NONE; requested[i]=false; grant_next_requests(); }
                }
            }
        }
        check_expired_leases();
    }
    tick_clock(); log_event("SERVER_STOP",-1); for (int i=0;i<MAX_CLIENTS;i++) if (active[i]) close(poll_fds[i+1].fd);
    close(server_fd); if (log_file) fclose(log_file); puts("[SERVER] Stopped safely."); return EXIT_SUCCESS;
}
