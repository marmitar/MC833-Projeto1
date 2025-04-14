/** Database operations. */
#include "./sqlite_source.h"  // must be included first for defines

#include "defines.h"
#include <assert.h>
#include <limits.h>
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
static inline bool is_predefined_errmsg(const char *NULLABLE error_message) {
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
    char *copy = calloc(len + 1, sizeof(char));
    if unlikely (copy == NULL) {
        return OUT_OF_MEMORY_ERROR;
    }

    memcpy(copy, error_message, len * sizeof(char));
    copy[len] = '\0';  // just to be sure
    return copy;
}

[[gnu::cold]]
/** Copies the current SQLite error message from `str` into `errmsg`, if non-NULL. */
static void errmsg_dup_str(const char *NONNULL errmsg[NULLABLE 1], const char str[restrict NONNULL]) {
    if likely (errmsg != NULL) {
        *errmsg = errmsg_dup(str);
    }
}

[[gnu::cold]]
/** Copies the current SQLite error message from `db` into `errmsg`, if non-NULL. */
static void errmsg_dup_db(const char *NONNULL errmsg[NULLABLE 1], sqlite3 *NULLABLE db) {
    errmsg_dup_str(errmsg, sqlite3_errmsg(db));
}

[[gnu::cold]]
/** Copies the SQLite error string for the error code `rc` into `errmsg`, if non-NULL. */
static void errmsg_dup_rc(const char *NONNULL errmsg[NULLABLE 1], const int rc) {
    errmsg_dup_str(errmsg, sqlite3_errstr(rc));
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
static bool db_close(sqlite3 *NULLABLE db, const char *NONNULL errmsg[NULLABLE 1]) {
    const int rv = sqlite3_close(db);  // safe to call with NULL
    if likely (rv == SQLITE_OK) {
        return true;
    }

    errmsg_dup_rc(errmsg, rv);
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
    NULLABLE db_open(const char filepath[restrict NONNULL], const char *NONNULL errmsg[NULLABLE 1], bool create) {
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
static bool db_create_schema(sqlite3 *NONNULL db, const char *NONNULL errmsg[NULLABLE 1]) {
    char *errorbuf = NULL;  // will be allocated via sqlite3_malloc, need to copied to std malloc

    int rv = sqlite3_exec(db, SCHEMA, nullptr, nullptr, (errmsg != NULL) ? &errorbuf : NULL);
    if unlikely (rv != SQLITE_OK) {
        errmsg_dup_str(errmsg, errorbuf);
        sqlite3_free(errorbuf);  // safe to call with NULL
        return false;
    }

    return true;
}

/** Create or migrate database at `filepath`. */
bool db_setup(const char filepath[restrict NONNULL], const char *NONNULL errmsg[NULLABLE 1]) {
    int rv = sqlite3_initialize();
    if unlikely (rv != SQLITE_OK) {
        errmsg_dup_rc(errmsg, rv);
        return false;
    }

    rv = atexit(db_shutdown);
    if unlikely (rv != OK) {
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
struct database_connection {
    /** The actual connection. */
    sqlite3 *NONNULL db;
    /** BEGIN TRANSACTION. */
    sqlite3_stmt *NONNULL op_begin;
    /** COMMIT (or END) TRANSACTION. */
    sqlite3_stmt *NONNULL op_commit;
    /** ROLLBACK TRANSACTION. */
    sqlite3_stmt *NONNULL op_rollback;
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
};

[[gnu::regcall, gnu::malloc, gnu::nonnull(1, 3, 4)]]
/** Build a SQLite statement for persistent use. Returns NULL on failure. */
static sqlite3_stmt *NULLABLE db_prepare(
    sqlite3 *NONNULL db,
    size_t len,
    const char sql[restrict NONNULL const len],
    bool *NONNULL const has_error,  // shared error flag, for creating multiple statements in series
    const char *NULLABLE errmsg[NULLABLE 1]
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
        errmsg_dup_rc(errmsg, rv);
        assert(stmt == NULL);
        return NULL;
    }

    assert(tail == &sql[len]);
    return stmt;
}

[[gnu::regcall, gnu::nonnull(1)]]
/** Create all used statements beforehand, for faster reuse later. */
static bool db_prepare_stmts(db_conn *NONNULL conn, const char *NONNULL errmsg[NULLABLE 1]) {
    sqlite3 *NONNULL db = conn->db;
    bool has_error = false;

/** Basic SQL macro: converts arguments into text and passes that to `db_prepare`. */
#define SQL(...)   SQL_(STR(__VA_ARGS__))
#define SQL_(stmt) db_prepare(db, strlen(stmt), stmt, &has_error, errmsg)
    sqlite3_stmt *begin = SQL(
        BEGIN TRANSACTION;
    );
    sqlite3_stmt *commit = SQL(
        COMMIT TRANSACTION;
    );
    sqlite3_stmt *rollback = SQL(
        ROLLBACK TRANSACTION;
    );
    sqlite3_stmt *insert_movie =
        SQL(
        INSERT OR ROLLBACK INTO movie(title, director, release_year)
            VALUES (:title, :director, :release_year)
            RETURNING id;
    );
    sqlite3_stmt *insert_genre = SQL(
        INSERT OR IGNORE INTO genre(name)
            VALUES (:genre);
    );
    sqlite3_stmt *insert_genre_link =
        SQL(
        INSERT OR ROLLBACK INTO movie_genre(movie_id, genre_id)
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
        SELECT *
            FROM movie;
    );
    sqlite3_stmt *select_movie = SQL(
        SELECT *
            FROM movie
            WHERE id = :movie;
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
        sqlite3_finalize(insert_movie);
        sqlite3_finalize(insert_genre);
        sqlite3_finalize(insert_genre_link);
        sqlite3_finalize(delete_movie);
        sqlite3_finalize(delete_unused_genres);
        sqlite3_finalize(select_all_titles);
        sqlite3_finalize(select_all_movies);
        sqlite3_finalize(select_movie);
        sqlite3_finalize(select_movie_genres);
        return false;
    }

#define set_stmt(name)      \
    assert((name) != NULL); \
    conn->op_##name = (name)

    set_stmt(begin);
    set_stmt(commit);
    set_stmt(rollback);
    set_stmt(insert_movie);
    set_stmt(insert_genre);
    set_stmt(insert_genre_link);
    set_stmt(delete_movie);
    set_stmt(delete_unused_genres);
    set_stmt(select_all_titles);
    set_stmt(select_all_movies);
    set_stmt(select_movie);
    set_stmt(select_movie_genres);
#undef set_stmt

    return true;
}

/** Connects to the existing database at `filepath`. */
db_conn *NULLABLE db_connect(const char filepath[restrict NONNULL], const char *NONNULL errmsg[NULLABLE 1]) {
    db_conn *conn = calloc(1, sizeof(struct database_connection));
    if unlikely (conn == NULL) {
        errmsg_dup_str(errmsg, OUT_OF_MEMORY_ERROR);
        return NULL;
    }

    sqlite3 *db = db_open(filepath, errmsg, false);
    if unlikely (db == NULL) {
        // `db_open` already sets `errmsg`
        free(conn);
        return NULL;
    }

    conn->db = db;
    const bool ok = db_prepare_stmts(conn, errmsg);
    if unlikely (!ok) {
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

[[gnu::regcall, gnu::nonnull(1, 2)]]
static void db_finalize(sqlite3_stmt *NONNULL stmt, bool *NONNULL ok, const char *NONNULL errmsg[NULLABLE 1]) {
    int rv = sqlite3_finalize(stmt);
    if unlikely (rv != SQLITE_OK) {
        // set errmsg on the first error only
        if (*ok) {
            errmsg_dup_rc(errmsg, rv);
        }
        *ok = false;
    }
}

/** Disconnects to the database at `filepath` and free resources. */
bool db_disconnect(db_conn *NONNULL conn, const char *NONNULL errmsg[NULLABLE 1]) {
    bool ok = true;
    db_finalize(conn->op_begin, &ok, errmsg);
    db_finalize(conn->op_commit, &ok, errmsg);
    db_finalize(conn->op_rollback, &ok, errmsg);
    db_finalize(conn->op_insert_movie, &ok, errmsg);
    db_finalize(conn->op_insert_genre, &ok, errmsg);
    db_finalize(conn->op_insert_genre_link, &ok, errmsg);
    db_finalize(conn->op_delete_movie, &ok, errmsg);
    db_finalize(conn->op_delete_unused_genres, &ok, errmsg);
    db_finalize(conn->op_select_all_titles, &ok, errmsg);
    db_finalize(conn->op_select_all_movies, &ok, errmsg);
    db_finalize(conn->op_select_movie, &ok, errmsg);
    db_finalize(conn->op_select_movie_genres, &ok, errmsg);
    ok = db_close(conn->db, ok ? errmsg : NULL) && ok;

    memset(conn, 0, sizeof(struct database_connection));
    free(conn);
    return ok;
}
