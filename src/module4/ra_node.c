#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_NODES 10
#define BUFFER_SIZE 256
#define BASE_PORT 9000
#define CRITICAL_SECTION_SECONDS 3

typedef struct {
    int id;
    int port;
} Node;

static Node nodes[MAX_NODES];
static int node_count;
static int my_id;
static int server_fd = -1;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static unsigned long long lamport_clock;
static unsigned long long request_timestamp;

static bool requesting;
static bool in_critical_section;
static bool reply_received[MAX_NODES];
static bool deferred[MAX_NODES];

static int replies_received;
static int request_number;

/* Increment the local Lamport clock. */
static void tick_clock(void)
{
    lamport_clock++;
}

/* Update the local Lamport clock after receiving a message. */
static void receive_clock(unsigned long long remote_clock)
{
    if (remote_clock > lamport_clock)
        lamport_clock = remote_clock;

    lamport_clock++;
}

/* Find a configured node by its ID. */
static bool is_configured_node(int id)
{
    for (int i = 0; i < node_count; i++) {
        if (nodes[i].id == id)
            return true;
    }

    return false;
}

/* Connect to another node on localhost. */
static int connect_to_node(int port)
{
    int fd;
    struct sockaddr_in address;

    fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&address,
                sizeof(address)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/* Send a complete message to another node. */
static int send_message_to_node(int node_id, const char *message)
{
    int fd;
    size_t length = strlen(message);
    size_t sent = 0;

    if (node_id == my_id)
        return 0;

    fd = connect_to_node(BASE_PORT + node_id);

    if (fd < 0) {
        fprintf(stderr,
                "[NODE %d] Cannot connect to node %d\n",
                my_id, node_id);
        return -1;
    }

    while (sent < length) {
        ssize_t n = send(fd, message + sent,
                         length - sent, 0);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0) {
            close(fd);
            return -1;
        }

        sent += (size_t)n;
    }

    close(fd);
    return 0;
}

/* Send permission to a requesting peer. */
static void send_reply(int node_id)
{
    char message[BUFFER_SIZE];
    unsigned long long clock_value;

    pthread_mutex_lock(&mutex);

    tick_clock();
    clock_value = lamport_clock;

    pthread_mutex_unlock(&mutex);

    snprintf(message, sizeof(message),
             "REPLY %d %llu\n", my_id, clock_value);

    if (send_message_to_node(node_id, message) == 0) {
        printf("[NODE %d] Sent REPLY to node %d\n",
               my_id, node_id);
    }
}

/*
 * Ricart–Agrawala rule:
 *
 * Defer a peer's request if we are in the critical section,
 * or if we are requesting the lock and our request has priority.
 *
 * Requests are ordered by (Lamport timestamp, node ID).
 */
static void handle_request(int sender_id,
                           unsigned long long timestamp,
                           unsigned long long remote_clock)
{
    bool defer_reply = false;

    if (sender_id < 0 ||
        sender_id >= MAX_NODES ||
        sender_id == my_id ||
        !is_configured_node(sender_id)) {
        return;
    }

    pthread_mutex_lock(&mutex);

    receive_clock(remote_clock);

    if (in_critical_section) {
        defer_reply = true;
    } else if (requesting) {
        if (request_timestamp < timestamp) {
            defer_reply = true;
        } else if (request_timestamp == timestamp &&
                   my_id < sender_id) {
            defer_reply = true;
        }
    }

    if (defer_reply)
        deferred[sender_id] = true;

    pthread_mutex_unlock(&mutex);

    if (defer_reply) {
        printf("[NODE %d] Deferred REPLY to node %d\n",
               my_id, sender_id);
    } else {
        send_reply(sender_id);
    }
}

/* Process one incoming message. */
static void process_message(const char *buffer)
{
    int sender_id;
    unsigned long long timestamp;
    unsigned long long remote_clock;

    if (sscanf(buffer, "REQUEST %d %llu %llu",
               &sender_id, &timestamp, &remote_clock) == 3) {
        handle_request(sender_id, timestamp, remote_clock);
        return;
    }

    if (sscanf(buffer, "REPLY %d %llu",
               &sender_id, &remote_clock) == 2) {

        if (sender_id < 0 ||
            sender_id >= MAX_NODES ||
            sender_id == my_id ||
            !is_configured_node(sender_id)) {
            return;
        }

        pthread_mutex_lock(&mutex);

        receive_clock(remote_clock);

        /*
         * Count a reply only once from each configured peer.
         * Ignore stale replies when we are not requesting.
         */
        if (requesting && !reply_received[sender_id]) {
            reply_received[sender_id] = true;
            replies_received++;

            printf("[NODE %d] Received REPLY from node %d "
                   "(%d/%d)\n",
                   my_id, sender_id,
                   replies_received, node_count - 1);
        }

        pthread_mutex_unlock(&mutex);
    }
}

/* Listen for incoming requests and replies. */
static void *listener_thread(void *unused)
{
    (void)unused;

    while (1) {
        int client_fd;
        char buffer[BUFFER_SIZE];
        size_t total = 0;

        client_fd = accept(server_fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR)
                continue;

            perror("accept");
            continue;
        }

        /*
         * Read until newline or buffer capacity.
         * TCP does not guarantee one recv() per message.
         */
        while (total < sizeof(buffer) - 1) {
            ssize_t n = recv(client_fd,
                             buffer + total,
                             sizeof(buffer) - 1 - total,
                             0);

            if (n < 0 && errno == EINTR)
                continue;

            if (n <= 0)
                break;

            total += (size_t)n;

            if (memchr(buffer, '\n', total) != NULL)
                break;
        }

        buffer[total] = '\0';
        close(client_fd);

        if (total > 0)
            process_message(buffer);
    }

    return NULL;
}

