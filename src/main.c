#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bits/types/struct_timeval.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "./database/database.h"
#include "./defines.h"
#include "./worker/worker.h"

static int start_server(void) {
    static constexpr const uint16_t PORT = 12'345;
    static constexpr const int BACKLOG = 5;
    static constexpr const struct timeval SOCKET_TIMEOUT = {.tv_sec = 60, .tv_usec = 0};

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if unlikely (server_fd < 0) {
        perror("socket");
        return -1;
    }

    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));  // NOLINT(misc-include-cleaner)
    setsockopt(
        server_fd,
        SOL_SOCKET,
        SO_RCVTIMEO,  // NOLINT(misc-include-cleaner)
        &SOCKET_TIMEOUT,
        sizeof(SOCKET_TIMEOUT)
    );
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if unlikely (bind(server_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }
    if unlikely (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    printf("server listening on port %d\n", PORT);
    return server_fd;
}

extern int main(void) {
    // initialize sqlite
    const char *errmsg = NULL;
    bool setup_ok = db_setup(DATABASE, &errmsg);
    if unlikely (!setup_ok) {
        (void) fprintf(stderr, "db_setup: %s\n", errmsg);
        db_free_errmsg(errmsg);
        return EXIT_FAILURE;
    }

    // initialize worker threads
    workers_t *workers = workers_start();
    if unlikely (workers == NULL) {
        perror("workers_start");
        return EXIT_FAILURE;
    }

    // initialize socket
    int server_fd = start_server();
    if unlikely (server_fd < 0) {
        workers_stop(workers);
        return EXIT_FAILURE;
    }

    // start accepting
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *) &client_addr, &addrlen);
        if unlikely (client_fd < 0) {
            continue;
        }

        bool ok = workers_add_work(workers, client_fd);
        if unlikely (!ok) {
            close(client_fd);
            (void) fprintf(stderr, "workers_add_work: no worker thread to handle request\n");
            break;
        }
    }

    close(server_fd);
    workers_stop(workers);
    return EXIT_SUCCESS;
}
