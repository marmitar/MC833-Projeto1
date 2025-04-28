/** Database operations. */
#include "./sqlite_source.h"  // must be included first for defines

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdckdint.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../alloc.h"
#include "../defines.h"
#include "../movie/builder.h"
#include "../movie/movie.h"
#include "./database.h"
#include "./schema.h"

static const char UNKNOWN_ERROR[] = "unknown error";
static const char OUT_OF_MEMORY_ERROR[] = "out of memory";
static const char ATEXIT_NOT_REGISTERED_ERROR[] = "could not call at_exit";

[[gnu::cold, gnu::const, nodiscard("useless call when discarded")]]
/** Predefined error messages, which are NOT dynamically allocated and should not be free. */
static inline bool is_predefined_errmsg(const char *NULLABLE const error_message) {
    return error_message == UNKNOWN_ERROR || error_message == OUT_OF_MEMORY_ERROR
        || error_message == ATEXIT_NOT_REGISTERED_ERROR;
}

[[gnu::cold, gnu::returns_nonnull, nodiscard("might allocate memory")]]
/**
 * Duplicates an error message string if not NULL, returning a default message when the original is NULL or
 * allocation fails.
 */
static const char *NONNULL errmsg_dup(const char *NULLABLE error_message) {
    if unlikely (is_predefined_errmsg(error_message)) {
        return error_message;
    } else if unlikely (error_message == NULL) {
        return UNKNOWN_ERROR;
    }

    const size_t size = strlen(error_message) + 1;
    char *copy = malloc(size * sizeof(char));
    if unlikely (copy == NULL) {
        return OUT_OF_MEMORY_ERROR;
    }

    memcpy(copy, error_message, size * sizeof(char));
    return copy;
}

/** Copies the current SQLite error message from `str` into `errmsg`, if non-NULL. */
static inline void errmsg_dup_str(message_t *NULLABLE restrict errmsg, const char str[NULLABLE restrict]) {
    if likely (errmsg != NULL) {
        *errmsg = errmsg_dup(str);
    }
}

/** Copies the current SQLite error message from `db` into `errmsg`, if non-NULL. */
static inline void errmsg_dup_db(message_t *NULLABLE errmsg, sqlite3 *NULLABLE db) {
    if likely (errmsg != NULL) {
        *errmsg = errmsg_dup(sqlite3_errmsg(db));
    }
}

/** Copies the SQLite error string for the error code `rc` into `errmsg`, if non-NULL. */
static inline void errmsg_dup_rc(message_t *NULLABLE errmsg, const int rc) {
    if likely (errmsg != NULL) {
        *errmsg = errmsg_dup(sqlite3_errstr(rc));
    }
}

[[gnu::cold, gnu::nonnull(1, 2)]]
/** Builds a formatted error message for expected user errors. */
static const char *NONNULL errmsg_vprintf(const char *NONNULL restrict format, va_list args) {
    static constexpr const size_t BUFSIZE = 128;

    char *error_message = malloc(BUFSIZE * sizeof(char));
    if unlikely (error_message == NULL) {
        return OUT_OF_MEMORY_ERROR;
    }

    const int rv = vsnprintf(error_message, BUFSIZE, format, args);
    if likely (rv > 0) {
        return error_message;
    } else {
        free(error_message);
        return UNKNOWN_ERROR;
    }
}

[[gnu::format(printf, 2, 3), gnu::nonnull(2)]]
/** Builds a formatted error message for expected user errors. */
static inline void errmsg_printf(message_t *NULLABLE errmsg, const char *NONNULL restrict format, ...) {
    if likely (errmsg != NULL) {
        va_list args;
        va_start(args);
        *errmsg = errmsg_vprintf(format, args);
        va_end(args);
        return;
    }
}

/** Frees a dynamically allocated error message string. */
void db_free_errmsg(const char *NONNULL errmsg) {
    // this string should have been "allocated" with `errmsg_dup`, which might return these static strings
    if likely (!is_predefined_errmsg(errmsg)) {
        free((char *) errmsg);
    }
}

[[gnu::nonnull(1), gnu::cold]]
/**
 * Closes an open SQLite3 database connection, free resources and set `errmsg`, if necessary.
 */
static bool db_close(sqlite3 *NULLABLE db, message_t *NULLABLE errmsg) {
    const int rv = sqlite3_close(db);  // safe to call with NULL
    if likely (rv == SQLITE_OK) {
        return true;
    }

    errmsg_dup_db(errmsg, db);
    if unlikely (rv != SQLITE_BUSY) {
        return false;
    }

    // try interrupting and close once again, but let caller know about the error
    sqlite3_interrupt(db);
    sqlite3_close(db);
    return false;
}

[[gnu::malloc, gnu::nonnull(1)]]
/**
 * Open a database at `filepath`, either connecting to an existing database or creating a new one whe `create` is true.
 */
static sqlite3 *NULLABLE
    db_open(const char filepath[NONNULL restrict], message_t *NULLABLE restrict errmsg, bool create) {
    const int FLAGS = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE | SQLITE_OPEN_EXRESCODE;

    sqlite3 *db;
    int rv = sqlite3_open_v2(filepath, &db, create ? FLAGS | SQLITE_OPEN_CREATE : FLAGS, NULL);
    if unlikely (rv != SQLITE_OK) {
        errmsg_dup_db(errmsg, db);
        db_close(db, NULL);
        return NULL;
    }

    return db;
}

[[gnu::cold]]
/** Finalize SQLite global variables. */
static void db_shutdown(void) {
    int rv = sqlite3_shutdown();
    if unlikely (rv != SQLITE_OK) {
        (void) fprintf(stderr, "sqlite3_shutdown failed: %s\n", sqlite3_errstr(rv));
    }
}

