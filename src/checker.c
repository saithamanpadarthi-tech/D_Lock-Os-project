#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRACKED_CLIENTS 10000

int main(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : "logs/dlock.log";
    FILE *file = fopen(path, "r");
    char line[512];
    char held[TRACKED_CLIENTS] = {0};
    int grants = 0, violations = 0, recoveries = 0, active_count = 0;

    if (!file) { perror(path); return EXIT_FAILURE; }
    while (fgets(line, sizeof line, file)) {
        char event[64] = "";
        int id = -1;
        char *p = strstr(line, "EVENT=");
        if (!p || sscanf(p, "EVENT=%63s CLIENT=%d", event, &id) < 2) continue;
        if (strcmp(event, "SERVER_START") == 0) {
            memset(held, 0, sizeof held);
            active_count = 0;
            continue;
        }
        if (id < 0 || id >= TRACKED_CLIENTS) continue;

        if (strcmp(event, "GRANT_READ") == 0 || strcmp(event, "GRANT_WRITE") == 0) {
            grants++;
            if (active_count > 0) {
                printf("[VIOLATION] Client %d granted %s while another operation is active.\n",
                       id, strcmp(event, "GRANT_READ") == 0 ? "READ" : "WRITE");
                violations++;
            }
            if (held[id]) {
                printf("[VIOLATION] Client %d received a duplicate grant.\n", id);
                violations++;
            } else {
                held[id] = strcmp(event, "GRANT_READ") == 0 ? 'R' : 'W';
                active_count++;
            }
        } else if (strcmp(event, "RELEASE_READ") == 0 ||
                   strcmp(event, "RELEASE_WRITE") == 0 ||
                   strcmp(event, "RECOVERY") == 0 ||
                   strcmp(event, "LEASE_EXPIRED") == 0) {
            if (held[id]) { held[id] = 0; active_count--; }
            if (strcmp(event, "RECOVERY") == 0 || strcmp(event, "LEASE_EXPIRED") == 0)
                recoveries++;
        }
    }
    fclose(file);
    printf("\n========== DLOCK STRICT FIFO MUTEX SAFETY CHECK =========\n"
           "Log file: %s\nGrants checked: %d\nRecoveries recorded: %d\n"
           "Safety violations: %d\n", path, grants, recoveries, violations);
    if (violations == 0) puts("RESULT: No overlapping READ/WRITE operations found in the recorded log.");
    else puts("RESULT: Potential strict mutual-exclusion violation found.");
    return violations ? EXIT_FAILURE : EXIT_SUCCESS;
}
