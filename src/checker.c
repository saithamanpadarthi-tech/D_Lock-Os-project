#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[])
{
    const char *path = argc > 1 ? argv[1] : "logs/dlock.log";
    FILE *file = fopen(path, "r");
    char line[512];
    int active_holder = -1;
    int grants = 0;
    int violations = 0;
    int recoveries = 0;

    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char event[64] = "";
        int client_id = -1;
        if (sscanf(strstr(line, "EVENT=") ? strstr(line, "EVENT=") : line,
                   "EVENT=%63s CLIENT=%d", event, &client_id) < 1)
            continue;

        if (strcmp(event, "SERVER_START") == 0) {
            if (active_holder != -1) {
                printf("[CHECKER] Previous server run ended while Client %d held the lock; treating restart as recovery.\n", active_holder);
                active_holder = -1;
                recoveries++;
            }
        } else if (strcmp(event, "GRANT") == 0) {
            grants++;
            if (active_holder != -1) {
                printf("[VIOLATION] Client %d granted lock while Client %d still appears to hold it.\n",
                       client_id, active_holder);
                violations++;
            }
            active_holder = client_id;
        } else if (strcmp(event, "RELEASE") == 0 ||
                   strcmp(event, "RECOVERY") == 0 ||
                   strcmp(event, "LEASE_EXPIRED") == 0) {
            if (active_holder == client_id) active_holder = -1;
            if (strcmp(event, "RECOVERY") == 0 || strcmp(event, "LEASE_EXPIRED") == 0)
                recoveries++;
        }
    }
    fclose(file);

    printf("\n========== DLOCK SAFETY CHECK =========\n");
    printf("Log file: %s\n", path);
    printf("Lock grants checked: %d\n", grants);
    printf("Recoveries recorded: %d\n", recoveries);
    printf("Safety violations: %d\n", violations);
    if (active_holder != -1)
        printf("Note: log ends while Client %d appears to hold the lock. This may mean the server/client was stopped mid-run.\n", active_holder);
    if (violations == 0)
        printf("RESULT: No overlapping lock grants found in the recorded log.\n");
    else
        printf("RESULT: Potential mutual-exclusion violation found.\n");

    return violations == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