[[gnu::cold]]
/** Apply schema from SQL file. */
static bool db_create_schema(sqlite3 *NONNULL db, message_t *NULLABLE errmsg) {
    char *errorbuf = NULL;  // will be allocated via sqlite3_malloc, need to copied to std malloc

    int rv = sqlite3_exec(db, SCHEMA, NULL, NULL, likely(errmsg != NULL) ? &errorbuf : NULL);
    if unlikely (rv != SQLITE_OK) {
        errmsg_dup_str(errmsg, errorbuf);
        sqlite3_free(errorbuf);  // safe to call with NULL
        return false;
    }

    return true;
}

/** Create or migrate database at `filepath`. */
bool db_setup(const char filepath[NONNULL restrict], message_t *NULLABLE restrict errmsg) {
    int rv = sqlite3_initialize();
    if unlikely (rv != SQLITE_OK) {
        errmsg_dup_rc(errmsg, rv);
        return false;
    }

    rv = atexit(db_shutdown);
    if unlikely (rv != 0) {
        // ATEXIT_NOT_REGISTERED_ERROR is statically predefined because `atexit` only fails on out-of-memory
        // situations, so it doesn't make sense to try another allocation here
        errmsg_dup_str(errmsg, ATEXIT_NOT_REGISTERED_ERROR);
        db_shutdown();
        return false;
    }

    sqlite3 *db = db_open(filepath, errmsg, true);
    if unlikely (db == NULL) {
        // `db_open` already sets `errmsg`
        return false;
    }

    bool ok = db_create_schema(db, errmsg);
    if unlikely (!ok) {
        db_close(db, NULL);
        return false;
    }

    return db_close(db, errmsg);
}

/**
 * A connection to the database file, which is a SQLite3 connection with cached statements.
 */
struct [[gnu::aligned(ALIGNMENT_DB_CONN)]] database_connection {
    /** The actual connection. */
    sqlite3 *NONNULL db;
    /** Internal buffer for string output. */
    movie_builder_t *NONNULL restrict builder;
    /** BEGIN TRANSACTION. */
    sqlite3_stmt *NONNULL op_begin;
    /** COMMIT (or END) TRANSACTION. */
    sqlite3_stmt *NONNULL op_commit;
    /** ROLLBACK TRANSACTION. */
    sqlite3_stmt *NONNULL op_rollback;
    /** REINDEX. */
    sqlite3_stmt *NONNULL op_reindex;
    /** Register new movie into database and returns the id. */
    sqlite3_stmt *NONNULL op_insert_movie;
    /** Register new genre, if not existent. */
    sqlite3_stmt *NONNULL op_insert_genre;
    /** Add genre to movie. */
    sqlite3_stmt *NONNULL op_insert_genre_link;
    /** Remove movie from database. */
    sqlite3_stmt *NONNULL op_delete_movie;
    /** Remove all genres without movies. */
    sqlite3_stmt *NONNULL op_delete_unused_genres;
    /** List all movie ids and titles. */
    sqlite3_stmt *NONNULL op_select_all_titles;
    /** List all movies and all information. */
    sqlite3_stmt *NONNULL op_select_all_movies;
    /** List information for a single movie. */
    sqlite3_stmt *NONNULL op_select_movie;
    /** List all genres for a single movie. */
    sqlite3_stmt *NONNULL op_select_movie_genres;
    /** List all movies for a single genre. */
    sqlite3_stmt *NONNULL op_select_movies_genre;
};
// ensure no padding here, even after correct alignment
static_assert(sizeof(db_conn_t) == alignof(db_conn_t));
static_assert(sizeof(db_conn_t) == offsetof(db_conn_t, op_select_movies_genre) + sizeof(void *));

[[gnu::malloc, gnu::nonnull(1, 3, 4)]]
/** Build a SQLite statement for persistent use. Returns NULL on failure. */
static sqlite3_stmt *NULLABLE db_prepare(
    sqlite3 *NONNULL db,
    size_t len,
    const char sql[NONNULL restrict len],
    bool *NONNULL has_error,  // shared error flag, for creating multiple statements in series
    message_t *NULLABLE restrict errmsg
) {
    assume(len < INT_MAX);
    assume(len == strlen(sql));
    // skip prepare if an error already happened before
    if unlikely (*has_error) {
        return NULL;
    }

    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    static constexpr const int FLAGS = SQLITE_PREPARE_PERSISTENT | SQLITE_PREPARE_NO_VTAB;

    const int rv = sqlite3_prepare_v3(db, sql, ((int) len) + 1, FLAGS, &stmt, &tail);
    if unlikely (rv != SQLITE_OK) {
        *has_error = true;
        errmsg_dup_db(errmsg, db);
        assume(stmt == NULL);
        return NULL;
    }

    assume(tail == &sql[len]);
    return stmt;
}

