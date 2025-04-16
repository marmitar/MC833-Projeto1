#include <liburing.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../database/database.h"
#include "../defines.h"
#include "./request.h"
#include "./worker.h"

/** Maximum number of queued events for io_uring. */
static constexpr const size_t IORING_QUEUE_DEPTH = 64;

/**
 * @brief Thread function that processes completions (and possibly enqueues new requests) from the io_uring.
 *
 * Each worker thread calls io_uring_wait_cqe() to block for completions. Once a CQE is received, the thread
 * dispatches the result (e.g. handle_request for a new accept, or read).
 *
 * @param arg pointer to the io_uring
 * @return NULL always
 */
static void *NULLABLE worker_thread(void *NONNULL arg) {
    const pthread_t id = pthread_self();
    struct io_uring *ring = (struct io_uring *) arg;

    const char *errmsg = NULL;
    db_conn *db = db_connect(DATABASE, &errmsg);
    if unlikely (db == NULL) {
        fprintf(stderr, "thread[%lu]: db_connect error: %s\n", id, errmsg);
        db_free_errmsg(errmsg);
        return PTR_FROM_INT(3);
    }

    bool stop = false;
    while (!stop) {
        struct io_uring_cqe *cqe = NULL;
        int rv = io_uring_wait_cqe(ring, &cqe);
        if unlikely (rv < 0) {
            fprintf(stderr, "thread[%lu]: io_uring_wait_cqe: %s\n", id, strerror(-rv));
            continue;
        }

        int res = cqe->res;
        void *udata = io_uring_cqe_get_data(cqe);
        if (res < 0) {
            fprintf(stderr, "thread[%lu]: io_uring op failed: %s\n", id, strerror(-res));
            io_uring_cqe_seen(ring, cqe);
            continue;
        }

        const int client_fd = INT_FROM_PTR(udata);
        io_uring_cqe_seen(ring, cqe);

        // This blocks the worker while we parse & respond, which might not be truly async.
        // For a fully async approach, you'd queue further read/write requests.
        stop = handle_request(client_fd, db);
    }

    fprintf(stderr, "thread[%lu]: full stop required\n", id);
    bool ok = db_disconnect(db, &errmsg);
    if unlikely (!ok) {
        fprintf(stderr, "thread[%lu]: db_disconnect error: %s\n", id, errmsg);
        return PTR_FROM_INT(2);
    }

    return PTR_FROM_INT(1);
}

/**
 * Get the number of available CPUs.
 */
unsigned cpu_count(void) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
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
bool start_worker(pthread_t *NONNULL id, struct io_uring *NONNULL uring) {
    return pthread_create(id, NULL, worker_thread, uring) == 0;
}

/**
 * Set up the IOUring.
 */
bool uring_init(struct io_uring *NONNULL ring) {
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    int rv = io_uring_queue_init_params(IORING_QUEUE_DEPTH, ring, &params);
    if unlikely (rv < 0) {
        fprintf(stderr, "thread[%lu] io_uring_queue_init: %s\n", pthread_self(), strerror(-rv));
        return false;
    }
    return true;
}

/**
 * Posts an async accept request into the ring.
 */
bool post_accept(struct io_uring *NONNULL ring, int server_fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        fprintf(stderr, "thread[%lu]: post_accept: no sqe available\n", pthread_self());
        return false;
    }

    io_uring_prep_accept(sqe, server_fd, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, PTR_FROM_INT(server_fd));
    return true;
}
