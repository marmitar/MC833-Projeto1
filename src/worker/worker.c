#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bits/pthreadtypes.h>
#include <immintrin.h>
#include <pthread.h>

#include "../alloc.h"
#include "../database/database.h"
#include "../defines.h"
#include "./queue.h"
#include "./request.h"
#include "./worker.h"

[[gnu::nonnull(2)]]
/**
 * Simple pop then wait loop, until a value is taken.
 */
static int workq_pop_or_wait(const pthread_t id, workq_t *NONNULL queue) {
    while (true) {
        int sock_fd;
        bool ok = workq_pop(queue, &sock_fd);
        if likely (ok) {
            assume(sock_fd > 0);
            return sock_fd;
        }

        ok = workq_wait_not_empty(queue);
        if unlikely (!ok) {
            (void) fprintf(stderr, "thread[%lu]: workq_wait_not_empty failed: %s\n", id, strerrordesc_np(errno));
            return -1;
        }
    }
}

[[gnu::nonnull(1)]]
/**
 * Thread function that processes completions from the work queue.
 *
 * @param arg pointer to the queue
 * @returns a pointer with the exit code.
 */
static void *NULLABLE worker_thread(void *NONNULL arg) {
    const pthread_t id = pthread_self();
    workq_t *NONNULL queue = (workq_t *) arg;

    const char *errmsg = NULL;
    db_conn_t *db = db_connect(DATABASE, &errmsg);
    if unlikely (db == NULL) {
        (void) fprintf(stderr, "thread[%lu]: db_connect error: %s\n", id, errmsg);
        db_free_errmsg(errmsg);
        return PTR_FROM_INT(3);
    }

    while (true) {
        const int sock_fd = workq_pop_or_wait(id, queue);
        if unlikely (sock_fd < 0) {
            break;
        }

        // This blocks the worker while we parse & respond, which might not be truly async.
        // For a fully async approach, you'd queue further read/write requests.
        bool ok = handle_request(sock_fd, db);
        if unlikely (!ok) {
            break;
        }
    }

    (void) fprintf(stderr, "thread[%lu]: full stop required\n", id);
    bool ok = db_disconnect(db, &errmsg);
    if unlikely (!ok) {
        (void) fprintf(stderr, "thread[%lu]: db_disconnect error: %s\n", id, errmsg);
        return PTR_FROM_INT(2);
    }

    return PTR_FROM_INT(0);
}

[[gnu::cold, gnu::nonnull(1, 2)]]
/**
 * Start `worker_thread` in a new thread, and save its ID to `id`.
 *
 * Returns `true` on success, or `false` if the thread could not be started.
 */
static bool start_worker(pthread_t *NONNULL id, workq_t *NONNULL queue) {
    pthread_t output_id;
    int rv = pthread_create(&output_id, NULL, worker_thread, queue);
    if unlikely (rv != 0) {
        perror("start_worker");
        return false;
    }

    *id = output_id;
    return true;
}

[[gnu::cold, gnu::nonnull(1, 2)]]
/**
 * Stop the previous worker with `id`, start a new one and save the new ID.
 *
 * Returns `true` on success, or `false` if the thread could not be started.
 */
static bool restart_worker(pthread_t *NONNULL id, workq_t *NONNULL queue) {
    pthread_kill(*id, SIGINT);
    pthread_join(*id, NULL);
    return start_worker(id, queue);
}

/**
 * The list of worker threads and their shared work queue.
 */
struct [[gnu::aligned(WORKERS_CAPACITY)]] worker_list {
    /** IDs of each worker thread. */
    pthread_t id[WORKERS_CAPACITY];
    /** Shared work queue. */
    workq_t *NONNULL queue;
};

[[gnu::cold, gnu::nonnull(1)]]
/**
 * Stops running threads, up to `initialized`. Also deallocates memory used for the list.
 */
static void workers_stop_partial(workers_t *NONNULL workers, size_t initialized) {
    for (size_t i = 0; i < initialized; i++) {
        int rv = pthread_kill(workers->id[i], SIGINT);
        if unlikely (rv != 0 && rv != ESRCH) {
            const char *err = strerrordesc_np(errno);
            (void) fprintf(stderr, "workers_stop: thread[%lu] could not be stopped: %s\n", workers->id[i], err);
        }
    }
    for (size_t i = 0; i < initialized; i++) {
        void *retval;
        int rv = pthread_join(workers->id[i], &retval);
        if unlikely (rv != 0) {
            const char *err = strerrordesc_np(errno);
            (void) fprintf(stderr, "workers_stop: thread[%lu] could not be joined: %s\n", workers->id[i], err);
        } else if unlikely (INT_FROM_PTR(retval) != 0) {
            int err = INT_FROM_PTR(retval);
            (void) fprintf(stderr, "workers_stop: thread[%lu] finished withe error: %d\n", workers->id[i], err);
        }
    }

    workq_destroy(workers->queue);
    free(workers);
}

/** Starts threads for handling TCP requests. */
workers_t *NULLABLE workers_start(void) {
    workq_t *queue = workq_create();
    if unlikely (queue == NULL) {
        return NULL;
    }

    workers_t *workers = alloc_like(struct worker_list);
    if unlikely (workers == NULL) {
        workq_destroy(queue);
        return NULL;
    }
    workers->queue = queue;

    for (size_t i = 0; i < WORKERS_CAPACITY; i++) {
        bool ok = start_worker(&(workers->id[i]), queue);
        if unlikely (!ok) {
            workers_stop_partial(workers, i);
            return NULL;
        }
    }
    return workers;
}

/** Stop all currently running worker threads and deallocate memory. */
void workers_stop(workers_t *NONNULL workers) {
    workers_stop_partial(workers, WORKERS_CAPACITY);
}

[[gnu::hot, gnu::nonnull(1)]]
/**
 * Check if any thread is dead, and start a new one in its place.
 */
static bool restart_dead_workers(workers_t *NONNULL workers) {
    size_t dead_threads = 0;

    for (size_t i = 0; i < WORKERS_CAPACITY; i++) {
        int rv = pthread_kill(workers->id[i], 0);
        // thread died (does it work without join?)
        if unlikely (rv == ESRCH) {
            bool ok = restart_worker(&(workers->id[i]), workers->queue);
            if unlikely (!ok) {
                dead_threads += 1;
            }
        }
    }

    return likely(dead_threads < WORKERS_CAPACITY);
}

/** Adds `socket_fd` to the worker queue and signal worker threads that a new connection is open. */
bool workers_add_work(workers_t *NONNULL workers, int socket_fd) {
    while (true) {
        bool has_workers = restart_dead_workers(workers);
        if unlikely (!has_workers) {
            return false;
        }

        bool not_full = workq_push(workers->queue, socket_fd);
        if likely (not_full) {
            return true;
        }

        _mm_pause();
    }
}
