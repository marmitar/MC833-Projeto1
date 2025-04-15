/** Database operations. */
#include "./sqlite_source.h"  // must be included first for defines

#include "defines.h"
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

#include "./database.h"
#include "./schema.h"

static const char UNKNOWN_ERROR[] = "unknown error";
static const char OUT_OF_MEMORY_ERROR[] = "out of memory";
static const char ATEXIT_NOT_REGISTERED_ERROR[] = "could not call at_exit";

[[gnu::cold, gnu::const, nodiscard]]
/** Predefined error messages, which are NOT dynamically allocated and should not be free. */
static inline bool is_predefined_errmsg(const char *NULLABLE const error_message) {
    return error_message == UNKNOWN_ERROR || error_message == OUT_OF_MEMORY_ERROR
        || error_message == ATEXIT_NOT_REGISTERED_ERROR;
}

[[gnu::cold, gnu::returns_nonnull, nodiscard]]
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

    const size_t len = strlen(error_message);
    char *copy = malloc((len + 1) * sizeof(char));
    if unlikely (copy == NULL) {
        return OUT_OF_MEMORY_ERROR;
    }

    memcpy(copy, error_message, len * sizeof(char));
    return copy;
}

[[gnu::cold]]
/** Copies the current SQLite error message from `str` into `errmsg`, if non-NULL. */
static void errmsg_dup_str(message *NULLABLE restrict errmsg, const char str[NULLABLE restrict]) {
    if likely (errmsg != NULL) {
        *errmsg = errmsg_dup(str);
    }
}

[[gnu::cold]]
/** Copies the current SQLite error message from `db` into `errmsg`, if non-NULL. */
static void errmsg_dup_db(message *NULLABLE errmsg, sqlite3 *NULLABLE db) {
    errmsg_dup_str(errmsg, sqlite3_errmsg(db));
}

[[gnu::cold]]
/** Copies the SQLite error string for the error code `rc` into `errmsg`, if non-NULL. */
static void errmsg_dup_rc(message *NULLABLE errmsg, const int rc) {
    errmsg_dup_str(errmsg, sqlite3_errstr(rc));
}

[[gnu::format(printf, 3, 4), gnu::nonnull(3)]]
/** Builds a formatted error message for expected user errors. */
static void errmsg_printf(message *NULLABLE errmsg, const size_t bufsize, const char *NONNULL restrict format, ...) {
    if unlikely (errmsg == NULL) {
        return;
    }

    char *error_message = malloc(bufsize * sizeof(char));
    if unlikely (error_message == NULL) {
        *errmsg = OUT_OF_MEMORY_ERROR;
        return;
    }

    va_list args;
    va_start(args, format);
    const int rv = vsnprintf(error_message, bufsize, format, args);
    va_end(args);

    if likely (rv > 0) {
        *errmsg = error_message;
    } else {
        *errmsg = UNKNOWN_ERROR;
        free(error_message);
    }
}

/** Frees a dynamically allocated error message string. */
void db_free_errmsg(const char *NONNULL errmsg) {
    // this string should have been "allocated" with `errmsg_dup`, which might return these static strings
    if likely (!is_predefined_errmsg(errmsg)) {
        free((char *) errmsg);
    }
}

[[gnu::regcall]]
/**
 * Closes an open SQLite3 database connection, free resources and set `errmsg`, if necessary.
 */
static bool db_close(sqlite3 *NULLABLE db, message *NULLABLE errmsg) {
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

[[gnu::regcall, gnu::malloc, gnu::nonnull(1)]]
/**
 * Open a database at `filepath`, either connecting to an existing database or creating a new one whe `create` is true.
 */
static sqlite3 *
    NULLABLE db_open(const char filepath[NONNULL restrict], message *NULLABLE restrict errmsg, bool create) {
    const int FLAGS = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE | SQLITE_OPEN_EXRESCODE;

    sqlite3 *db = NULL;
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
        fprintf(stderr, "sqlite3_shutdown failed: %s\n", sqlite3_errstr(rv));
    }
}