[[gnu::nonnull(1)]]
/** Create all used statements beforehand, for faster reuse later. */
static bool db_prepare_stmts(db_conn_t *NONNULL conn, message_t *NULLABLE errmsg) {
    sqlite3 *NONNULL db = conn->db;
    bool has_error = false;

/** Basic SQL macro: converts arguments into text and passes that to `db_prepare`. */
#define SQL(...)   SQL_(STR(__VA_ARGS__))
#define SQL_(stmt) db_prepare(db, strlen(stmt), stmt, &has_error, errmsg)
    sqlite3_stmt *begin = SQL(
        BEGIN DEFERRED TRANSACTION;
    );
    sqlite3_stmt *commit = SQL(
        COMMIT TRANSACTION;
    );
    sqlite3_stmt *rollback = SQL(
        ROLLBACK TRANSACTION;
    );
    sqlite3_stmt *reindex = SQL(
        REINDEX;
    );
    sqlite3_stmt *insert_movie = SQL(
        INSERT INTO movie(title, director, release_year)
            VALUES (:title, :director, :release_year)
            RETURNING movie.id;
    );
    sqlite3_stmt *insert_genre = SQL(
        INSERT OR IGNORE INTO genre(name)
            VALUES (:genre);
    );
    sqlite3_stmt *insert_genre_link =
        SQL(
        INSERT INTO movie_genre(movie_id, genre_id)
            SELECT :movie, genre.id
                FROM genre
                WHERE genre.name = :genre;
    );
    sqlite3_stmt *delete_movie = SQL(
        DELETE FROM movie
            WHERE id = :movie;
    );
    sqlite3_stmt *delete_unused_genres = SQL(
        DELETE FROM genre
            WHERE id NOT IN (
                SELECT DISTINCT genre_id
                    FROM movie_genre
            );
    );
    sqlite3_stmt *select_all_titles = SQL(
        SELECT id, title
            FROM movie;
    );
    sqlite3_stmt *select_all_movies = SQL(
        SELECT id, title, director, release_year
            FROM movie;
    );
    sqlite3_stmt *select_movie = SQL(
        SELECT id, title, director, release_year
            FROM movie
            WHERE id = :movie;
    );
    sqlite3_stmt *select_movies_genre =
        SQL(
        SELECT movie.id, movie.title, movie.director, movie.release_year
            FROM movie_genre
                INNER JOIN movie ON movie.id = movie_genre.movie_id
                INNER JOIN genre ON genre.id = movie_genre.genre_id
            WHERE genre.name = :genre;
    );
    sqlite3_stmt *select_movie_genres =
        SQL(
        SELECT genre.name
            FROM genre
                INNER JOIN movie_genre ON genre.id = genre_id
            WHERE movie_id = :movie;
    );
#undef SQL_
#undef SQL

    if unlikely (has_error) {
        // safe to call with NULL
        sqlite3_finalize(begin);
        sqlite3_finalize(commit);
        sqlite3_finalize(rollback);
        sqlite3_finalize(reindex);
        sqlite3_finalize(insert_movie);
        sqlite3_finalize(insert_genre);
        sqlite3_finalize(insert_genre_link);
        sqlite3_finalize(delete_movie);
        sqlite3_finalize(delete_unused_genres);
        sqlite3_finalize(select_all_titles);
        sqlite3_finalize(select_all_movies);
        sqlite3_finalize(select_movie);
        sqlite3_finalize(select_movie_genres);
        sqlite3_finalize(select_movies_genre);
        return false;
    }

#define set_stmt(name)      \
    assume((name) != NULL); \
    conn->op_##name = (name)

    set_stmt(begin);
    set_stmt(commit);
    set_stmt(rollback);
    set_stmt(reindex);
    set_stmt(insert_movie);
    set_stmt(insert_genre);
    set_stmt(insert_genre_link);
    set_stmt(delete_movie);
    set_stmt(delete_unused_genres);
    set_stmt(select_all_titles);
    set_stmt(select_all_movies);
    set_stmt(select_movie);
    set_stmt(select_movie_genres);
    set_stmt(select_movies_genre);
#undef set_stmt

    return true;
}

/** Connects to the existing database at `filepath`. */
db_conn_t *NULLABLE db_connect(const char filepath[NONNULL restrict], message_t *NULLABLE restrict errmsg) {
    db_conn_t *conn = alloc_like(struct database_connection);
    if unlikely (conn == NULL) {
        errmsg_dup_str(errmsg, OUT_OF_MEMORY_ERROR);
        return NULL;
    }

    movie_builder_t *builder = movie_builder_create();
    if unlikely (builder == NULL) {
        errmsg_dup_str(errmsg, OUT_OF_MEMORY_ERROR);
        free(conn);
        return NULL;
    }

    sqlite3 *db = db_open(filepath, errmsg, false);
    if unlikely (db == NULL) {
        // `db_open` already sets `errmsg`
        movie_builder_destroy(builder);
        free(conn);
        return NULL;
    }

    conn->db = db;
    conn->builder = builder;
    const bool ok = db_prepare_stmts(conn, errmsg);
    if unlikely (!ok) {
        db_close(db, NULL);
        movie_builder_destroy(builder);
        free(conn);
        return NULL;
    }

    // last verification that all pointers are non null
    for (size_t i = 0; i < sizeof(db_conn_t) / sizeof(void *); i++) {
        const void *const *start = (const void *const *) conn;
        assume(start[i] != NULL);
    }
    return conn;
}

[[gnu::nonnull(1, 2, 3)]]
/** Closes a prepared statement. Used during disconnect. */
static void db_finalize(sqlite3 *NONNULL db, sqlite3_stmt *NONNULL stmt, bool *NONNULL ok, message_t *NULLABLE errmsg) {
    int rv = sqlite3_finalize(stmt);
    if unlikely (rv != SQLITE_OK) {
        // set errmsg on the first error only
        if (*ok) {
            errmsg_dup_db(errmsg, db);
        }
        *ok = false;
    }
}

/** Disconnects to the database and free resources. */
bool db_disconnect(db_conn_t *NONNULL conn, message_t *NULLABLE errmsg) {
    bool ok = true;
    sqlite3 *db = conn->db;
    db_finalize(db, conn->op_begin, &ok, errmsg);
    db_finalize(db, conn->op_commit, &ok, errmsg);
    db_finalize(db, conn->op_rollback, &ok, errmsg);
    db_finalize(db, conn->op_reindex, &ok, errmsg);
    db_finalize(db, conn->op_insert_movie, &ok, errmsg);
    db_finalize(db, conn->op_insert_genre, &ok, errmsg);
    db_finalize(db, conn->op_insert_genre_link, &ok, errmsg);
    db_finalize(db, conn->op_delete_movie, &ok, errmsg);
    db_finalize(db, conn->op_delete_unused_genres, &ok, errmsg);
    db_finalize(db, conn->op_select_all_titles, &ok, errmsg);
    db_finalize(db, conn->op_select_all_movies, &ok, errmsg);
    db_finalize(db, conn->op_select_movie, &ok, errmsg);
    db_finalize(db, conn->op_select_movie_genres, &ok, errmsg);
    db_finalize(db, conn->op_select_movies_genre, &ok, errmsg);
    ok = db_close(db, ok ? errmsg : NULL) && ok;
    movie_builder_destroy(conn->builder);

    memset(conn, 0, sizeof(struct database_connection));
    free(conn);
    return ok;
}

