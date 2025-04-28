#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <immintrin.h>
#include <pthread.h>

#include "../alloc.h"
#include "../database/database.h"
#include "../defines.h"
#include "./queue.h"
#include "./request.h"
#include "./worker.h"

/** Optimal alignment for `struct worker`. */
#define WORKER_ALIGNMENT 32

/**
 * Status of a single worker thread.
 */
struct [[gnu::aligned(WORKER_ALIGNMENT)]] worker {
    /** The thread id. */
    pthread_t id;
    /** ID for naming the thread. */
    size_t worker_id;
    /** If it should be stopped. */
    atomic_bool finished;
};

/** Optimal alignment for `workers`. */
#define WORKERS_ALIGNMENT 128

/**
 * The list of worker threads and their shared work queue.
 */
static struct [[gnu::aligned(WORKERS_ALIGNMENT)]] worker_list {
    /** IDs of each worker thread. */
    struct worker list[WORKERS_CAPACITY];
    /** ID of the worker thread. */
    size_t next_worker_id;
    /** Shared work queue. */
    workq_t *NONNULL queue;
} workers;

[[gnu::cold, gnu::nonnull(2)]]
/**
 * Set up a handler for the current
 */
static bool set_signal_handler(int signo, sighandler_t handler) {
    struct sigaction action = {
        .sa_handler = handler,
        .sa_flags = 0,  // do NOT use SA_RESTART
    };
    int rv = sigemptyset(&(action.sa_mask));
    if unlikely (rv != 0) {
        return false;
    }

    rv = sigaction(signo, &action, NULL);
    return likely(rv == 0);
}

[[gnu::cold]]
/**
 * Set sigmask to `signo` and block all other signals.
 */
static bool set_single_sigmask(int signo) {
    sigset_t thread_mask;

    int rv = sigfillset(&thread_mask);
    if unlikely (rv != 0) {
        return false;
    }

    rv = sigdelset(&thread_mask, signo);
    if unlikely (rv != 0) {
        return false;
    }

    rv = pthread_sigmask(SIG_BLOCK, &thread_mask, NULL);
    return likely(rv == 0);
}

/** Set to 1 when a termination signal is received. */
static volatile sig_atomic_t shutdown_requested = 0;

/** Handles SIGINT and SIGTERM in main thread. */
static void handle_termination(int signo) {
    assume(signo == SIGINT || signo == SIGTERM);
    (void) signo;

    shutdown_requested = true;
    for (size_t i = 0; i < WORKERS_CAPACITY; i++) {
        atomic_store(&(workers.list[i].finished), true);
    }
}

/** Handles SIGUSR1 in worker thread. */
static void handle_sigusr1(int signo) {
    assume(signo == SIGUSR1);
    (void) signo;
    // main thread should have already set `finished` flag at this point
}

[[gnu::nonnull(2, 3)]]
/**
 * Simple pop then wait loop, until a value is taken.
 */
static int workq_pop_or_wait(const pthread_t id, workq_t *NONNULL queue, atomic_bool *NONNULL finished) {
    while (!unlikely(atomic_load(finished))) {
        int sock_fd;
        bool ok = workq_pop(queue, &sock_fd);
        if likely (ok) {
            assume(sock_fd > 0);
            return sock_fd;
        }

        ok = workq_wait_not_empty(queue, finished);
        if unlikely (!ok) {
            (void) fprintf(stderr, "worker[%zu]: workq_wait_not_empty failed: %s\n", id, strerrordesc_np(errno));
            return -1;
        }
    }
    return -1;
}

/** Data for starting the thread. */
struct [[gnu::aligned(WORKER_ALIGNMENT)]] worker_input {
    /** The shared work queue. */
    workq_t *NONNULL queue;
    /** ID for naming the thread. */
    size_t worker_id;
    /** If the thread should be stopped. */
    atomic_bool *NONNULL finished;
};

[[gnu::nonnull(1)]]
/**
 * Thread function that processes completions from the work queue.
 *
 * @param arg pointer to the queue
 * @returns a pointer with the exit code.
 */
static void *NULLABLE worker_thread(void *NONNULL arg) {
    struct worker_input *NONNULL input = aligned_like(struct worker_input, arg);
    const size_t id = input->worker_id;
    workq_t *NONNULL const queue = aligned_as(2 * CACHE_LINE_SIZE, input->queue);
    atomic_bool *NONNULL const finished = input->finished;
    free(input);

    bool sig_ok = set_single_sigmask(SIGUSR1) && set_signal_handler(SIGUSR1, handle_sigusr1);
    if unlikely (!sig_ok) {
        (void) fprintf(stderr, "worker[%zu]: failed to set signal handler for SIGUSR1\n", id);
        return PTR_FROM_INT(1);
    }

    const char *errmsg = NULL;
    db_conn_t *db = db_connect(DATABASE, &errmsg);
    if unlikely (db == NULL) {
        (void) fprintf(stderr, "worker[%zu]: db_connect error: %s\n", id, errmsg);
        db_free_errmsg(errmsg);
        return PTR_FROM_INT(2);
    }

    while (!unlikely(atomic_load(finished))) {
        const int sock_fd = workq_pop_or_wait(id, queue, finished);
        if unlikely (sock_fd < 0) {
            break;
        }

        // This blocks the worker while we parse & respond, which might not be truly async.
        // For a fully async approach, you'd queue further read/write requests.
        bool ok = handle_request(id, sock_fd, db, finished);
        if unlikely (!ok) {
            break;
        }
    }

    (void) fprintf(stderr, "worker[%zu]: full stop requested\n", id);
    bool ok = db_disconnect(db, &errmsg);
    if unlikely (!ok) {
        (void) fprintf(stderr, "worker[%zu]: db_disconnect error: %s\n", id, errmsg);
        return PTR_FROM_INT(3);
    }

    return PTR_FROM_INT(0);
}

