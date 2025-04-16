#ifndef SRC_PARSER_H
/** Database operation parser. */
#define SRC_PARSER_H

#include <liburing.h>
#include <pthread.h>
#include <stdint.h>

#include "../defines.h"

unsigned cpu_count(void);

[[gnu::regcall, gnu::nonnull(1, 2), gnu::nothrow]]
bool start_worker(pthread_t *NONNULL id, struct io_uring *NONNULL uring);

[[gnu::regcall, gnu::nonnull(1), gnu::leaf, gnu::nothrow]]
bool uring_init(struct io_uring *NONNULL ring);

[[gnu::regcall, gnu::nonnull(1), gnu::leaf, gnu::nothrow]]
bool post_accept(struct io_uring *NONNULL ring, int server_fd);

#endif
