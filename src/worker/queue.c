#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bits/pthreadtypes.h>
#include <bits/time.h>
#include <immintrin.h>
#include <pthread.h>

#include "../alloc.h"
#include "../defines.h"
#include "./queue.h"

// For safety, simplicity and performance: WORK_QUEUE_CAPACITY must be a divisor of `UINT_MAX + 1`, so it wraps
// correctly on the UINT_MAX to 0 boundary.
static_assert(0 < WORK_QUEUE_CAPACITY && WORK_QUEUE_CAPACITY < UINT_MAX);
static_assert(UINT_MAX % WORK_QUEUE_CAPACITY == WORK_QUEUE_CAPACITY - 1);

/**
 * The actual work queue.
 *
 * Implemented as a ring buffer, using atomics where possible and synchronization locks otherwise.
 */
struct [[gnu::aligned(2 * CACHE_LINE_SIZE)]] work_queue {
    // Slow-path synchronisation, mostly isolated on its own cache line. ---------------------------------------------
    /**
     * Signalled on each push, to wake worker threads.
     */
    [[gnu::aligned(CACHE_LINE_SIZE)]] pthread_cond_t item_added_cond;
    /**
     * Guards the `item_added_cond`.
     */
    [[gnu::aligned(CACHE_LINE_SIZE)]] pthread_mutex_t item_added_mtx;
    // Ring storage --------------------------------------------------------------------------------------------------
    /**
     * The ring buffer, limited to `WORK_QUEUE_CAPACITY` items of `work_item`.
     *
     * Might share a bit of the cache with other variables, but most of it is on its own cache. This also serves as a
     * separation between the cache lines for synchronization variables above and the atomics below.
     */
    work_item buf[WORK_QUEUE_CAPACITY];
    /**
     * Next ticket for consumers to pop. Not capped to `WORK_QUEUE_CAPACITY`.
     */
    atomic_uint_fast64_t head;
    /**
     * Next ticket for producers to push. Not capped to `WORK_QUEUE_CAPACITY`.
     */
    atomic_uint_fast64_t tail;
};

// We need to ensure the atomic variables are on the same cache line.
#define cache_line_of(offset)            ((offset) / CACHE_LINE_SIZE)
#define first_cache_line_of(type, field) cache_line_of(offsetof(type, field))
#define last_cache_line_of(type, field)  cache_line_of(offsetof(type, field) + sizeof(((type) {}).field))
static_assert(
    first_cache_line_of(workq_t, head) == last_cache_line_of(workq_t, tail),
    "Both atomics should be in the same cache line, because they have true sharing."
);
// And ensure they don't interfere with outside cache lines.
static_assert(sizeof(workq_t) % CACHE_LINE_SIZE == 0, "The shared variables should not interfere with outside cache.");

[[nodiscard("mutex unitialized on false"), gnu::nonnull(1)]]
/** Initialize the mutex with custom attributes. */
static bool workq_mutex_init(pthread_mutex_t *NONNULL mutex) {
    pthread_mutexattr_t attr;
    int rv = pthread_mutexattr_init(&attr);
    if unlikely (rv != 0) {
        return false;
    }

    const int rvs[] = {
        // Single process only.
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_PRIVATE),
        // We don't use priorities.
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_NONE),
        // Assume threads can die at any time.
        pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST),
#ifdef DEBUG  // On debug, enable error checking.
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK),
#else  // On release, use bare threads.
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL),
#endif
    };
    for (size_t i = 0; i < sizeof(rvs) / sizeof(int); i++) {
        if unlikely (rvs[i] != 0) {
            pthread_mutexattr_destroy(&attr);
            return false;
        }
    }

    rv = pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    return likely(rv == 0);
}

[[nodiscard("mutex unitialized on false"), gnu::nonnull(1)]]
/** Initialize the condition variables with custom attributes. */
static bool workq_cond_init(pthread_cond_t *NONNULL cond) {
    pthread_condattr_t attr;
    int rv = pthread_condattr_init(&attr);
    if unlikely (rv != 0) {
        return false;
    }

    const int rvs[] = {
        // Single process only.
        pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_PRIVATE),
        // Use low resolution (1 ms to 10 ms) clock.
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC),
    };
    for (size_t i = 0; i < sizeof(rvs) / sizeof(int); i++) {
        if unlikely (rvs[i] != 0) {
            pthread_condattr_destroy(&attr);
            return false;
        }
    }

    rv = pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
    return likely(rv == 0);
}

