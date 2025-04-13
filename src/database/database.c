/** Database operations. */
#include "./sqlite_source.h"  // must be included first for defines

#include "defines.h"
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

    const size_t len = strlen(error_message) + 1;
    char *copy = sqlite3_malloc64(len * sizeof(char));
    if unlikely (copy == NULL) {
        return OUT_OF_MEMORY_ERROR;
    }

    memcpy(copy, error_message, len * sizeof(char));
    return copy;
}

[[gnu::cold]]
/** Copies the current SQLite error message from `str` into `errmsg`, if non-NULL. */
static void errmsg_dup_str(const char *NONNULL errmsg[NULLABLE 1], const char str[NONNULL]) {
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
        sqlite3_free((char *) errmsg);
    }
}

[[gnu::regcall, gnu::nonnull(1)]]
/**
 * Closes an open SQLite3 database connection, free resources and set `errmsg`, if necessary.
 */
static bool db_close(sqlite3 *NONNULL db, const char *NONNULL errmsg[NULLABLE 1]) {
    const int rv = sqlite3_close_v2(db);
    if unlikely (rv != SQLITE_OK) {
        errmsg_dup_rc(errmsg, rv);
        return false;
    }
    return true;
}

[[gnu::regcall, gnu::malloc, gnu::nonnull(1)]]
/**
 * Open a database at `filepath`, either connecting to an existing database or creating a new one whe `create` is true.
 */
static sqlite3 *NULLABLE db_open(const char filepath[NONNULL], const char *NONNULL errmsg[NULLABLE 1], bool create) {
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
    int rv = sqlite3_exec(db, SCHEMA, nullptr, nullptr, (char **) errmsg);
    return likely(rv == SQLITE_OK);
}

/** Create or migrate database at `filepath`. */
bool db_setup(const char filepath[NONNULL], const char *NONNULL errmsg[NULLABLE 1]) {
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
};

/** Connects to the existing database at `filepath`. */
db_conn *NULLABLE db_connect(const char filepath[NONNULL], const char *NONNULL errmsg[NULLABLE 1]) {
    db_conn *conn = sqlite3_malloc64(sizeof(struct database_connection));
    if unlikely (conn == NULL) {
        errmsg_dup_str(errmsg, OUT_OF_MEMORY_ERROR);
        return NULL;
    }

    conn->db = db_open(filepath, errmsg, false);
    if unlikely (conn->db == NULL) {
        // `db_open` already sets `errmsg`
        sqlite3_free(conn);
        return NULL;
    }

    return conn;
}

/** Disconnects to the database at `filepath` and free resources. */
bool db_disconnect(db_conn *NONNULL conn, const char *NONNULL errmsg[NULLABLE 1]) {
    bool ok = db_close(conn->db, errmsg);

    memset(conn, 0, sizeof(struct database_connection));
    sqlite3_free(conn);
    return ok;
}