[[gnu::const]]
/**
 * Translate SQLite3 extended error codes into simpler values.
 *
 * @param rv Return value of the last statement operation.
 * @param rrv Return value of the `sqlite3_reset()` call after that.
 */
static db_result_t check_result(const int rv, const int rrv) {
    // if `sqlite3_reset()` fails, we restart the thread
    if unlikely (rrv != SQLITE_OK) {
        return DB_HARD_ERROR;
    }

    switch (rv) {
        /* DB_SUCCESS */
        case SQLITE_DONE:
        case SQLITE_OK_LOAD_PERMANENTLY:
        case SQLITE_OK_SYMLINK:
        case SQLITE_OK:
            return DB_SUCCESS;
        /* DB_HARD_ERROR */
        case SQLITE_CANTOPEN_CONVPATH:
        case SQLITE_CANTOPEN_DIRTYWAL:
        case SQLITE_CANTOPEN_FULLPATH:
        case SQLITE_CANTOPEN_ISDIR:
        case SQLITE_CANTOPEN_NOTEMPDIR:
        case SQLITE_CANTOPEN_SYMLINK:
        case SQLITE_CORRUPT_INDEX:
        case SQLITE_CORRUPT_SEQUENCE:
        case SQLITE_CORRUPT_VTAB:
        case SQLITE_CORRUPT:
        case SQLITE_INTERNAL:
        case SQLITE_INTERRUPT:
        case SQLITE_IOERR_AUTH:
        case SQLITE_IOERR_BEGIN_ATOMIC:
        case SQLITE_IOERR_BLOCKED:
        case SQLITE_IOERR_CHECKRESERVEDLOCK:
        case SQLITE_IOERR_CLOSE:
        case SQLITE_IOERR_COMMIT_ATOMIC:
        case SQLITE_IOERR_CONVPATH:
        case SQLITE_IOERR_CORRUPTFS:
        case SQLITE_IOERR_DATA:
        case SQLITE_IOERR_DIR_CLOSE:
        case SQLITE_IOERR_DIR_FSYNC:
        case SQLITE_IOERR_FSTAT:
        case SQLITE_IOERR_FSYNC:
        case SQLITE_IOERR_GETTEMPPATH:
        case SQLITE_IOERR_IN_PAGE:
        case SQLITE_IOERR_LOCK:
        case SQLITE_IOERR_MMAP:
        case SQLITE_IOERR_READ:
        case SQLITE_IOERR_ROLLBACK_ATOMIC:
        case SQLITE_IOERR_SHORT_READ:
        case SQLITE_IOERR_UNLOCK:
        case SQLITE_IOERR_VNODE:
        case SQLITE_IOERR_WRITE:
        case SQLITE_MISUSE:
        case SQLITE_NOTADB:
        case SQLITE_NOTFOUND:
        case SQLITE_PERM:
        case SQLITE_READONLY_CANTINIT:
        case SQLITE_READONLY_CANTLOCK:
        case SQLITE_READONLY_DBMOVED:
        case SQLITE_READONLY_DIRECTORY:
        case SQLITE_READONLY_RECOVERY:
        case SQLITE_READONLY_ROLLBACK:
        case SQLITE_READONLY:
            return DB_HARD_ERROR;
        /* DB_RUNTIME_ERROR */
        case SQLITE_ABORT_ROLLBACK:
        case SQLITE_ABORT:
        case SQLITE_BUSY_RECOVERY:
        case SQLITE_BUSY_SNAPSHOT:
        case SQLITE_BUSY_TIMEOUT:
        case SQLITE_BUSY:
        case SQLITE_CANTOPEN:
        case SQLITE_ERROR_RETRY:
        case SQLITE_ERROR_SNAPSHOT:
        case SQLITE_FULL:
        case SQLITE_IOERR_ACCESS:
        case SQLITE_IOERR_DELETE_NOENT:
        case SQLITE_IOERR_DELETE:
        case SQLITE_IOERR_NOMEM:
        case SQLITE_IOERR_RDLOCK:
        case SQLITE_IOERR_SEEK:
        case SQLITE_IOERR_SHMLOCK:
        case SQLITE_IOERR_SHMMAP:
        case SQLITE_IOERR_SHMOPEN:
        case SQLITE_IOERR_SHMSIZE:
        case SQLITE_IOERR_TRUNCATE:
        case SQLITE_IOERR:
        case SQLITE_LOCKED_SHAREDCACHE:
        case SQLITE_LOCKED_VTAB:
        case SQLITE_LOCKED:
        case SQLITE_NOLFS:
        case SQLITE_NOMEM:
        case SQLITE_PROTOCOL:
        case SQLITE_ROW:
        case SQLITE_SCHEMA:
            return DB_RUNTIME_ERROR;
        /* DB_USER_ERROR */
        case SQLITE_AUTH_USER:
        case SQLITE_AUTH:
        case SQLITE_CONSTRAINT_CHECK:
        case SQLITE_CONSTRAINT_COMMITHOOK:
        case SQLITE_CONSTRAINT_DATATYPE:
        case SQLITE_CONSTRAINT_FOREIGNKEY:
        case SQLITE_CONSTRAINT_FUNCTION:
        case SQLITE_CONSTRAINT_NOTNULL:
        case SQLITE_CONSTRAINT_PINNED:
        case SQLITE_CONSTRAINT_PRIMARYKEY:
        case SQLITE_CONSTRAINT_ROWID:
        case SQLITE_CONSTRAINT_TRIGGER:
        case SQLITE_CONSTRAINT_UNIQUE:
        case SQLITE_CONSTRAINT_VTAB:
        case SQLITE_CONSTRAINT:
        case SQLITE_EMPTY:
        case SQLITE_ERROR_MISSING_COLLSEQ:
        case SQLITE_ERROR:
        case SQLITE_FORMAT:
        case SQLITE_MISMATCH:
        case SQLITE_NOTICE_RBU:
        case SQLITE_NOTICE_RECOVER_ROLLBACK:
        case SQLITE_NOTICE_RECOVER_WAL:
        case SQLITE_NOTICE:
        case SQLITE_RANGE:
        case SQLITE_TOOBIG:
        case SQLITE_WARNING_AUTOINDEX:
        case SQLITE_WARNING:
        default:
            return DB_USER_ERROR;
    }
}

