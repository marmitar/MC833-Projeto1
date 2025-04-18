#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <liburing.h>

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
    struct context *ctx = calloc(1, sizeof(struct context));
    bool ok = uring_init(&(ctx->ring));
    if unlikely (!ok) {
        return EXIT_FAILURE;
    }
    int rv = pthread_mutex_init(&(ctx->mutex), NULL);
    if unlikely(rv != 0) {
        return EXIT_FAILURE;
    }

    // initialize socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if unlikely (server_fd < 0) {
        perror("socket");
        pthread_mutex_destroy(&(ctx->mutex));
        return EXIT_FAILURE;
    }

    int yes = 1;
    struct timeval tv = {.tv_sec = 60, .tv_usec = 0};
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
        pthread_mutex_destroy(&(ctx->mutex));
        return EXIT_FAILURE;
    }
    if unlikely (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        pthread_mutex_destroy(&(ctx->mutex));
        return EXIT_FAILURE;
    }

    printf("server listening on port %d\n", PORT);
    if likely(post_accept(&(ctx->ring), server_fd)) {
        io_uring_submit(&(ctx->ring));
    }

    const unsigned n = cpu_count();
    pthread_t *workers = malloc(n * sizeof(pthread_t));
    if unlikely (workers == NULL) {
        perror("malloc");
        close(server_fd);
        pthread_mutex_destroy(&(ctx->mutex));
        return EXIT_FAILURE;
        ;
    }

    for (unsigned i = 0; i < n; i++) {
        bool ok = start_worker(&(workers[i]), ctx);
        if unlikely (!ok) {
            workers[i] = 0;
        }
    }

    // start accepting
    while (true) {
        rv = pthread_mutex_lock(&(ctx->mutex));
        if unlikely(rv != 0) {
            break;
        }
        if likely(post_accept(&(ctx->ring), server_fd)) {
            io_uring_submit(&(ctx->ring));
        }
        pthread_mutex_unlock(&(ctx->mutex));

        sleep(1);
    }

    for (unsigned i = 0; i < n; i++) {
        pthread_join(workers[i], NULL);
    }
    pthread_mutex_destroy(&(ctx->mutex));
    close(server_fd);
    return EXIT_SUCCESS;
}
