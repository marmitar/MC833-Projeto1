#ifndef SRC_DATABASE_H
/** Handles database operations. */
#define SRC_DATABASE_H

#include "defines.h"
#include <stdint.h>

/** Opaque handle to a database connection. */
typedef struct database_connection db_conn;

[[gnu::regcall, gnu::nonnull(1), gnu::cold, gnu::leaf, gnu::nothrow]]
/**
 * Create or migrate database at `filepath`.
 *
 * Return `true` on success. On failure, the function returns `false` and, if `errmsg` is provided, stores an error
 * message there.
 *
 * @note The caller is responsible for eventually calling `db_close` to release resources and `db_free_errmsg` to free
 *      any error message.
 */
bool db_setup(const char filepath[restrict NONNULL], const char *NONNULL errmsg[NULLABLE 1]);

[[gnu::regcall, gnu::nonnull(1), gnu::cold, gnu::leaf, gnu::nothrow]]
/**
 * Frees a dynamically allocated error message string.
 *
 * This function releases the memory allocated for the error message returned by any database function. If `errmsg` is
 * NULL, no action is taken. After calling this function, `errmsg` becomes invalid.
 */
void db_free_errmsg(const char *NONNULL errmsg);

[[gnu::regcall, gnu::malloc, gnu::nonnull(1), nodiscard, gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Connects to the existing database at `filepath`.
 *
 * On success, returns a newly allocated pointer to a `db_conn` structure. On failure, the function returns `NULL` and,
 * if `errmsg` is provided, stores an error message there.
 *
 * @note The caller is responsible for eventually calling `db_close` to release resources and `db_free_errmsg` to free
 *      any error message.
 */
db_conn *NULLABLE db_connect(const char filepath[restrict NONNULL], const char *NONNULL errmsg[NULLABLE 1]);

[[gnu::regcall, gnu::nonnull(1), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Closes an open database connection.
 *
 * Terminates the connection represented by `conn`. On error, the function logs a message into `errmsg` (if non-null).
 * Once closed, the `conn` pointer is invalid for further use.
 *
 * @note The caller must also call `db_free_errmsg` if an error message is set.
 */
bool db_disconnect(db_conn *NONNULL conn, const char *NONNULL errmsg[NULLABLE 1]);

/** Possible results for database operations. */
typedef enum db_result {
    /** Operation completed without errors. */
    DB_SUCCESS,
    /** Operation was incomplete, but could be retried later. */
    DB_RUNTIME_ERROR,
    /** Invalid input. Don't retry. */
    DB_USER_ERROR,
    /** Unreacoverable error. Stop the thread. */
    DB_HARD_ERROR
} db_result;

/**
 * Represents a single movie record, including an embedded list of genres.
 */
struct movie {
    /** Unique identifier for the movie entry in the database. */
    int64_t id;  // zero for adding
    /** Movie title. */
    const char *NONNULL title;
    /** Director name. */
    const char *NONNULL director;
    /** Year the movie was released. */
    int release_year;
    /** `NULL` terminated list of genres for the movie. */
    const char *NULLABLE genres[];
};

/**
 * Short representation of a movie. Title content is embeded into a single allocation.
 */
struct movie_summary {
    /** Unique identifier for the movie entry in the database. */
    int64_t id;
    /** Embedded movie title. */
    char title[];
};

[[gnu::regcall, gnu::nonnull(1, 2), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Registers a new movie in the database. Updates the `id` field of `movie` if successful.
 *
 * If the `id` field of `movie` is 0, a new record is created, and an ID is generated.
 * The `movie->genres` array (if provided) will be inserted into the `genres` table.
 *
 * Return `DB_SUCCESS` on success; otherwise, returns one of the `db_result` error codes and,
 * if `errmsg` is provided, stores an error message there.
 */
db_result db_register_movie(db_conn *NONNULL conn, struct movie *NONNULL movie, const char *NONNULL errmsg[NULLABLE 1]);

#endif  // SRC_DATABASE_H
