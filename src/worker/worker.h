#ifndef SRC_PARSER_H
/** Database operation parser. */
#define SRC_PARSER_H

#include <stdbool.h>

#include "../defines.h"

/** Expected number of worker threads running. */
#define WORKERS_CAPACITY 128

/** Optimal alignment for `workers_t`. */
#define WORKERS_ALIGNMENT 128

/** Opaque handle to the list of worker threads. */
typedef struct worker_list workers_t [[gnu::aligned(WORKERS_ALIGNMENT)]];

[[gnu::malloc, gnu::assume_aligned(WORKERS_ALIGNMENT), gnu::cold, gnu::leaf, gnu::nothrow]]
/**
 * Starts `WORKERS_CAPACITY` threads for handling TCP requests.
 *
 * Each worker waits for sockets from `workers_add_work`.
 *
 * Returns the list with all threads running, or `NULL` if any failure occurs during initialization.
 */
workers_t *NULLABLE workers_start(void);

[[gnu::cold, gnu::nonnull(1), gnu::leaf, gnu::nothrow]]
/**
 * Stop all currently running worker threads and deallocate memory.
 */
void workers_stop(workers_t *NONNULL workers);

[[gnu::hot, gnu::nonnull(1), gnu::leaf, gnu::nothrow]]
/**
 * Adds `socket_fd` to the worker queue and signal worker threads that a new connection is open.
 *
 * This function also tries to restart worker thread that died for some reason.
 *
 * Returns true if successful, or false if all workers are dead.
 */
bool workers_add_work(workers_t *NONNULL workers, int socket_fd);

#endif