[[gnu::pure, gnu::nonnull(2)]]
/** Checks a list of return values sequentially. */
static db_result_t check_results(size_t n, const int rv[NONNULL const n], const int rrv) {
    for (size_t i = 0; i < n; i++) {
        db_result_t result = check_result(rv[i], rrv);
        if (result != DB_SUCCESS) {
            return result;
        }
    }
    return DB_SUCCESS;
}

[[gnu::nonnull(1, 2), gnu::hot]]
/** Runs a single transaction statement and reset it. */
static db_result_t db_transaction_op(db_conn_t *NONNULL conn, sqlite3_stmt *NONNULL stmt, message_t *NULLABLE errmsg) {
    int rv = sqlite3_step(stmt);
    int rrv = sqlite3_reset(stmt);
    if unlikely (rv != SQLITE_DONE || rrv != SQLITE_OK) {
        errmsg_dup_db(errmsg, conn->db);
        return check_result(rv, rrv);
    }
    return DB_SUCCESS;
}

[[gnu::nonnull(1), gnu::hot]]
/** Runs `BEGIN TRANSACTION`. */
static db_result_t db_transaction_begin(db_conn_t *NONNULL conn, message_t *NULLABLE errmsg) {
    return db_transaction_op(conn, conn->op_begin, errmsg);
}

[[gnu::nonnull(1), gnu::hot]]
/** Runs `ROLLBACK TRANSACTION`. */
static db_result_t db_transaction_rollback(db_conn_t *NONNULL conn, message_t *NULLABLE errmsg) {
    return db_transaction_op(conn, conn->op_rollback, errmsg);
}

[[gnu::nonnull(1), gnu::hot]]
/** Runs `COMMIT TRANSACTION`. */
static db_result_t db_transaction_commit(db_conn_t *NONNULL conn, message_t *NULLABLE errmsg) {
    return db_transaction_op(conn, conn->op_commit, errmsg);
}

[[gnu::nonnull(1), gnu::hot]]
/** Step through statement, ignoring results. */
static db_result_t db_eval_stmt(sqlite3_stmt *NONNULL stmt) {
    int rv = SQLITE_OK;
    do {
        rv = sqlite3_step(stmt);
    } while (rv == SQLITE_ROW);

    sqlite3_clear_bindings(stmt);
    int rrv = sqlite3_reset(stmt);
    if unlikely (rv != SQLITE_DONE || rrv != SQLITE_OK) {
        return check_result(rv, rrv);
    }

    return DB_SUCCESS;
}

[[gnu::nonnull(2)]]
/** Runs `op_insert_movie` inside an open transaction. */
static db_result_t register_movie_in_transaction(const db_conn_t conn, struct movie *NONNULL movie) {
    const size_t genres = movie->genre_count;

    // add all movie genres to db
    for (size_t i = 0; i < genres; i++) {
        int rv = sqlite3_bind_text(conn.op_insert_genre, 1, movie->genres[i], -1, SQLITE_STATIC);
        if unlikely (rv != SQLITE_OK) {
            sqlite3_clear_bindings(conn.op_insert_genre);
            return check_result(rv, sqlite3_reset(conn.op_insert_genre));
        }

        db_result_t res = db_eval_stmt(conn.op_insert_genre);
        if unlikely (res != DB_SUCCESS) {
            return res;
        }
    }

    // add movie itself to db
    const int rvv[3] = {
        sqlite3_bind_text(conn.op_insert_movie, 1, movie->title, -1, SQLITE_STATIC),
        sqlite3_bind_text(conn.op_insert_movie, 2, movie->director, -1, SQLITE_STATIC),
        sqlite3_bind_int(conn.op_insert_movie, 3, movie->release_year),
    };
    if unlikely (rvv[0] != SQLITE_OK || rvv[1] != SQLITE_OK || rvv[2] != SQLITE_OK) {
        sqlite3_clear_bindings(conn.op_insert_movie);
        return check_results(3, rvv, sqlite3_reset(conn.op_insert_movie));
    }

    int rv;
    unsigned id_set = 0;
    while ((rv = sqlite3_step(conn.op_insert_movie)) == SQLITE_ROW) {
        movie->id = sqlite3_column_int64(conn.op_insert_movie, 0);
        id_set += 1;
    }

    sqlite3_clear_bindings(conn.op_insert_movie);
    int rrv = sqlite3_reset(conn.op_insert_movie);
    if unlikely (rv != SQLITE_DONE || rrv != SQLITE_OK) {
        return check_result(rv, rrv);
    } else if unlikely (id_set != 1) {
        return DB_HARD_ERROR;
    }

    // link movie to the genres
    for (size_t i = 0; i < genres; i++) {
        const int rvv[2] = {
            sqlite3_bind_int64(conn.op_insert_genre_link, 1, movie->id),
            sqlite3_bind_text(conn.op_insert_genre_link, 2, movie->genres[i], -1, SQLITE_STATIC),
        };
        if unlikely (rvv[0] != SQLITE_OK || rvv[1] != SQLITE_OK) {
            sqlite3_clear_bindings(conn.op_insert_genre_link);
            return check_results(2, rvv, sqlite3_reset(conn.op_insert_genre_link));
        }

        db_result_t res = db_eval_stmt(conn.op_insert_genre_link);
        if unlikely (res != DB_SUCCESS) {
            return res;
        }
    }

    return DB_SUCCESS;
}