/** Allocate memory for the work queue and initialize its synchronization variables. */
workq_t *NULLABLE workq_create(void) {
    workq_t *queue = alloc_like(struct work_queue);
    if unlikely (queue == NULL) {
        return NULL;
    }

    bool ok = workq_mutex_init(&(queue->item_added_mtx));
    if unlikely (!ok) {
        free(queue);
        return NULL;
    }

    ok = workq_cond_init(&(queue->item_added_cond));
    if unlikely (!ok) {
        pthread_mutex_destroy(&(queue->item_added_mtx));
        free(queue);
        return NULL;
    }

    atomic_init(&(queue->head), 0);
    atomic_init(&(queue->tail), 0);
    return queue;
}

/** Deallocates memory for the work queue and destroy its synchronization variables. */
void workq_destroy(workq_t *NONNULL queue) {
    const char *func[] = {"pthread_cond_destroy", "pthread_mutex_destroy"};
    const int rvs[] = {
        pthread_cond_destroy(&(queue->item_added_cond)),
        pthread_mutex_destroy(&(queue->item_added_mtx)),
    };
    memset(queue, 0, sizeof(struct work_queue));
    free(queue);

    for (size_t i = 0; i < sizeof(rvs) / sizeof(int); i++) {
        if unlikely (rvs[i] != 0) {
            (void) fprintf(
                stderr,
                "workq_destroy: %s failed: %s (%s)\n",
                func[i],
                strerrordesc_np(rvs[i]),
                strerrorname_np(rvs[i])
            );
        }
    }
}

[[nodiscard("lock may fail"), gnu::nonnull(1)]]
/**
 * Lock a mutex and ensure consistency if last owner died.
 *
 * Returns false on errors.
 */
static bool workq_mutex_lock(workq_t *NONNULL queue) {
    int rv = pthread_mutex_lock(&(queue->item_added_mtx));
    if unlikely (rv == EOWNERDEAD) {
        rv = pthread_mutex_consistent(&(queue->item_added_mtx));
    }
    return likely(rv == 0);
}

[[gnu::nonnull(1)]]
/**
 * Signal one worker thread that a new item was added, so it may starting working on that.
 *
 * Returns false on errors.
 */
static bool workq_signal_item_added(workq_t *NONNULL queue) {
    bool ok = workq_mutex_lock(queue);
    if unlikely (!ok) {
        return false;
    }

    int rv0 = pthread_cond_signal(&(queue->item_added_cond));
    int rv1 = pthread_mutex_unlock(&(queue->item_added_mtx));
    return likely(rv0 == 0 && rv1 == 0);
}

[[nodiscard("useless call if discarded"), gnu::const]]
/**
 * Convert `head` and `tail` to valid ring buffer indexes.
 */
static inline size_t idx(uint_fast64_t ticket) {
    return (size_t) (ticket % WORK_QUEUE_CAPACITY);
}

[[nodiscard("useless call if discarded"), gnu::const]]
static inline int_fast64_t workq_size(uint_fast64_t head, uint_fast64_t tail) {
    const uint_fast64_t diff = tail - head;
    if likely (diff <= INT_FAST64_MAX) {
        return (int_fast64_t) diff;
    }

    static_assert(((int_fast64_t) -1) == ~((int_fast64_t) 0), "Two's complement representation required");
    static_assert(sizeof(int_fast64_t) == sizeof(uint_fast64_t), "Input and output types must have the same size");
    return -(int_fast64_t) (~diff + 1);
}

