#ifndef SRC_PARSER_H
/** Database operation parser. */
#define SRC_PARSER_H

#include <stdbool.h>

#include <pthread.h>

#include <liburing.h>

#include "../defines.h"

unsigned cpu_count(void);

struct worker_context {
    pthread_mutex_t mutex;
    struct io_uring ring;
};

[[gnu::nonnull(1, 2), gnu::nothrow]]
bool start_worker(pthread_t *NONNULL id, struct worker_context *NONNULL ctx);

[[gnu::nonnull(1), gnu::leaf, gnu::nothrow]]
bool uring_init(struct io_uring *NONNULL ring);

[[gnu::nonnull(1), gnu::leaf, gnu::nothrow]]
bool post_accept(struct io_uring *NONNULL ring, int server_fd);

#endif