/** Registers a new movie and updates its 'id' if successful. */
db_result_t db_register_movie(
    db_conn_t *NONNULL conn,
    struct movie *NONNULL movie,
    message_t *NULLABLE restrict errmsg
) {
    assume(movie->id == 0);

    db_result_t res = db_transaction_begin(conn, errmsg);
    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    res = register_movie_in_transaction(*conn, movie);
    if unlikely (res != DB_SUCCESS) {
        errmsg_dup_db(errmsg, conn->db);
        db_transaction_rollback(conn, NULL);
        return res;
    }

    return db_transaction_commit(conn, errmsg);
}

[[gnu::nonnull(3)]]
/** Runs `op_insert_genre_link` inside an open transaction. */
static db_result_t add_genres_in_transaction(
    const db_conn_t conn,
    size_t len,
    const char *NONNULL restrict const genres[NONNULL len],
    int64_t movie_id
) {
    for (size_t i = 0; i < len; i++) {
        int rv = sqlite3_bind_text(conn.op_insert_genre, 1, genres[i], -1, SQLITE_STATIC);
        if unlikely (rv != SQLITE_OK) {
            sqlite3_clear_bindings(conn.op_insert_genre);
            return check_result(rv, sqlite3_reset(conn.op_insert_genre));
        }

        db_result_t res = db_eval_stmt(conn.op_insert_genre);
        if unlikely (res != DB_SUCCESS) {
            return res;
        }
    }

    for (size_t i = 0; i < len; i++) {
        const int rvv[2] = {
            sqlite3_bind_int64(conn.op_insert_genre_link, 1, movie_id),
            sqlite3_bind_text(conn.op_insert_genre_link, 2, genres[i], -1, SQLITE_STATIC),
        };
        if unlikely (rvv[0] != SQLITE_OK || rvv[1] != SQLITE_OK) {
            sqlite3_clear_bindings(conn.op_insert_genre_link);
            return check_results(2, rvv, sqlite3_reset(conn.op_insert_genre_link));
        }

        db_result_t res = db_eval_stmt(conn.op_insert_genre_link);
        if unlikely (res != DB_SUCCESS) {
            return res;
        }
    }
    return DB_SUCCESS;
}

/** Adds a list of genres tp an existing movie. */
db_result_t db_add_genre(
    db_conn_t *NONNULL conn,
    int64_t movie_id,
    const char genre[NONNULL restrict const],
    message_t *NULLABLE restrict errmsg
) {
    db_result_t res = db_transaction_begin(conn, errmsg);
    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    res = add_genres_in_transaction(*conn, 1, &genre, movie_id);
    if likely (res == DB_SUCCESS) {
        return db_transaction_commit(conn, errmsg);
    }

    switch (sqlite3_extended_errcode(conn->db)) {
        case SQLITE_CONSTRAINT_FOREIGNKEY:
            errmsg_printf(errmsg, "no movie with id = %" PRIi64 " found in the database", movie_id);
            res = DB_USER_ERROR;
            break;
        case SQLITE_CONSTRAINT_UNIQUE:
            errmsg_printf(errmsg, "movie with id = %" PRIi64 " already has the provided genre", movie_id);
            res = DB_USER_ERROR;
            break;
        default: {
            errmsg_dup_db(errmsg, conn->db);
            break;
        }
    }
    db_transaction_rollback(conn, NULL);
    return res;
}

/** Runs `op_delete_movie` inside its automatic transaction. */
static db_result_t delete_movie_in_transaction(const db_conn_t conn, int64_t movie_id) {
    int rv = sqlite3_bind_int64(conn.op_delete_movie, 1, movie_id);
    if unlikely (rv != SQLITE_OK) {
        sqlite3_clear_bindings(conn.op_delete_movie);
        return check_result(rv, sqlite3_reset(conn.op_delete_movie));
    }

    return db_eval_stmt(conn.op_delete_movie);
}

/** Runs `op_delete_unused_genres` inside its automatic transaction. */
static void delete_unused_genres_in_transaction(const db_conn_t conn) {
    db_result_t result = db_eval_stmt(conn.op_delete_unused_genres);
    if unlikely (result != DB_SUCCESS) {
        const char *NONNULL errmsg = sqlite3_errmsg(conn.db);
        (void) fprintf(stderr, "failed to delete unused genres: %s\n", errmsg);
        // just print errors for this one, and keeps running
    }
}

/** Removes a movie from the database. */
db_result_t db_delete_movie(db_conn_t *NONNULL conn, int64_t movie_id, message_t *NULLABLE errmsg) {
    // no need to create an explicit transaction for a single statement
    const db_result_t res = delete_movie_in_transaction(*conn, movie_id);
    if unlikely (res != DB_SUCCESS) {
        errmsg_dup_db(errmsg, conn->db);
        return res;
    }

    if (sqlite3_changes64(conn->db) < 1) {
        errmsg_printf(errmsg, "no movie with id = %" PRIi64 " to be deleted from the database", movie_id);
        return DB_USER_ERROR;
    }

    delete_unused_genres_in_transaction(*conn);
    return DB_SUCCESS;
}

[[gnu::nonnull(1, 3)]]
/** Extract `column` as UTF8 string. Writes length in `length`. */
static const char *NULLABLE get_str_column(sqlite3_stmt *NONNULL stmt, int column, size_t *NONNULL length) {
    const char *str = (const char *) sqlite3_column_text(stmt, column);
    if unlikely (str == NULL) {
        return NULL;
    }

    const int bytes = sqlite3_column_bytes(stmt, column);
    if likely (bytes >= 0) {
        assert(str[bytes] == '\0');
        *length = (size_t) bytes;
    } else {
        *length = strlen(str);
    }
    return str;
}

