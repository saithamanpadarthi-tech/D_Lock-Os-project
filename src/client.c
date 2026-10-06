#include "dlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    int client_id;
    int socket_fd;

    char buffer[BUFFER_SIZE];

    if (argc != 2)
    {
        printf("Usage: %s <client_id>\n",
               argv[0]);

        return EXIT_FAILURE;
    }

    client_id = atoi(argv[1]);

    socket_fd = connect_to_server("127.0.0.1",
                                  SERVER_PORT);

    printf("[CLIENT %d] Connected to DLock server\n",
           client_id);

    snprintf(buffer,
             sizeof(buffer),
             "REQUEST %d\n",
             client_id);

    send_message(socket_fd,
                 buffer);

    printf("[CLIENT %d] Lock requested\n",
           client_id);

    while (1)
    {
        memset(buffer,
               0,
               sizeof(buffer));

        if (receive_message(socket_fd,
                            buffer,
                            sizeof(buffer)) <= 0)
        {
            printf("[CLIENT %d] Server disconnected\n",
                   client_id);

            close(socket_fd);

            return EXIT_FAILURE;
        }

        if (strncmp(buffer,
                    "QUEUED",
                    6) == 0)
        {
            printf("[CLIENT %d] Waiting in FIFO queue...\n",
                   client_id);
        }

        else if (strncmp(buffer,
                         "GRANT",
                         5) == 0)
        {
            break;
        }
    }

    printf("\n");
    printf("========================================\n");
    printf("[CLIENT %d] ENTERING CRITICAL SECTION\n",
           client_id);
    printf("========================================\n");

    sleep(5);

    printf("[CLIENT %d] Leaving critical section\n",
           client_id);

    snprintf(buffer,
             sizeof(buffer),
             "RELEASE %d\n",
             client_id);

    send_message(socket_fd,
                 buffer);

    printf("[CLIENT %d] Lock released\n",
           client_id);

    close(socket_fd);

    return EXIT_SUCCESS;
}
