#include "./defines.h"
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "./database/database.h"
#include "./net/handler.h"

static constexpr const uint16_t PORT = 12'345;
static constexpr const int BACKLOG = 5;

extern int main(void) {
    // Suppose the DB is local
    const char *NONNULL errmsg[1] = {NULL};
    bool setup_ok = db_setup("movies.db", errmsg);
    if (!setup_ok) {
        fprintf(stderr, "db_setup: %s\n", errmsg[0]);
        db_free_errmsg(errmsg[0]);
        return 1;
    }

    db_conn *db = db_connect("movies.db", errmsg);
    if (!db) {
        fprintf(stderr, "db_connect: %s\n", errmsg[0]);
        db_free_errmsg(errmsg[0]);
        return 1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));  // NOLINT(misc-include-cleaner)

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(server_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("Server listening on port %d\n", PORT);

    bool abort = false;
    while (!abort) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *) &client_addr, &addrlen);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        // Single-thread approach: handle one client at a time
        abort = handle_request(client_fd, db);
        close(client_fd);
    }

    db_disconnect(db, NULL);
    close(server_fd);
    return abort ? EXIT_FAILURE : EXIT_SUCCESS;
}