[[gnu::nonnull(1, 2, 3)]]
/** Build movie data into `buffer` and correct pointers to `movie`. */
static db_result_t get_movie_with_genres(
    movie_builder_t *NONNULL builder,
    sqlite3_stmt *NONNULL outer_stmt,
    sqlite3_stmt *NONNULL inner_stmt
) {
    size_t title_len;
    size_t director_len;
    const int64_t id = sqlite3_column_int64(outer_stmt, 0);
    const int release_year = sqlite3_column_int(outer_stmt, 3);
    const char *director = get_str_column(outer_stmt, 2, &director_len);
    const char *title = get_str_column(outer_stmt, 1, &title_len);
    // ignore results on allocation issues
    if unlikely (title == NULL || director == NULL) {
        return DB_RUNTIME_ERROR;
    }

    movie_builder_set_id(builder, id);
    movie_builder_set_release_year(builder, release_year);
    bool ok1 = movie_builder_set_title(builder, title_len, title);
    bool ok2 = movie_builder_set_director(builder, director_len, director);
    if unlikely (!ok1 || !ok2) {
        return DB_RUNTIME_ERROR;
    }

    int rv = sqlite3_bind_int64(inner_stmt, 1, id);
    if unlikely (rv != SQLITE_OK) {
        sqlite3_clear_bindings(inner_stmt);
        return check_result(rv, sqlite3_reset(inner_stmt));
    }

    movie_builder_start_genres(builder);
    while ((rv = sqlite3_step(inner_stmt)) == SQLITE_ROW) {
        size_t genre_len;
        const char *genre = get_str_column(inner_stmt, 0, &genre_len);
        if unlikely (genre == NULL) {
            break;
        }

        bool ok = movie_builder_add_genre(builder, genre_len, genre);
        if unlikely (!ok) {
            break;
        }
    }

    sqlite3_clear_bindings(inner_stmt);
    int rrv = sqlite3_reset(inner_stmt);
    if unlikely ((rv != SQLITE_DONE && rv != SQLITE_ROW) || rrv != SQLITE_OK) {
        return check_result(rv, rrv);
    }

    return DB_SUCCESS;
}

[[gnu::nonnull(1, 2, 3)]]
/** Iterate over movie entries, calling `callback` on each and returning the final result in `movie`.  */
static db_result_t iter_movies(
    movie_builder_t *NONNULL builder,
    sqlite3_stmt *NONNULL outer_stmt,
    sqlite3_stmt *NONNULL inner_stmt
) {
    movie_builder_reset(builder);

    int rv;
    db_result_t res;
    while ((rv = sqlite3_step(outer_stmt)) == SQLITE_ROW) {
        res = get_movie_with_genres(builder, outer_stmt, inner_stmt);
        if unlikely (res != DB_SUCCESS) {
            break;
        }

        bool ok = movie_builder_add_current_movie_to_list(builder);
        if unlikely (!ok) {
            res = DB_RUNTIME_ERROR;
            break;
        }
    }

    sqlite3_clear_bindings(outer_stmt);
    int rrv = sqlite3_reset(outer_stmt);
    if unlikely ((rv != SQLITE_DONE && rv != SQLITE_ROW) || rrv != SQLITE_OK) {
        return check_result(rv, rrv);
    } else if unlikely (res != DB_SUCCESS) {
        return res;
    }

    return DB_SUCCESS;
}

[[gnu::nonnull(3)]]
/** Read a single movie and write to `movie`. */
static db_result_t get_movie_in_transaction(const db_conn_t conn, int64_t movie_id, struct movie *NONNULL output) {
    int rv = sqlite3_bind_int64(conn.op_select_movie, 1, movie_id);
    if unlikely (rv != SQLITE_OK) {
        sqlite3_clear_bindings(conn.op_select_movie);
        return check_result(rv, sqlite3_reset(conn.op_select_movie));
    }

    const db_result_t res = iter_movies(conn.builder, conn.op_select_movie, conn.op_select_movie_genres);
    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    size_t count = movie_builder_list_size(conn.builder);
    if unlikely (count < 1) {
        return DB_USER_ERROR;
    }

    bool ok = movie_builder_take_movie_from_list(conn.builder, 0, output);
    return unlikely(!ok) ? DB_USER_ERROR : DB_SUCCESS;
}

/** Get a movie from the database. */
db_result_t db_get_movie(
    db_conn_t *NONNULL conn,
    int64_t movie_id,
    struct movie *NONNULL output,
    message_t *NULLABLE restrict errmsg
) {
    db_result_t res = db_transaction_begin(conn, errmsg);
    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    res = get_movie_in_transaction(*conn, movie_id, output);
    if likely (res == DB_SUCCESS) {
        res = db_transaction_commit(conn, errmsg);
        if unlikely (res != DB_SUCCESS) {
            free_movie(*output);
        }
        return res;
    }

    if (sqlite3_extended_errcode(conn->db) != SQLITE_OK) {
        errmsg_dup_db(errmsg, conn->db);
    } else {
        switch (res) {
            case DB_USER_ERROR:
                errmsg_printf(errmsg, "no movie with id = %" PRIi64 " found in the database", movie_id);
                break;
            case DB_RUNTIME_ERROR:
                errmsg_dup_str(errmsg, OUT_OF_MEMORY_ERROR);
                break;
            default:
                errmsg_dup_str(errmsg, UNKNOWN_ERROR);
                break;
        }
    }
    db_transaction_rollback(conn, errmsg);
    return res;
}

[[gnu::nonnull(2)]]
/** Read all movies and run callback on each. */
static db_result_t list_movies_in_transaction(
    const db_conn_t conn,
    struct movie *NONNULL *NONNULL output,
    size_t *NONNULL output_length
) {
    const db_result_t res = iter_movies(conn.builder, conn.op_select_all_movies, conn.op_select_movie_genres);

    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    size_t length;
    struct movie *list = movie_builder_take_movie_list(conn.builder, &length);
    if unlikely (list == NULL) {
        return DB_RUNTIME_ERROR;
    }

    *output = list;
    *output_length = length;
    return DB_SUCCESS;
}

