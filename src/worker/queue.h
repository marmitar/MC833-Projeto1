#ifndef SCR_WORKER_QUEUE_H
/** Concurrent work queue. */
#define SCR_WORKER_QUEUE_H

#include <stdbool.h>
#include <stddef.h>

#include "../defines.h"

/**
 * Assumed size for the cache line.
 *
 * Used to separate atomic variables from synchronization variables, that are used in different code paths.
 */
static constexpr const size_t CACHE_LINE_SIZE = 64;

/** Maximum number of items that can be in the queue at a single time. */
#define WORK_QUEUE_CAPACITY 128

/**
 * An opaque handle to the concurrent work queue.
 */
typedef struct work_queue workq_t [[gnu::aligned(2 * CACHE_LINE_SIZE)]];

/**
 * The content of the work queue.
 *
 * Currently, a socket file descriptor (`int`).
 */
typedef int work_item;

[[nodiscard("might need to destroy queue"),
  gnu::malloc,
  gnu::assume_aligned(2 * CACHE_LINE_SIZE),
  gnu::cold,
  gnu::leaf,
  gnu::nothrow]]
/**
 * Allocate memory for the work queue and initialize its synchronization variables.
 *
 * Returns the a pointer to the new work queue, or `NULL` on out-of-memory situations or other initialization issues.
 */
workq_t *NULLABLE workq_create(void);

[[gnu::nonnull(1), gnu::cold, gnu::leaf, gnu::nothrow]]
/**
 * Deallocates memory for the work queue and destroy its synchronization variables.
 *
 * Returns `true` if destruction was successful, or `false` if the synchronization variables are still in use.
 */
void workq_destroy(workq_t *NONNULL queue);

[[gnu::nonnull(1), gnu::leaf, gnu::nothrow]]
/**
 * Add an item to the queue and signal other threads about it.
 *
 * Warning: not thread safe. Should only be called by the main thread.
 *
 * Returns `true` if the item was inserted successfully, or `false` if the queue is full or the mutex lock could not be
 * acquired.
 */
bool workq_push(workq_t *NONNULL queue, work_item item);

[[nodiscard("item will be uninitialized on false"), gnu::nonnull(1, 2), gnu::leaf, gnu::nothrow]]
/**
 * Remove an item from the queue.
 *
 * Returns `true` if an item was removed successfully, or `false` if the queue is empty.
 */
bool workq_pop(workq_t *NONNULL queue, work_item *NONNULL item);

[[gnu::nonnull(1), gnu::leaf, gnu::nothrow]]
/**
 * Block current thread until there is an item to be taken in the work queue.
 *
 * Returns `true` if the wait was successfull and the work queue is not empty anymore, or `false` in case of issues
 * with the synchronization variables.
 */
bool workq_wait_not_empty(workq_t *NONNULL queue);

#endif /* SCR_WORKER_QUEUE_H */
