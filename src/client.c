#define _POSIX_C_SOURCE 200809L
#include "dlock.h"
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

static unsigned long long logical_clock;

static void update_clock(unsigned long long clock_value)
{
    if (clock_value > logical_clock) logical_clock = clock_value;
    logical_clock++;
}

static void send_clocked_message(int fd, const char *kind, int id)
{
    char message[BUFFER_SIZE];
    logical_clock++;
    snprintf(message, sizeof message, "%s %d %llu\n", kind, id, logical_clock);
    send_message(fd, message);
}

static char choose_mode(void)
{
    char line[64];
    for (;;) {
        printf("Select mode: [R]ead or [W]rite: ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) return 0;
        if (tolower((unsigned char)line[0]) == 'r') return 'R';
        if (tolower((unsigned char)line[0]) == 'w') return 'W';
        puts("Please enter R for read or W for write.");
    }
}

#define SHARED_FILE "shared_file.txt"

static void show_shared_file(void)
{
    FILE *file = fopen(SHARED_FILE, "r");
    char line[512];
    if (!file) {
        if (errno == ENOENT) {
            printf("[FILE] %s does not exist yet. It will be created when you write.\n", SHARED_FILE);
            return;
        }
        perror("[FILE] Could not open shared file");
        return;
    }
    printf("\n========== %s ==========\n", SHARED_FILE);
    int has_content = 0;
    while (fgets(line, sizeof line, file)) { fputs(line, stdout); has_content = 1; }
    if (!has_content) puts("(file is empty)");
    printf("========== end of file ==========\n\n");
    fclose(file);
}

static int append_to_shared_file(const char *line)
{
    FILE *file = fopen(SHARED_FILE, "a");
    if (!file) { perror("[FILE] Could not open shared file for writing"); return -1; }
    if (fputs(line, file) == EOF || fflush(file) != 0) {
        perror("[FILE] Could not write shared file"); fclose(file); return -1;
    }
    if (fclose(file) != 0) { perror("[FILE] Could not close shared file"); return -1; }
    return 0;
}

static int clear_shared_file(void)
{
    FILE *file = fopen(SHARED_FILE, "w");
    if (!file) { perror("[FILE] Could not clear shared file"); return -1; }
    if (fclose(file) != 0) { perror("[FILE] Could not close shared file after clearing"); return -1; }
    puts("[FILE] File contents cleared.");
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "Usage: %s <client_id> [read|write]\n", argv[0]);
        fprintf(stderr, "Run without a mode to choose interactively.\n");
        return EXIT_FAILURE;
    }

    char *end = NULL;
    long parsed_id = strtol(argv[1], &end, 10);
    if (!argv[1][0] || !end || *end != '\0' || parsed_id < 0 || parsed_id > 2147483647L) {
        fprintf(stderr, "Client ID must be a non-negative integer.\n");
        return EXIT_FAILURE;
    }
    int id = (int)parsed_id;

    char mode;
    if (argc == 2) {
        mode = choose_mode();
        if (mode == 0) return EXIT_FAILURE;
    } else if (strcasecmp(argv[2], "read") == 0) {
        mode = 'R';
    } else if (strcasecmp(argv[2], "write") == 0) {
        mode = 'W';
    } else {
        fprintf(stderr, "Mode must be read or write.\n");
        return EXIT_FAILURE;
    }

    int fd = connect_to_server("127.0.0.1", SERVER_PORT);
    printf("[CLIENT %d] Connected; requesting %s access.\n", id, mode == 'R' ? "READ" : "WRITE");

    char message[BUFFER_SIZE];
    logical_clock++;
    snprintf(message, sizeof message, "REQUEST %d %c %llu\n", id, mode, logical_clock);
    send_message(fd, message);

    int granted = 0;
    while (!granted) {
        int n = receive_message(fd, message, sizeof message);
        unsigned long long clock_value = 0;
        char granted_mode = 0;
        if (n <= 0) {
            fprintf(stderr, "[CLIENT %d] Server disconnected before lock grant.\n", id);
            close(fd);
            return EXIT_FAILURE;
        }
        if (sscanf(message, "GRANT %c %llu", &granted_mode, &clock_value) == 2) {
            update_clock(clock_value);
            granted = 1;
        } else if (sscanf(message, "QUEUED %llu", &clock_value) == 1) {
            update_clock(clock_value);
            printf("[CLIENT %d] Waiting in the shared FIFO queue for %s access...\n", id,
                   mode == 'R' ? "READ" : "WRITE");
        } else if (strncmp(message, "REJECT", 6) == 0) {
            printf("[CLIENT %d] Request rejected: %s", id, message);
            close(fd);
            return EXIT_FAILURE;
        }
    }

    printf("\n[CLIENT %d] LOCK GRANTED — %s mode is ACTIVE.\n", id,
           mode == 'R' ? "READ" : "WRITE");
    printf("[CLIENT %d] Shared real file: %s\n", id, SHARED_FILE);
    show_shared_file();
    if (mode == 'W') {
        puts("WRITE mode: type a line and press Enter to APPEND it to shared_file.txt.");
        puts("Commands: :show (display file), :clear (empty file), q (release lock).");
    } else {
        puts("READ mode: the file is displayed above. Type r to read it again, or q to release.");
        puts("You cannot change the file in READ mode.");
    }
    printf("> ");
    fflush(stdout);

    int running = 1;
    while (running) {
        struct pollfd descriptors[2];
        descriptors[0].fd = fd; descriptors[0].events = POLLIN; descriptors[0].revents = 0;
        descriptors[1].fd = STDIN_FILENO; descriptors[1].events = POLLIN; descriptors[1].revents = 0;
        int ready = poll(descriptors, 2, HEARTBEAT_INTERVAL * 1000);
        if (ready < 0) { if (errno == EINTR) continue; perror("poll"); break; }

        if (descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            fprintf(stderr, "\n[CLIENT %d] Server connection lost; leaving active mode.\n", id);
            close(fd); return EXIT_FAILURE;
        }
        if (descriptors[0].revents & POLLIN) {
            int n = receive_message(fd, message, sizeof message);
            if (n <= 0 || strncmp(message, "REVOKED", 7) == 0) {
                printf("\n[CLIENT %d] Lock was revoked or the server disconnected.\n", id);
                close(fd); return EXIT_FAILURE;
            }
        }

        if (descriptors[1].revents & (POLLIN | POLLHUP)) {
            char line[512];
            if (!fgets(line, sizeof line, stdin)) break;
            line[strcspn(line, "\r\n")] = '\0';
            if (strcmp(line, "q") == 0 || strcmp(line, "Q") == 0 ||
                strcasecmp(line, "release") == 0 || strcasecmp(line, "exit") == 0) {
                running = 0;
            } else if (strcasecmp(line, ":show") == 0 || strcasecmp(line, "r") == 0) {
                show_shared_file();
            } else if (strcasecmp(line, ":clear") == 0) {
                if (mode == 'W') clear_shared_file();
                else puts("[READ ONLY] You cannot clear the file while in READ mode.");
            } else if (mode == 'W') {
                if (append_to_shared_file(line) == 0 && append_to_shared_file("\n") == 0)
                    puts("[FILE] Line saved to shared_file.txt.");
            } else {
                puts("[READ ONLY] Type r to reread the file or q to release the lock.");
            }
            if (running) { printf("> "); fflush(stdout); }
        }
        send_clocked_message(fd, "HEARTBEAT", id);
    }

    printf("[CLIENT %d] Releasing %s lock...\n", id, mode == 'R' ? "READ" : "WRITE");
    send_clocked_message(fd, "RELEASE", id);
    close(fd);
    printf("[CLIENT %d] Lock released.\n", id);
    return EXIT_SUCCESS;
}
