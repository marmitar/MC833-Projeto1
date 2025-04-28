#ifndef SRC_PARSER_H
/** Database operation parser. */
#define SRC_PARSER_H

#include <stdbool.h>

/** Expected number of worker threads running. */
#define WORKERS_CAPACITY 128

[[gnu::cold, gnu::leaf, gnu::nothrow]]
/**
 * Starts `WORKERS_CAPACITY` threads for handling TCP requests.
 *
 * Each worker waits for sockets from `workers_add_work`.
 *
 * Returns the list with all threads running, or `NULL` if any failure occurs during initialization.
 */
bool workers_start(void);

[[gnu::cold, gnu::leaf, gnu::nothrow]]
/**
 * Stop all currently running worker threads and deallocate memory.
 */
void workers_stop(void);

[[gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Adds `socket_fd` to the worker queue and signal worker threads that a new connection is open.
 *
 * This function also tries to restart worker thread that died for some reason.
 *
 * Returns true if successful, or false if all workers are dead.
 */
bool workers_add_work(int socket_fd);

[[gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Returns true if main thread received a signal for shutdown.
 */
bool was_shutdown_requested(void);

#endif
