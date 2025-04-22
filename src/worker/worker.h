#ifndef SRC_PARSER_H
/** Database operation parser. */
#define SRC_PARSER_H

#include <stdbool.h>

#include <bits/pthreadtypes.h>

#include "../defines.h"
#include "./queue.h"

[[gnu::leaf, gnu::nothrow]]
unsigned cpu_count(void);

[[gnu::nonnull(1, 2), gnu::nothrow]]
bool start_worker(pthread_t *NONNULL id, workq_t *NONNULL queue);

#endif