/* Create the listening TCP socket. */
static int start_server(int port)
{
    int fd;
    int reuse = 1;
    struct sockaddr_in address;

    fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("socket");
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&address,
             sizeof(address)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 32) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

/* Broadcast a lock request to every other configured node. */
static int broadcast_request(unsigned long long timestamp)
{
    char message[BUFFER_SIZE];
    int failed = 0;

    snprintf(message, sizeof(message),
             "REQUEST %d %llu %llu\n",
             my_id, timestamp, timestamp);

    for (int i = 0; i < node_count; i++) {
        if (nodes[i].id == my_id)
            continue;

        if (send_message_to_node(nodes[i].id, message) < 0)
            failed = 1;
    }

    return failed ? -1 : 0;
}

/* Wait until permission arrives from every other node. */
static int wait_for_all_replies(int this_request)
{
    while (1) {
        struct timespec delay = {0, 100000000L};
        int ready;

        pthread_mutex_lock(&mutex);

        ready = replies_received;
        pthread_mutex_unlock(&mutex);

        if (ready >= node_count - 1)
            return 0;

        /*
         * Allow Ctrl+C to interrupt waiting.
         * The caller remains responsible for retrying if a peer
         * is unreachable; this prototype does not recover crashes.
         */
        if (this_request != request_number)
            return -1;

        nanosleep(&delay, NULL);
    }
}

/* Request the lock and enter the critical section. */
static void request_lock(void)
{
    unsigned long long timestamp;
    int this_request;
    int i;

    pthread_mutex_lock(&mutex);

    tick_clock();

    requesting = true;
    in_critical_section = false;

    request_timestamp = lamport_clock;
    timestamp = request_timestamp;

    replies_received = 0;
    memset(reply_received, 0, sizeof(reply_received));

    request_number++;
    this_request = request_number;

    pthread_mutex_unlock(&mutex);

    printf("[NODE %d] Requesting lock at Lamport=%llu\n",
           my_id, timestamp);

    if (broadcast_request(timestamp) < 0) {
        fprintf(stderr,
                "[NODE %d] Could not contact every peer. "
                "Cancelling this request.\n",
                my_id);

        pthread_mutex_lock(&mutex);
        requesting = false;
        pthread_mutex_unlock(&mutex);

        return;
    }

    if (wait_for_all_replies(this_request) < 0)
        return;

    pthread_mutex_lock(&mutex);

    requesting = false;
    in_critical_section = true;

    pthread_mutex_unlock(&mutex);

    printf("\n========================================\n");
    printf("[NODE %d] ENTERING CRITICAL SECTION\n", my_id);
    printf("========================================\n");

    sleep(CRITICAL_SECTION_SECONDS);

    printf("[NODE %d] LEAVING CRITICAL SECTION\n", my_id);

    pthread_mutex_lock(&mutex);

    in_critical_section = false;

    pthread_mutex_unlock(&mutex);

    /*
     * Send all deferred replies after leaving the critical section.
     */
    for (i = 0; i < MAX_NODES; i++) {
        bool send_deferred = false;

        pthread_mutex_lock(&mutex);

        if (deferred[i]) {
            deferred[i] = false;
            send_deferred = true;
        }

        pthread_mutex_unlock(&mutex);

        if (send_deferred)
            send_reply(i);
    }
}

int main(int argc, char *argv[])
{
    pthread_t listener;
    bool seen[MAX_NODES] = {false};
    int i;

    if (argc < 3 || argc > MAX_NODES + 1) {
        fprintf(stderr,
                "Usage: %s <node_id> <peer_id> [peer_id ...]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    my_id = atoi(argv[1]);

    if (my_id < 0 || my_id >= MAX_NODES) {
        fprintf(stderr, "Node ID must be between 0 and 9.\n");
        return EXIT_FAILURE;
    }

    node_count = argc - 2;

    for (i = 2; i < argc; i++) {
        char *end = NULL;
        long parsed = strtol(argv[i], &end, 10);

        if (end == argv[i] || *end != '\0' ||
            parsed < 0 || parsed >= MAX_NODES) {
            fprintf(stderr, "Invalid node ID: %s\n", argv[i]);
            return EXIT_FAILURE;
        }

        int id = (int)parsed;

        if (seen[id]) {
            fprintf(stderr, "Duplicate node ID: %d\n", id);
            return EXIT_FAILURE;
        }

    	seen[id] = true;
    	nodes[i - 2].id = id;
	nodes[i - 2].port = BASE_PORT + id;
    }

    if (!seen[my_id]) {
        fprintf(stderr,
                "The node list must include your own ID (%d).\n",
                my_id);
        return EXIT_FAILURE;
    }

    server_fd = start_server(BASE_PORT + my_id);

    if (server_fd < 0)
        return EXIT_FAILURE;

    if (pthread_create(&listener, NULL,
                       listener_thread, NULL) != 0) {
        perror("pthread_create");
        close(server_fd);
        return EXIT_FAILURE;
    }

    pthread_detach(listener);

    printf("[NODE %d] Listening on port %d\n",
           my_id, BASE_PORT + my_id);

    printf("Configured nodes:");

    for (i = 0; i < node_count; i++)
        printf(" %d", nodes[i].id);

    printf("\nPress Enter to request the lock, or Ctrl+C to exit.\n");

    while (getchar() != EOF)
        request_lock();

    close(server_fd);
    return EXIT_SUCCESS;
}
