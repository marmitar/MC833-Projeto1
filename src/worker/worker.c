#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bits/pthreadtypes.h>
#include <pthread.h>
#include <unistd.h>

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
        int sock_fd = 0;
        bool ok = workq_pop(queue, &sock_fd);
        if likely (ok) {
            assume(sock_fd > 0);
            return sock_fd;
        }

        (void) fprintf(stderr, "thread[%lu]: workq_pop: empty\n", id);
        ok = workq_wait_not_empty(queue);
        if unlikely (!ok) {
            (void) fprintf(stderr, "thread[%lu]: workq_wait_not_empty: failed\n", id);
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

    return PTR_FROM_INT(1);
}

/**
 * Get the number of available CPUs.
 */
unsigned cpu_count(void) {
    long long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if unlikely (cpus < 0) {
        return 0;
    } else if unlikely (cpus > UINT_MAX) {
        return UINT_MAX;
    } else {
        return (unsigned) cpus;
    }
}

/**
 * Start a worker thread.
 */
bool start_worker(pthread_t *NONNULL id, workq_t *NONNULL queue) {
    return pthread_create(id, NULL, worker_thread, queue) == 0;
}
