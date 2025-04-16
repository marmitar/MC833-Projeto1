#include <liburing.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "./database/database.h"
#include "./defines.h"
#include "./worker/worker.h"

static constexpr const uint16_t PORT = 12'345;
static constexpr const int BACKLOG = 5;

extern int main(void) {
    // initialize sqlite
    const char *errmsg = NULL;
    bool setup_ok = db_setup(DATABASE, &errmsg);
    if unlikely (!setup_ok) {
        fprintf(stderr, "db_setup: %s\n", errmsg);
        db_free_errmsg(errmsg);
        return EXIT_FAILURE;
    }

    // initialize io_uring
    struct io_uring ring;
    bool ok = uring_init(&ring);
    if unlikely (!ok) {
        return EXIT_FAILURE;
    }

    // initialize socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if unlikely (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int yes = 1;
    struct timeval tv = {.tv_sec = 300, .tv_usec = 0};
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));  // NOLINT(misc-include-cleaner)
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));     // NOLINT(misc-include-cleaner)
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if unlikely (bind(server_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }
    if unlikely (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("server listening on port %d\n", PORT);
    if (post_accept(&ring, server_fd) == 0) {
        io_uring_submit(&ring);
    }

    const unsigned n = cpu_count();
    pthread_t *workers = malloc(n * sizeof(pthread_t));
    if unlikely (workers == NULL) {
        perror("malloc");
        close(server_fd);
        return EXIT_FAILURE;
        ;
    }

    for (unsigned i = 0; i < n; i++) {
        bool ok = start_worker(&(workers[i]), &ring);
        if unlikely (!ok) {
            workers[i] = 0;
        }
    }

    // start accepting
    while (true) {
        if (post_accept(&ring, server_fd) == 0) {
            io_uring_submit(&ring);
        }
        sleep(100);
    }

    close(server_fd);
    return EXIT_SUCCESS;
}