/** List all movies with full information. */
db_result_t db_list_movies(
    db_conn_t *NONNULL conn,
    struct movie *NONNULL *NONNULL output,
    size_t *NONNULL output_length,
    message_t *NULLABLE restrict errmsg
) {
    db_result_t res = db_transaction_begin(conn, errmsg);
    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    res = list_movies_in_transaction(*conn, output, output_length);
    if likely (res == DB_SUCCESS) {
        return db_transaction_commit(conn, errmsg);
    }

    if (sqlite3_extended_errcode(conn->db) != SQLITE_OK) {
        errmsg_dup_db(errmsg, conn->db);
    } else {
        errmsg_dup_str(errmsg, (res == DB_RUNTIME_ERROR) ? OUT_OF_MEMORY_ERROR : UNKNOWN_ERROR);
    }
    db_transaction_rollback(conn, errmsg);
    return res;
}

[[gnu::nonnull(2)]]
/** Search through movies and run callback on each. */
static db_result_t search_movies_in_transaction(
    const db_conn_t conn,
    const char genre[NONNULL restrict const],
    struct movie *NONNULL *NONNULL output,
    size_t *NONNULL output_length
) {
    int rv = sqlite3_bind_text(conn.op_select_movies_genre, 1, genre, -1, SQLITE_STATIC);
    if unlikely (rv != SQLITE_OK) {
        sqlite3_clear_bindings(conn.op_select_movies_genre);
        return check_result(rv, sqlite3_reset(conn.op_select_movies_genre));
    }

    const db_result_t res = iter_movies(conn.builder, conn.op_select_movies_genre, conn.op_select_movie_genres);

    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    size_t length;
    struct movie *list = movie_builder_take_movie_list(conn.builder, &length);
    if unlikely (list == NULL) {
        return DB_RUNTIME_ERROR;
    }

    *output = list;
    *output_length = length;
    return DB_SUCCESS;
}

/* List all movies with a given genre. */
db_result_t db_search_movies_by_genre(
    db_conn_t *NONNULL conn,
    const char genre[NONNULL restrict const],
    struct movie *NONNULL *NONNULL output,
    size_t *NONNULL output_length,
    message_t *NULLABLE restrict errmsg
) {
    db_result_t res = db_transaction_begin(conn, errmsg);
    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    res = search_movies_in_transaction(*conn, genre, output, output_length);
    if likely (res == DB_SUCCESS) {
        return db_transaction_commit(conn, errmsg);
    }

    if (sqlite3_extended_errcode(conn->db) != SQLITE_OK) {
        errmsg_dup_db(errmsg, conn->db);
    } else {
        errmsg_dup_str(errmsg, (res == DB_RUNTIME_ERROR) ? OUT_OF_MEMORY_ERROR : UNKNOWN_ERROR);
    }
    db_transaction_rollback(conn, errmsg);
    return res;
}

[[gnu::nonnull(1, 2)]]
/** Build summary data using `buffer`. */
static db_result_t get_summary_in_list(movie_builder_t *NONNULL builder, sqlite3_stmt *NONNULL stmt) {
    size_t title_len;
    const int64_t id = sqlite3_column_int64(stmt, 0);
    const char *title = get_str_column(stmt, 1, &title_len);
    // ignore results on allocation issues
    if unlikely (title == NULL) {
        return DB_RUNTIME_ERROR;
    }

    movie_builder_set_id(builder, id);
    bool ok = movie_builder_set_title(builder, title_len, title);
    if unlikely (!ok) {
        return DB_RUNTIME_ERROR;
    }

    ok = movie_builder_add_current_summary_to_list(builder);
    if unlikely (!ok) {
        return DB_RUNTIME_ERROR;
    }
    return DB_SUCCESS;
}

[[gnu::nonnull(2)]]
/** Read title and id of all movies and run callback on each. */
static db_result_t list_summaries_in_transaction(
    const db_conn_t conn,
    struct movie_summary *NONNULL *NONNULL output,
    size_t *NONNULL output_length
) {
    movie_builder_reset(conn.builder);

    int rv;
    db_result_t res;
    while ((rv = sqlite3_step(conn.op_select_all_titles)) == SQLITE_ROW) {
        res = get_summary_in_list(conn.builder, conn.op_select_all_titles);
        if unlikely (res != DB_SUCCESS) {
            break;
        }
    }

    sqlite3_clear_bindings(conn.op_select_all_titles);
    int rrv = sqlite3_reset(conn.op_select_all_titles);
    if unlikely ((rv != SQLITE_DONE && rv != SQLITE_ROW) || rrv != SQLITE_OK) {
        return check_result(rv, rrv);
    } else if unlikely (res != DB_SUCCESS) {
        return res;
    }

    size_t length;
    struct movie_summary *list = movie_builder_take_summary_list(conn.builder, &length);
    if unlikely (list == NULL) {
        return DB_RUNTIME_ERROR;
    }

    *output = list;
    *output_length = length;
    return DB_SUCCESS;
}

/** List all movies with reduced information. */
db_result_t db_list_summaries(
    db_conn_t *NONNULL conn,
    struct movie_summary *NONNULL *NONNULL output,
    size_t *NONNULL output_length,
    message_t *NULLABLE restrict errmsg
) {
    db_result_t res = db_transaction_begin(conn, errmsg);
    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    res = list_summaries_in_transaction(*conn, output, output_length);
    if likely (res == DB_SUCCESS) {
        return db_transaction_commit(conn, errmsg);
    }

    if (sqlite3_extended_errcode(conn->db) != SQLITE_OK) {
        errmsg_dup_db(errmsg, conn->db);
    } else {
        errmsg_dup_str(errmsg, (res == DB_RUNTIME_ERROR) ? OUT_OF_MEMORY_ERROR : UNKNOWN_ERROR);
    }
    db_transaction_rollback(conn, errmsg);
    return res;
}
