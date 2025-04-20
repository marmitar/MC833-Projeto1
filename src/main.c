#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bits/pthreadtypes.h>
#include <bits/types/struct_timeval.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>

#include "./alloc.h"
#include "./database/database.h"
#include "./defines.h"
#include "./worker/queue.h"
#include "./worker/worker.h"

static constexpr const uint16_t PORT = 12'345;
static constexpr const int BACKLOG = 5;
static constexpr const struct timeval SOCKET_TIMEOUT = {.tv_sec = 60, .tv_usec = 0};

extern int main(void) {
    // initialize sqlite
    const char *errmsg = NULL;
    bool setup_ok = db_setup(DATABASE, &errmsg);
    if unlikely (!setup_ok) {
        (void) fprintf(stderr, "db_setup: %s\n", errmsg);
        db_free_errmsg(errmsg);
        return EXIT_FAILURE;
    }

    // initialize io_uring
    workq_t *queue = workq_create();
    if unlikely (!queue) {
        (void) fprintf(stderr, "workq_create: out of memory\n");
        return EXIT_FAILURE;
    }

    // initialize socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if unlikely (server_fd < 0) {
        perror("socket");
        workq_destroy(queue);
        return EXIT_FAILURE;
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
        workq_destroy(queue);
        return EXIT_FAILURE;
    }
    if unlikely (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        workq_destroy(queue);
        return EXIT_FAILURE;
    }

    printf("server listening on port %d\n", PORT);

    const unsigned n = cpu_count();
    pthread_t *workers = calloc_like(pthread_t, n);
    if unlikely (workers == NULL) {
        perror("malloc");
        close(server_fd);
        workq_destroy(queue);
        return EXIT_FAILURE;
    }

    for (unsigned i = 0; i < n; i++) {
        bool ok = start_worker(&(workers[i]), queue);
        if unlikely (!ok) {
            workers[i] = pthread_self();
        }
    }

    // start accepting
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *) &client_addr, &addrlen);
        if unlikely (client_fd < 0) {
            continue;
        }

        bool ok = workq_push(queue, client_fd);
        if unlikely (!ok) {
            close(client_fd);
            (void) fprintf(stderr, "workq_push: full, stopping the server\n");
            break;
        }
    }

    for (unsigned i = 0; i < n; i++) {
        if likely (workers[i] != pthread_self()) {
            // FIXME: signal the workers
            pthread_join(workers[i], NULL);
        }
    }

    close(server_fd);
    workq_destroy(queue);
    return EXIT_SUCCESS;
}
