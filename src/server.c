#include "dlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>

typedef struct
{
    int client_id;
    int socket_fd;
} Request;

Request request_queue[MAX_CLIENTS];

int queue_front = 0;
int queue_rear = 0;

int lock_holder = -1;

void enqueue(int client_id, int socket_fd)
{
    if (queue_rear >= MAX_CLIENTS)
    {
        printf("[SERVER] Queue is full\n");
        return;
    }

    request_queue[queue_rear].client_id = client_id;
    request_queue[queue_rear].socket_fd = socket_fd;

    queue_rear++;
}

Request dequeue_request(void)
{
    Request request;

    request.client_id = -1;
    request.socket_fd = -1;

    if (queue_front >= queue_rear)
    {
        return request;
    }

    request = request_queue[queue_front];

    queue_front++;

    return request;
}

int queue_empty(void)
{
    return queue_front >= queue_rear;
}

void grant_next_request(void)
{
    Request request;

    if (lock_holder != -1)
    {
        return;
    }

    if (queue_empty())
    {
        return;
    }

    request = dequeue_request();

    lock_holder = request.client_id;

    send_message(request.socket_fd, "GRANT\n");

    printf("[SERVER] Lock granted to Client %d\n",
           request.client_id);
}

int main(void)
{
    int server_fd;

    struct pollfd client_fds[MAX_CLIENTS + 1];

    int client_ids[MAX_CLIENTS + 1];

    int number_of_fds = 1;

    char buffer[BUFFER_SIZE];

    printf("\n");
    printf("====================================\n");
    printf("         DLOCK LOCK SERVER\n");
    printf("====================================\n");

    server_fd = create_server_socket(SERVER_PORT);

    client_fds[0].fd = server_fd;
    client_fds[0].events = POLLIN;

    client_ids[0] = -1;

    printf("[SERVER] Listening on TCP port %d\n",
           SERVER_PORT);

    while (1)
    {
        int ready;

        ready = poll(client_fds,
                     number_of_fds,
                     -1);

        if (ready < 0)
        {
            perror("poll");
            break;
        }

        if (client_fds[0].revents & POLLIN)
        {
            int client_fd;
            int i;

            client_fd = accept(server_fd,
                               NULL,
                               NULL);

            if (client_fd < 0)
            {
                perror("accept");
                continue;
            }

            for (i = 1; i <= MAX_CLIENTS; i++)
            {
                if (i >= number_of_fds)
                {
                    client_fds[i].fd = client_fd;
                    client_fds[i].events = POLLIN;

                    client_ids[i] = -1;

                    number_of_fds++;

                    printf("[SERVER] New client connected\n");

                    break;
                }
            }
        }

        {
            int i;

            for (i = 1; i < number_of_fds; i++)
            {
                if (!(client_fds[i].revents & POLLIN))
                {
                    continue;
                }

                memset(buffer,
                       0,
                       sizeof(buffer));

                if (receive_message(client_fds[i].fd,
                                    buffer,
                                    sizeof(buffer)) <= 0)
                {
                    printf("[SERVER] Client disconnected\n");

                    if (client_ids[i] == lock_holder)
                    {
                        lock_holder = -1;

                        printf("[SERVER] Lock released because client disconnected\n");
                    }

                    close(client_fds[i].fd);

                    client_fds[i] =
                        client_fds[number_of_fds - 1];

                    client_ids[i] =
                        client_ids[number_of_fds - 1];

                    number_of_fds--;

                    i--;

                    grant_next_request();

                    continue;
                }

                printf("[SERVER] Received: %s",
                       buffer);

                if (strncmp(buffer,
                            "REQUEST",
                            7) == 0)
                {
                    int client_id;

                    sscanf(buffer,
                           "REQUEST %d",
                           &client_id);

                    client_ids[i] = client_id;

                    enqueue(client_id,
                            client_fds[i].fd);

                    printf("[SERVER] Client %d added to queue\n",
                           client_id);

                    if (lock_holder == -1)
                    {
                        grant_next_request();
                    }
                    else
                    {
                        send_message(client_fds[i].fd,
                                     "QUEUED\n");
                    }
                }

                else if (strncmp(buffer,
                                 "RELEASE",
                                 7) == 0)
                {
                    int client_id;

                    sscanf(buffer,
                           "RELEASE %d",
                           &client_id);

                    if (client_id == lock_holder)
                    {
                        lock_holder = -1;

                        printf("[SERVER] Client %d released the lock\n",
                               client_id);

                        grant_next_request();
                    }
                }
            }
        }
    }

    close(server_fd);

    return 0;
}