[[gnu::cold, gnu::nonnull(1, 2)]]
/**
 * Start `worker_thread` in a new thread, and save its ID to `id`.
 *
 * Returns `true` on success, or `false` if the thread could not be started.
 */
static bool start_worker(struct worker *NONNULL worker, workq_t *NONNULL queue) {
    struct worker_input *input = alloc_like(struct worker_input);
    if unlikely (input == NULL) {
        return false;
    }

    const size_t worker_id = worker->worker_id;
    atomic_store(&(worker->finished), false);
    *input = (struct worker_input) {.queue = queue, .worker_id = worker_id, .finished = &(worker->finished)};

    pthread_t output_id;
    int rv = pthread_create(&output_id, NULL, worker_thread, input);
    if unlikely (rv != 0) {
        perror("start_worker");
        free(input);
        return false;
    }

    static constexpr const size_t NAME_LEN = 16;
    char name[NAME_LEN] = "";
    (void) snprintf(name, NAME_LEN, "worker[%zu]", worker_id);
    (void) pthread_setname_np(output_id, name);
    worker->id = output_id;
    return true;
}

[[gnu::cold, gnu::nonnull(1, 2)]]
/**
 * Stop the previous worker with `id`, start a new one and save the new ID.
 *
 * Returns `true` on success, or `false` if the thread could not be started.
 */
static bool restart_worker(struct worker *NONNULL worker, workq_t *NONNULL queue) {
    // worker should be dead already, but let's ensure
    atomic_store(&(worker->finished), true);
    pthread_kill(worker->id, SIGKILL);
    pthread_join(worker->id, NULL);

    return start_worker(worker, queue);
}

[[gnu::cold]]
/**
 * Stops running threads, up to `initialized`. Also deallocates memory used for the list.
 */
static void workers_stop_partial(size_t initialized) {
    for (size_t i = 0; i < initialized; i++) {
        int rv = pthread_kill(workers.list[i].id, SIGUSR1);
        if unlikely (rv != 0 && rv != ESRCH) {
            const char *err = strerrordesc_np(errno);
            (void) fprintf(stderr, "workers_stop: worker[%zu] could not be stopped: %s\n", workers.list[i].id, err);
        }
    }
    for (size_t i = 0; i < initialized; i++) {
        void *retval;
        int rv = pthread_join(workers.list[i].id, &retval);
        if unlikely (rv != 0) {
            const char *err = strerrordesc_np(errno);
            (void) fprintf(stderr, "workers_stop: worker[%zu] could not be joined: %s\n", workers.list[i].id, err);
        } else if unlikely (INT_FROM_PTR(retval) != 0) {
            int err = INT_FROM_PTR(retval);
            (void) fprintf(stderr, "workers_stop: worker[%zu] finished withe error: %d\n", workers.list[i].id, err);
        }
    }

    workq_destroy(workers.queue);
    memset(&workers, 0, sizeof(workers));
}

/** Starts threads for handling TCP requests. */
bool workers_start(void) {
    bool sig_ok = set_signal_handler(SIGINT, handle_termination) && set_signal_handler(SIGTERM, handle_termination)
        && set_signal_handler(SIGPIPE, SIG_IGN);
    if unlikely (!sig_ok) {
        return false;
    }

    memset(&workers, 0, sizeof(workers));

    workq_t *queue = workq_create();
    if unlikely (queue == NULL) {
        return false;
    }
    workers.queue = queue;
    workers.next_worker_id = 0;

    for (size_t i = 0; i < WORKERS_CAPACITY; i++) {
        workers.list[i].worker_id = workers.next_worker_id++;

        bool ok = start_worker(&(workers.list[i]), queue);
        if unlikely (!ok) {
            workers_stop_partial(i);
            return false;
        }
    }
    return true;
}

/** Stop all currently running worker threads and deallocate memory. */
void workers_stop(void) {
    workq_clear(workers.queue);
    workers_stop_partial(WORKERS_CAPACITY);
}

[[gnu::hot]]
/**
 * Check if any thread is dead, and start a new one in its place.
 */
static bool restart_dead_workers(void) {
    size_t dead_threads = 0;

    for (size_t i = 0; i < WORKERS_CAPACITY; i++) {
        int rv = pthread_kill(workers.list[i].id, 0);
        // thread died (does it work without join?)
        if unlikely (rv == ESRCH) {
            workers.list[i].worker_id = workers.next_worker_id++;

            bool ok = restart_worker(&(workers.list[i]), workers.queue);
            if unlikely (!ok) {
                dead_threads += 1;
            }
        }
    }

    return likely(dead_threads < WORKERS_CAPACITY);
}

/** Adds `socket_fd` to the worker queue and signal worker threads that a new connection is open. */
bool workers_add_work(int socket_fd, unsigned retries) {
    workq_t *NONNULL const queue = aligned_as(2 * CACHE_LINE_SIZE, workers.queue);

    while (likely(!was_shutdown_requested()) && likely(retries > 0)) {
        bool has_workers = restart_dead_workers();
        if unlikely (!has_workers) {
            return false;
        }

        bool not_full = workq_push(queue, socket_fd);
        if likely (not_full) {
            return true;
        }

        retries -= 1;
        _mm_pause();
    }

    // stopped for shutdown request, so not an issue
    return true;
}

/** Returns true if main thread received a signal for shutdown. */
bool was_shutdown_requested(void) {
    return unlikely(shutdown_requested != 0);
}