/** Mostly lock-free push. Returns `false` on full. */
bool workq_push(workq_t *NONNULL queue, work_item item) {
    // we just use head to ensure the queue is not full, so we don't need the latest value
    uint_fast64_t head = atomic_load_explicit(&(queue->head), memory_order_relaxed);
    // assuming push is called by only one thread, we already have the latest tail
    uint_fast64_t tail = atomic_load_explicit(&(queue->tail), memory_order_relaxed);

    if unlikely (workq_size(head, tail) >= WORK_QUEUE_CAPACITY) {
        // if the queue seems full, then we load the latest head to ensure that is the case before dropping items
        head = atomic_load_explicit(&(queue->head), memory_order_acquire);
        if unlikely (workq_size(head, tail) >= WORK_QUEUE_CAPACITY) {
            assert(workq_size(head, tail) == WORK_QUEUE_CAPACITY);
            return false;  // actually full
        }
    }

    // WARNING: this is racy, if two threads try to push at the same time, which can't happen in this project.
    queue->buf[idx(tail)] = item;

    bool ok = atomic_compare_exchange_weak_explicit(
        &(queue->tail),
        &tail,
        tail + 1,
        memory_order_release,  // release tail, for other threads to see
        memory_order_relaxed
    );
    // this has to be true, otherwise we just overwrote another work_item
    assert(ok);

    assert(workq_size(head, tail) <= WORK_QUEUE_CAPACITY);
    return likely(ok) && workq_signal_item_added(queue);
}

/** Lock-free pop. Returns `false` on empty. */
bool workq_pop(workq_t *NONNULL queue, work_item *NONNULL item) {
    while (true) {
        // we can have an outdated head here, because it will be checked again later and it's wrong, then we
        // just throw away the wrong value from `buf` and try again
        uint_fast64_t head = atomic_load_explicit(&(queue->head), memory_order_relaxed);
        // tail must be the latest, otherwise we could get an underflow on subtraction
        uint_fast64_t tail = atomic_load_explicit(&(queue->tail), memory_order_relaxed);

        if unlikely (workq_size(head, tail) <= 0) {
            // queue seems empty, we need to get the latest tail and check again
            tail = atomic_load_explicit(&(queue->tail), memory_order_acquire);
            if unlikely (workq_size(head, tail) <= 0) {
                return false;  // actually empty
            }
        }

        // At this point we know `head` was a valid position, but the item may have already been taken,
        // so we can't use it yet.
        // Also, this was not overwritten by a push, unless it had already been taken by another thread,
        work_item target = queue->buf[idx(head)];
        // because a push neve writes to head.
        // Then we try and announce we have taken it.
        bool ok = atomic_compare_exchange_weak_explicit(
            &(queue->head),
            &head,
            head + 1,
            memory_order_acquire,
            memory_order_relaxed
        );

        if likely (ok) {
            assert(workq_size(head, tail) >= 0);
            // If we took the position and the item, then the read was valid and not overwritten at that point.
            // Now, we can use it safely.
            *item = target;
            return true;
        }

        // wait a bit before trying again
        _mm_pause();
    }
}

[[nodiscard("useless call if discarded"), gnu::pure, gnu::nonnull(1)]]
/**
 * Check if the list is empty.
 */
static bool is_empty(const workq_t *NONNULL queue) {
    // memory_order_acquire is not required while holding a mutex in x86, GCC and Clang actually ignores it here,
    // but they are required in same architectures like ARM
    const uint_fast64_t head = atomic_load_explicit(&(queue->head), memory_order_acquire);
    const uint_fast64_t tail = atomic_load_explicit(&(queue->tail), memory_order_acquire);
    assume(workq_size(head, tail) <= WORK_QUEUE_CAPACITY);
    return workq_size(head, tail) <= 0;
}

/**
 * Block current thread until there is an item to be taken in the work queue.
 */
bool workq_wait_not_empty(workq_t *NONNULL queue) {
    bool ok = workq_mutex_lock(queue);
    if unlikely (!ok) {
        return false;
    }

    int rv0 = 0;
    while (unlikely(is_empty(queue))) {
        rv0 = pthread_cond_wait(&(queue->item_added_cond), &(queue->item_added_mtx));
        if unlikely (rv0 != 0) {
            break;
        }
    }

    // always unlock the mutex, even if `pthread_cond_wait` failed
    int rv1 = pthread_mutex_unlock(&(queue->item_added_mtx));
    return likely(rv0 == 0) && likely(rv1 == 0);
}