[[gnu::cold]]
/** Apply schema from SQL file. */
static bool db_create_schema(sqlite3 *NONNULL db, message *NULLABLE errmsg) {
    char *errorbuf = NULL;  // will be allocated via sqlite3_malloc, need to copied to std malloc

    int rv = sqlite3_exec(db, SCHEMA, nullptr, nullptr, likely (errmsg != NULL) ? &errorbuf : NULL);
    if unlikely (rv != SQLITE_OK) {
        errmsg_dup_str(errmsg, errorbuf);
        sqlite3_free(errorbuf);  // safe to call with NULL
        return false;
    }

    return true;
}

/** Create or migrate database at `filepath`. */
bool db_setup(const char filepath[NONNULL restrict], message *NULLABLE restrict errmsg) {
    int rv = sqlite3_initialize();
    if unlikely (rv != SQLITE_OK) {
        errmsg_dup_rc(errmsg, rv);
        return false;
    }

    rv = atexit(db_shutdown);
    if unlikely (rv != OK) {
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
 * Internal buffer for string output.
 *
 * Each buffer holds multiple NUL terminated strings. The strings contents are modifiable, but cannot be increased
 * in-place. A new string slice must be re
 */
struct string_buffer {
    /** Current allocated size for `data`. */
    size_t capacity;
    /** Currently in use part of the buffer. */
    size_t in_use;
    /** Modifiable shared string. */
    char *NONNULL restrict data;
};

static const constexpr size_t BUFFER_PAGE_SIZE = 4096;
static_assert(BUFFER_PAGE_SIZE > 0);

[[gnu::regcall, gnu::malloc]]
/** Allocates initial memory for a string buffer. */
static struct string_buffer *NULLABLE string_buffer_alloc(void) {
    struct string_buffer *buffer = calloc(1, sizeof(struct string_buffer));
    if unlikely (buffer == NULL) {
        return NULL;
    }

    char *data = malloc(BUFFER_PAGE_SIZE * sizeof(char));
    if unlikely (data == NULL) {
        free(buffer);
        return NULL;
    }

    buffer->data = data;
    buffer->capacity = BUFFER_PAGE_SIZE;
    buffer->in_use = 0;
    return buffer;
}

[[gnu::regcall, gnu::nonnull(1)]]
/** Release memory used for buffer. */
static void string_buffer_free(struct string_buffer *NONNULL buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(struct string_buffer));
    free(buffer);
}

[[gnu::const]]
/** Calculates $ceil(a / b)$. */
static inline size_t ceil_div(size_t a, size_t b) {
    return unlikely(a == 0) ? 0 : 1 + ((a - 1) / b);
}

[[gnu::regcall, gnu::nonnull(1)]]
/**
 * Request an allocated slice in string buffer.
 *
 * Returns the index for the slice, or `SIZE_MAX` if a slice of `size` bytes could be generated.
 */
static size_t string_buffer_slice(struct string_buffer *NONNULL buffer, size_t size) {
    assert(buffer->capacity >= buffer->in_use);
    if unlikely (size == SIZE_MAX) {
        return SIZE_MAX;
    }

    const size_t actual_size = size + 1;
    if likely (buffer->capacity - buffer->in_use >= actual_size) {
        size_t slice = buffer->in_use;
        buffer->in_use += actual_size;
        return slice;
    }

    size_t final_size = 0;
    if unlikely (ckd_add(&final_size, buffer->in_use, actual_size)) {
        return SIZE_MAX;
    }
    assert(final_size > buffer->in_use);

    const size_t alloc_pages = ceil_div(final_size, BUFFER_PAGE_SIZE);
    const size_t final_capacity = alloc_pages * BUFFER_PAGE_SIZE;

    char *data = realloc(buffer->data, final_capacity * sizeof(char));
    if unlikely (data == NULL) {
        return SIZE_MAX;
    }

    buffer->data = data;
    buffer->capacity = final_capacity;
    size_t slice = buffer->in_use;
    buffer->in_use += actual_size;
    return slice;
}

[[gnu::regcall, gnu::nonnull(1)]]
/** Reset the buffer to no data in use. */
static inline void string_buffer_reset(struct string_buffer *NONNULL buffer) {
    buffer->in_use = 0;
}

/**
 * A connection to the database file, which is a SQLite3 connection with cached statements.
 */
struct database_connection {
    /** The actual connection. */
    sqlite3 *NONNULL db;
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
    /** Internal buffer for string output. */
    struct string_buffer *NONNULL restrict text_buffer;
};

[[gnu::regcall, gnu::malloc, gnu::nonnull(1, 3, 4)]]
/** Build a SQLite statement for persistent use. Returns NULL on failure. */
static sqlite3_stmt *NULLABLE db_prepare(
    sqlite3 *NONNULL db,
    size_t len,
    const char sql[NONNULL restrict len],
    bool *NONNULL has_error,  // shared error flag, for creating multiple statements in series
    message *NULLABLE restrict errmsg
) {
    assert(len < INT_MAX);
    assert(len == strlen(sql));
    // skip prepare if an error already happened before
    if unlikely (*has_error) {
        return NULL;
    }

    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    constexpr int FLAGS = SQLITE_PREPARE_PERSISTENT | SQLITE_PREPARE_NO_VTAB;

    const int rv = sqlite3_prepare_v3(db, sql, ((int) len) + 1, FLAGS, &stmt, &tail);
    if unlikely (rv != SQLITE_OK) {
        *has_error = true;
        errmsg_dup_db(errmsg, db);
        assert(stmt == NULL);
        return NULL;
    }

    assert(tail == &sql[len]);
    return stmt;
}

[[gnu::regcall, gnu::nonnull(1)]]
/** Create all used statements beforehand, for faster reuse later. */
static bool db_prepare_stmts(db_conn *NONNULL conn, message *NULLABLE errmsg) {
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
    sqlite3_stmt *insert_movie =
        SQL(
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
    assert((name) != NULL); \
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
db_conn *NULLABLE db_connect(const char filepath[NONNULL restrict], message *NULLABLE restrict errmsg) {
    db_conn *conn = calloc(1, sizeof(struct database_connection));
    if unlikely (conn == NULL) {
        errmsg_dup_str(errmsg, OUT_OF_MEMORY_ERROR);
        return NULL;
    }

    struct string_buffer *text_buffer = string_buffer_alloc();
    if unlikely (text_buffer == NULL) {
        errmsg_dup_str(errmsg, OUT_OF_MEMORY_ERROR);
        free(conn);
        return NULL;
    }

    sqlite3 *db = db_open(filepath, errmsg, false);
    if unlikely (db == NULL) {
        // `db_open` already sets `errmsg`
        string_buffer_free(text_buffer);
        free(conn);
        return NULL;
    }

    conn->db = db;
    conn->text_buffer = text_buffer;
    const bool ok = db_prepare_stmts(conn, errmsg);
    if unlikely (!ok) {
        string_buffer_free(text_buffer);
        db_close(db, NULL);
        free(conn);
        return NULL;
    }

    // last verification that all pointers are non null
    for (size_t i = 0; i < sizeof(db_conn) / sizeof(void *); i++) {
        const void *const *start = (const void *const *) conn;
        assert(start[i] != NULL);
    }
    return conn;
}

[[gnu::regcall, gnu::nonnull(1, 2, 3)]]
/** Closes a prepared statement. Used during disconnect. */
static void db_finalize(sqlite3 *NONNULL db, sqlite3_stmt *NONNULL stmt, bool *NONNULL ok, message *NULLABLE errmsg) {
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
bool db_disconnect(db_conn *NONNULL conn, message *NULLABLE errmsg) {
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

    memset(conn, 0, sizeof(struct database_connection));
    free(conn);
    return ok;
}

[[gnu::regcall, gnu::const]]
/**
 * Translate SQLite3 extended error codes into simpler values.
 *
 * @param rv Return value of the last statement operation.
 * @param rrv Return value of the `sqlite3_reset()` call after that.
 */
static db_result check_result(const int rv, const int rrv) {
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

[[gnu::regcall, gnu::pure, gnu::nonnull(2)]]
/** Checks a list of return values sequentially. */
static db_result check_results(size_t n, const int rv[NONNULL const n], const int rrv) {
    for (size_t i = 0; i < n; i++) {
        db_result result = check_result(rv[i], rrv);
        if (result != DB_SUCCESS) {
            return result;
        }
    }
    return DB_SUCCESS;
}

[[gnu::regcall, gnu::nonnull(1, 2), gnu::hot]]
/** Runs a single transaction statement and reset it. */
static db_result db_transaction_op(db_conn *NONNULL conn, sqlite3_stmt *NONNULL stmt, message *NULLABLE errmsg) {
    int rv = sqlite3_step(stmt);
    int rrv = sqlite3_reset(stmt);
    if unlikely (rv != SQLITE_DONE || rrv != SQLITE_OK) {
        errmsg_dup_db(errmsg, conn->db);
        return check_result(rv, rrv);
    }
    return DB_SUCCESS;
}

[[gnu::regcall, gnu::nonnull(1), gnu::hot]]
/** Runs `BEGIN TRANSACTION`. */
static db_result db_transaction_begin(db_conn *NONNULL conn, message *NULLABLE errmsg) {
    return db_transaction_op(conn, conn->op_begin, errmsg);
}

[[gnu::regcall, gnu::nonnull(1)]]
/** Runs `ROLLBACK TRANSACTION`. */
static db_result db_transaction_rollback(db_conn *NONNULL conn, message *NULLABLE errmsg) {
    return db_transaction_op(conn, conn->op_rollback, errmsg);
}

[[gnu::regcall, gnu::nonnull(1), gnu::hot]]
/** Runs `COMMIT TRANSACTION`. */
static db_result db_transaction_commit(db_conn *NONNULL conn, message *NULLABLE errmsg) {
    return db_transaction_op(conn, conn->op_commit, errmsg);
}

[[gnu::regcall, gnu::nonnull(1), gnu::hot]]
/** Step through statement, ignoring results. */
static db_result db_eval_stmt(sqlite3_stmt *NONNULL stmt) {
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

[[gnu::regcall, gnu::pure, gnu::nonnull(1)]]
/** Calculate the size of a NULL terminated list of strings. */
static size_t list_len(const char *NULLABLE const list[NONNULL]) {
    if unlikely (list == NULL) {
        return 0;
    }

    size_t len = 0;
    while (list[len] != NULL) {
        len++;
    }
    return len;
}

[[gnu::regcall, gnu::nonnull(2)]]
/** Runs `op_insert_movie` inside an open transaction. */
static db_result register_movie_in_transaction(const db_conn conn, struct movie *NONNULL movie) {
    const size_t genres = list_len(movie->genres);

    // add all movie genres to db
    for (size_t i = 0; i < genres; i++) {
        int rv = sqlite3_bind_text(conn.op_insert_genre, 1, movie->genres[i], -1, SQLITE_STATIC);
        if unlikely (rv != SQLITE_OK) {
            sqlite3_clear_bindings(conn.op_insert_genre);
            return check_result(rv, sqlite3_reset(conn.op_insert_genre));
        }

        db_result res = db_eval_stmt(conn.op_insert_genre);
        if unlikely (res != DB_SUCCESS) {
            return res;
        }
    }

    // add movie itself to db
    int rvv[3] = {SQLITE_OK, SQLITE_OK, SQLITE_OK};
    rvv[0] = sqlite3_bind_text(conn.op_insert_movie, 1, movie->title, -1, SQLITE_STATIC);
    rvv[1] = sqlite3_bind_text(conn.op_insert_movie, 2, movie->director, -1, SQLITE_STATIC);
    rvv[2] = sqlite3_bind_int(conn.op_insert_movie, 3, movie->release_year);
    if unlikely (rvv[0] != SQLITE_OK || rvv[1] != SQLITE_OK || rvv[2] != SQLITE_OK) {
        sqlite3_clear_bindings(conn.op_insert_movie);
        return check_results(3, rvv, sqlite3_reset(conn.op_insert_movie));
    }

    int rv = SQLITE_OK;
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
        int rvv[2] = {SQLITE_OK, SQLITE_OK};
        rvv[0] = sqlite3_bind_int64(conn.op_insert_genre_link, 1, movie->id);
        rvv[1] = sqlite3_bind_text(conn.op_insert_genre_link, 2, movie->genres[i], -1, SQLITE_STATIC);
        if unlikely (rvv[0] != SQLITE_OK || rvv[1] != SQLITE_OK) {
            sqlite3_clear_bindings(conn.op_insert_genre_link);
            return check_results(2, rvv, sqlite3_reset(conn.op_insert_genre_link));
        }

        db_result res = db_eval_stmt(conn.op_insert_genre_link);
        if unlikely (res != DB_SUCCESS) {
            return res;
        }
    }

    return DB_SUCCESS;
}

/** Registers a new movie and updates its 'id' if successful. */
db_result db_register_movie(db_conn *NONNULL conn, struct movie *NONNULL movie, message *NULLABLE restrict errmsg) {
    assert(movie != NULL);
    assert(movie->id == 0);

    db_result res = db_transaction_begin(conn, errmsg);
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

[[gnu::regcall, gnu::nonnull(3)]]
/** Runs `op_insert_genre_link` inside an open transaction. */
static db_result add_genres_in_transaction(
    const db_conn conn,
    size_t len,
    const char *NONNULL const genres[NONNULL restrict len],
    int64_t movie_id
) {
    for (size_t i = 0; i < len; i++) {
        int rv = sqlite3_bind_text(conn.op_insert_genre, 1, genres[i], -1, SQLITE_STATIC);
        if unlikely (rv != SQLITE_OK) {
            sqlite3_clear_bindings(conn.op_insert_genre);
            return check_result(rv, sqlite3_reset(conn.op_insert_genre));
        }

        db_result res = db_eval_stmt(conn.op_insert_genre);
        if unlikely (res != DB_SUCCESS) {
            return res;
        }
    }

    for (size_t i = 0; i < len; i++) {
        int rv1 = sqlite3_bind_int64(conn.op_insert_genre_link, 1, movie_id);
        int rv2 = sqlite3_bind_text(conn.op_insert_genre_link, 2, genres[i], -1, SQLITE_STATIC);
        if unlikely (rv1 != SQLITE_OK || rv2 != SQLITE_OK) {
            sqlite3_clear_bindings(conn.op_insert_genre_link);
            return check_results(2, (int[2]) {rv1, rv2}, sqlite3_reset(conn.op_insert_genre_link));
        }

        db_result res = db_eval_stmt(conn.op_insert_genre_link);
        if unlikely (res != DB_SUCCESS) {
            return res;
        }
    }
    return DB_SUCCESS;
}

/** Adds a list of genres tp an existing movie. */
db_result db_add_genres(
    db_conn *NONNULL conn,
    int64_t movie_id,
    const char *NULLABLE const genres[NONNULL restrict],
    message *NULLABLE restrict errmsg
) {
    const size_t len = list_len(genres);
    if unlikely (len == 0) {
        errmsg_dup_str(errmsg, "empty list of genres to add, operation ignored");
        return DB_USER_ERROR;
    }

    db_result res = db_transaction_begin(conn, errmsg);
    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    res = add_genres_in_transaction(*conn, len, genres, movie_id);
    if likely (res == DB_SUCCESS) {
        return db_transaction_commit(conn, errmsg);
    }

    switch (sqlite3_extended_errcode(conn->db)) {
        case SQLITE_CONSTRAINT_FOREIGNKEY:
            errmsg_printf(errmsg, 128, "no movie with id = %" PRIi64 " found in the database", movie_id);
            break;
        case SQLITE_CONSTRAINT_UNIQUE:
            errmsg_printf(errmsg, 128, "movie with id = %" PRIi64 " already has the provided genre", movie_id);
            break;
        default: {
            errmsg_dup_db(errmsg, conn->db);
            break;
        }
    }
    db_transaction_rollback(conn, NULL);
    return res;
}

[[gnu::regcall]]
/** Runs `op_delete_movie` inside its automatic transaction. */
static db_result delete_movie_in_transaction(const db_conn conn, int64_t movie_id) {
    int rv = sqlite3_bind_int64(conn.op_delete_movie, 1, movie_id);
    if unlikely (rv != SQLITE_OK) {
        sqlite3_clear_bindings(conn.op_delete_movie);
        return check_result(rv, sqlite3_reset(conn.op_delete_movie));
    }

    return db_eval_stmt(conn.op_delete_movie);
}

/** Removes a movie from the database. */
db_result db_delete_movie(db_conn *NONNULL conn, int64_t movie_id, message *NULLABLE errmsg) {
    // no need to create an explicit transaction for a single statement
    const db_result res = delete_movie_in_transaction(*conn, movie_id);
    if unlikely (res != DB_SUCCESS) {
        errmsg_dup_db(errmsg, conn->db);
        return res;
    }

    if (sqlite3_changes64(conn->db) < 1) {
        errmsg_printf(errmsg, 128, "no movie with id = %" PRIi64 " to be deleted from the database", movie_id);
        return DB_USER_ERROR;
    }
    return DB_SUCCESS;
}

[[gnu::regcall, gnu::nonnull(1, 2)]]
/** Insert column text into `buffer` and return the slice position. */
static size_t get_column_in_slice(struct string_buffer *NONNULL buffer, sqlite3_stmt *NONNULL stmt, int column) {
    const unsigned char *data = sqlite3_column_text(stmt, column);
    if unlikely (data == NULL) {
        return SIZE_MAX;
    }

    const int bytes = sqlite3_column_bytes(stmt, column);
    const size_t size = unlikely(bytes < 0) ? strlen((const char *) data) : (size_t) bytes;

    const size_t slice = string_buffer_slice(buffer, size);
    if unlikely (slice == SIZE_MAX) {
        return SIZE_MAX;
    }

    memcpy(buffer->data + slice, data, size);
    buffer->data[slice + size] = '\0';

    return slice;
}

[[gnu::regcall, gnu::nonnull(1, 2, 3, 4, 5)]]
/** Build movie data into `buffer` and correct pointers to `movie`. */
static db_result get_movie_with_genres(
    struct string_buffer *NONNULL buffer,
    sqlite3_stmt *NONNULL outer_stmt,
    sqlite3_stmt *NONNULL inner_stmt,
    struct movie *NONNULL *NONNULL movie,
    size_t *NONNULL genre_count
) {
    string_buffer_reset(buffer);

    const int64_t id = sqlite3_column_int64(outer_stmt, 0);
    const int release_year = sqlite3_column_int(outer_stmt, 3);
    const size_t director = get_column_in_slice(buffer, outer_stmt, 2);
    const size_t title = get_column_in_slice(buffer, outer_stmt, 1);
    // ignore results on allocation issues
    if unlikely (title == SIZE_MAX || director == SIZE_MAX) {
        return DB_RUNTIME_ERROR;
    }

    int rv = sqlite3_bind_int64(inner_stmt, 1, id);
    if unlikely (rv != SQLITE_OK) {
        sqlite3_clear_bindings(inner_stmt);
        return check_result(rv, sqlite3_reset(inner_stmt));
    }

    const size_t genre_slice = buffer->in_use;
    size_t count = 0;
    while ((rv = sqlite3_step(inner_stmt)) == SQLITE_ROW) {
        const size_t genre = get_column_in_slice(buffer, inner_stmt, 0);
        if unlikely (genre == SIZE_MAX) {
            count = SIZE_MAX;
            break;
        }

        if likely (count <= SIZE_MAX - 1) {
            count += 1;
        }
    }

    sqlite3_clear_bindings(inner_stmt);
    int rrv = sqlite3_reset(inner_stmt);
    if unlikely ((rv != SQLITE_DONE && rv != SQLITE_ROW) || rrv != SQLITE_OK) {
        return check_result(rv, rrv);
    }

    constexpr size_t MAX_GENRES = (SIZE_MAX - sizeof(struct movie)) / sizeof(char *);
    if unlikely (count >= MAX_GENRES) {
        return DB_RUNTIME_ERROR;
    }

    if (*genre_count < count) {
        struct movie *m = realloc(*movie, sizeof(struct movie) + (count + 1) * sizeof(char *));
        if unlikely (m == NULL) {
            return DB_RUNTIME_ERROR;
        }

        *movie = m;
        *genre_count = count;
    }

    struct movie *m = *movie;
    m->id = id;
    m->title = buffer->data + title;
    m->director = buffer->data + director;
    m->release_year = release_year;

    const char *g = buffer->data + genre_slice;
    for (size_t i = 0; i < count; i++) {
        m->genres[i] = g;
        // move to the next string in buffer
        g += strlen(g) + 1;
    }
    m->genres[count] = NULL;

    return DB_SUCCESS;
}

[[gnu::regcall, gnu::nonnull(1, 2, 3, 4, 5)]]
/** Iterate over movie entries, calling `callback` on each and returning the final result in `movie`.  */
static db_result iter_movies(
    struct string_buffer *NONNULL buffer,
    sqlite3_stmt *NONNULL outer_stmt,
    sqlite3_stmt *NONNULL inner_stmt,
    struct movie *NONNULL *NONNULL movie,
    [[gnu::regcall]] bool callback(void *UNSPECIFIED data, const struct movie *NONNULL summary),
    void *NULLABLE callback_data
) {
    size_t genres = 0;
    struct movie *current_movie = malloc(sizeof(struct movie) + (genres + 1) * sizeof(char *));
    if unlikely (current_movie == NULL) {
        return DB_RUNTIME_ERROR;
    }

    int rv = SQLITE_OK;
    db_result res = DB_SUCCESS;
    while ((rv = sqlite3_step(outer_stmt)) == SQLITE_ROW) {
        res = get_movie_with_genres(buffer, outer_stmt, inner_stmt, &current_movie, &genres);
        if unlikely (res != DB_SUCCESS) {
            break;
        }

        bool stop = callback(callback_data, current_movie);
        if (stop) {
            break;
        }
    }

    sqlite3_clear_bindings(outer_stmt);
    int rrv = sqlite3_reset(outer_stmt);
    if unlikely ((rv != SQLITE_DONE && rv != SQLITE_ROW) || rrv != SQLITE_OK) {
        free(current_movie);
        return check_result(rv, rrv);
    } else if unlikely (res != DB_SUCCESS) {
        free(current_movie);
        return res;
    }

    *movie = current_movie;
    return DB_SUCCESS;
}

[[gnu::regcall, gnu::nonnull(1, 2)]]
/** Count each iteration. */
static bool count_iterations(void *NONNULL counter, [[maybe_unused]] const struct movie *NONNULL movie) {
    unsigned *cnt = (unsigned *) counter;
    (*cnt) += 1;
    return false;
}

[[gnu::regcall, gnu::nonnull(3)]]
/** Read a single movie and write to `movie`. */
static db_result get_movie_in_transaction(const db_conn conn, int64_t movie_id, struct movie *NONNULL *NONNULL movie) {
    int rv = sqlite3_bind_int64(conn.op_select_movie, 1, movie_id);
    if unlikely (rv != SQLITE_OK) {
        sqlite3_clear_bindings(conn.op_select_movie);
        return check_result(rv, sqlite3_reset(conn.op_select_movie));
    }

    unsigned count = 0;
    const db_result res = iter_movies(
        conn.text_buffer,
        conn.op_select_movie,
        conn.op_select_movie_genres,
        movie,
        count_iterations,
        &count
    );
    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    return (count < 1) ? DB_USER_ERROR : DB_SUCCESS;
}

/** Get a movie from the database. */
db_result db_get_movie(
    db_conn *NONNULL conn,
    int64_t movie_id,
    struct movie *NONNULL *NONNULL movie,
    message *NULLABLE restrict errmsg
) {
    db_result res = db_transaction_begin(conn, errmsg);
    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    res = get_movie_in_transaction(*conn, movie_id, movie);
    if likely (res == DB_SUCCESS) {
        res = db_transaction_commit(conn, errmsg);
        if unlikely (res != DB_SUCCESS) {
            free(*movie);
        }
        return res;
    }

    if (sqlite3_extended_errcode(conn->db) != SQLITE_OK) {
        errmsg_dup_db(errmsg, conn->db);
    } else {
        switch (res) {
            case DB_USER_ERROR:
                errmsg_printf(errmsg, 128, "no movie with id = %" PRIi64 " found in the database", movie_id);
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

[[gnu::regcall, gnu::nonnull(2)]]
/** Read all movies and run callback on each. */
static db_result list_movies_in_transaction(
    const db_conn conn,
    [[gnu::regcall]] bool callback(void *UNSPECIFIED data, const struct movie *NONNULL movie),
    void *NULLABLE callback_data
) {
    struct movie *last_movie = NULL;
    const db_result res = iter_movies(
        conn.text_buffer,
        conn.op_select_all_movies,
        conn.op_select_movie_genres,
        &last_movie,
        callback,
        callback_data
    );

    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    free(last_movie);
    return DB_SUCCESS;
}

/** List all movies with full information. */
db_result db_list_movies(
    db_conn *NONNULL conn,
    [[gnu::regcall]] bool callback(void *UNSPECIFIED data, const struct movie *NONNULL movie),
    void *NULLABLE callback_data,
    message *NULLABLE restrict errmsg
) {
    db_result res = db_transaction_begin(conn, errmsg);
    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    res = list_movies_in_transaction(*conn, callback, callback_data);
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

[[gnu::regcall, gnu::nonnull(2)]]
/** Search through movies and run callback on each. */
static db_result search_movies_in_transaction(
    const db_conn conn,
    const char genre[NONNULL restrict const],
    [[gnu::regcall]] bool callback(void *UNSPECIFIED data, const struct movie *NONNULL movie),
    void *NULLABLE callback_data
) {
    int rv = sqlite3_bind_text(conn.op_select_movies_genre, 1, genre, -1, SQLITE_STATIC);
    if unlikely (rv != SQLITE_OK) {
        sqlite3_clear_bindings(conn.op_select_movies_genre);
        return check_result(rv, sqlite3_reset(conn.op_select_movies_genre));
    }

    struct movie *last_movie = NULL;
    const db_result res = iter_movies(
        conn.text_buffer,
        conn.op_select_movies_genre,
        conn.op_select_movie_genres,
        &last_movie,
        callback,
        callback_data
    );

    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    free(last_movie);
    return DB_SUCCESS;
}

/* List all movies with a given genre. */
db_result db_search_movies_by_genre(
    db_conn *NONNULL conn,
    const char genre[NONNULL restrict const],
    [[gnu::regcall]] bool callback(void *UNSPECIFIED data, const struct movie *NONNULL movie),
    void *NULLABLE callback_data,
    message *NULLABLE restrict errmsg
) {
    db_result res = db_transaction_begin(conn, errmsg);
    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    res = search_movies_in_transaction(*conn, genre, callback, callback_data);
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

[[gnu::regcall, gnu::nonnull(1, 2, 3)]]
/** Build summary data using `buffer`. */
static db_result get_summary(
    struct string_buffer *NONNULL buffer,
    sqlite3_stmt *NONNULL stmt,
    struct movie_summary *NONNULL summary
) {
    string_buffer_reset(buffer);

    const int64_t id = sqlite3_column_int64(stmt, 0);
    const size_t title = get_column_in_slice(buffer, stmt, 1);
    // ignore results on allocation issues
    if unlikely (title == SIZE_MAX) {
        return DB_RUNTIME_ERROR;
    }

    summary->id = id;
    summary->title = buffer->data + title;
    return DB_SUCCESS;
}

[[gnu::regcall, gnu::nonnull(2)]]
/** Read title and id of all movies and run callback on each. */
static db_result list_summaries_in_transaction(
    const db_conn conn,
    [[gnu::regcall]] bool callback(void *UNSPECIFIED data, struct movie_summary summary),
    void *NULLABLE callback_data
) {

    int rv = SQLITE_OK;
    db_result res = DB_SUCCESS;
    while ((rv = sqlite3_step(conn.op_select_all_titles)) == SQLITE_ROW) {
        struct movie_summary summary = {.id = -1, .title = ""};
        res = get_summary(conn.text_buffer, conn.op_select_all_titles, &summary);
        if unlikely (res != DB_SUCCESS) {
            break;
        }

        bool stop = callback(callback_data, summary);
        if (stop) {
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

    return DB_SUCCESS;
}

/** List all movies with reduced information. */
db_result db_list_summaries(
    db_conn *NONNULL conn,
    [[gnu::regcall]] bool callback(void *UNSPECIFIED data, struct movie_summary summary),
    void *NULLABLE callback_data,
    message *NULLABLE restrict errmsg
) {
    db_result res = db_transaction_begin(conn, errmsg);
    if unlikely (res != DB_SUCCESS) {
        return res;
    }

    res = list_summaries_in_transaction(*conn, callback, callback_data);
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
