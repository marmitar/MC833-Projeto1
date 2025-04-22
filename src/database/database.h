#ifndef SRC_DATABASE_H
/** Handles database operations. */
#define SRC_DATABASE_H

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../defines.h"
#include "../movie/movie.h"

/** The default database name. */
static constexpr const char DATABASE[] = "movies.db";

/** Enforced alignment for `db_conn_t`. */
#define ALIGNMENT_DB_CONN 128

/** Opaque handle to a database connection. */
typedef struct database_connection db_conn_t [[gnu::aligned(ALIGNMENT_DB_CONN)]];

/** Output error messages, never nullable. */
typedef const char *NONNULL message_t;

[[nodiscard("cannot use database on false"), gnu::nonnull(1), gnu::cold, gnu::leaf, gnu::nothrow]]
/**
 * Create or migrate database at `filepath`.
 *
 * Return `true` on success. On failure, the function returns `false` and, if `errmsg` is provided, stores an error
 * message there.
 *
 * @note The caller is responsible for eventually calling `db_close` to release resources and `db_free_errmsg` to free
 *      any error message.
 */
bool db_setup(const char filepath[NONNULL restrict], message_t *NULLABLE restrict errmsg);

[[gnu::nonnull(1), gnu::cold, gnu::leaf, gnu::nothrow]]
/**
 * Frees a dynamically allocated error message string.
 *
 * This function releases the memory allocated for the error message returned by any database function. If `errmsg` is
 * NULL, no action is taken. After calling this function, `errmsg` becomes invalid.
 */
void db_free_errmsg(message_t errmsg);

[[nodiscard("allocated memory must be freed"),
  gnu::malloc,
  gnu::assume_aligned(ALIGNMENT_DB_CONN),
  gnu::nonnull(1),
  gnu::cold,
  gnu::leaf,
  gnu::nothrow]]
/**
 * Connects to the existing database at `filepath`.
 *
 * On success, returns a newly allocated pointer to a `db_conn` structure. On failure, the function returns `NULL` and,
 * if `errmsg` is provided, stores an error message there.
 *
 * @note The caller is responsible for eventually calling `db_close` to release resources and `db_free_errmsg` to free
 *      any error message.
 */
db_conn_t *NULLABLE db_connect(const char filepath[NONNULL restrict], message_t *NULLABLE restrict errmsg);

[[gnu::nonnull(1), gnu::cold, gnu::leaf, gnu::nothrow]]
/**
 * Closes an open database connection.
 *
 * Terminates the connection represented by `conn`. On error, the function logs a message into `errmsg` (if non-null).
 * Once closed, the `conn` pointer is invalid for further use.
 *
 * @note The caller must also call `db_free_errmsg` if an error message is set.
 */
bool db_disconnect(db_conn_t *NONNULL conn, message_t *NULLABLE errmsg);

/** Possible results for database operations. */
typedef enum [[gnu::packed]] db_result {
    /** Operation completed without errors. */
    DB_SUCCESS,
    /** Operation was incomplete, but could be retried later. */
    DB_RUNTIME_ERROR,
    /** Invalid input. Don't retry. */
    DB_USER_ERROR,
    /** Unreacoverable error. Stop the thread. */
    DB_HARD_ERROR
} db_result_t;

static_assert(sizeof(db_result_t) == 1);

[[nodiscard("hard errors cannot be ignored"), gnu::nonnull(1, 2), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Registers a new movie in the database. Updates the `id` field of `movie` if successful.
 *
 * If the `id` field of `movie` is 0, a new record is created, and an ID is generated. The `movie->genres` array
 (if provided) will be inserted into the `genres` table.
 *
 * Return `DB_SUCCESS` on success; otherwise, returns one of the `db_result` error codes and, if `errmsg` is provided,
 stores an error message there.
 */
db_result_t db_register_movie(
    db_conn_t *NONNULL conn,
    struct movie *NONNULL movie,
    message_t *NULLABLE restrict errmsg
);

[[nodiscard("hard errors cannot be ignored"), gnu::nonnull(1, 3), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Adds a new genre to an existing movie.
 *
 * Ensures the movie exists and the genre is new to that movie. If required, also creates a entry for the genre itself.
 *
 * Return `DB_SUCCESS` on success; otherwise, returns one of the `db_result` error codes and, if `errmsg` is provided,
 stores an error message there.
 */
db_result_t db_add_genre(
    db_conn_t *NONNULL conn,
    int64_t movie_id,
    const char genre[NONNULL restrict const],
    message_t *NULLABLE restrict errmsg
);

[[nodiscard("hard errors cannot be ignored"), gnu::nonnull(1), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Removes a movie from the database.
 *
 * Return `DB_SUCCESS` on success; otherwise, returns one of the `db_result` error codes and, if `errmsg` is provided,
 stores an error message there.
 */
db_result_t db_delete_movie(db_conn_t *NONNULL conn, int64_t movie_id, message_t *NULLABLE restrict errmsg);

[[nodiscard("hard errors cannot be ignored"), gnu::nonnull(1, 3), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Get a movie from the database and write it to `movie`. The caller is reponsible for calling `free` on each.
 *
 * Return `DB_SUCCESS` on success; otherwise, returns one of the `db_result` error codes and, if `errmsg` is provided,
 stores an error message there.
 */
db_result_t db_get_movie(
    db_conn_t *NONNULL conn,
    int64_t movie_id,
    struct movie *NONNULL output,
    message_t *NULLABLE restrict errmsg
);

[[nodiscard("hard errors cannot be ignored"), gnu::nonnull(1, 2, 3), gnu::hot]]
/**
 * List all movies from the database and run `callback` on each one. The caller is reponsible for calling `free` on it.
 *
 * Return `DB_SUCCESS` on success; otherwise, returns one of the `db_result` error codes and, if `errmsg` is provided,
 stores an error message there.
 */
db_result_t db_list_movies(
    db_conn_t *NONNULL conn,
    struct movie *NONNULL *NONNULL output,
    size_t *NONNULL output_length,
    message_t *NULLABLE restrict errmsg
);

[[nodiscard("hard errors cannot be ignored"), gnu::nonnull(1, 2, 3, 4), gnu::hot]]
/**
 * List all movies with a given genre and run `callback` on each one. The caller is reponsible for calling `free` on
 * each.
 *
 * Return `DB_SUCCESS` on success; otherwise, returns one of the `db_result` error codes and, if `errmsg` is provided,
 * stores an error message there.
 */
db_result_t db_search_movies_by_genre(
    db_conn_t *NONNULL conn,
    const char genre[NONNULL restrict const],
    struct movie *NONNULL *NONNULL output,
    size_t *NONNULL output_length,
    message_t *NULLABLE restrict errmsg
);

[[nodiscard("hard errors cannot be ignored"), gnu::nonnull(1, 2, 3), gnu::hot]]
/**
 * List summaries of all movies in the database and run `callback` on each summary. The caller is reponsible for calling
 * `free` on it.
 *
 * Return `DB_SUCCESS` on success; otherwise, returns one of the `db_result` error codes and, if `errmsg` is provided,
 * stores an error message there.
 */
db_result_t db_list_summaries(
    db_conn_t *NONNULL conn,
    struct movie_summary *NONNULL *NONNULL output,
    size_t *NONNULL output_length,
    message_t *NULLABLE restrict errmsg
);

#endif  // SRC_DATABASE_H
