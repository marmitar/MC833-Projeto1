/** Database operations. */
#include "./sqlite_source.h"  // must be included first for defines
#define database_connection sqlite3
#include "./database.h"
#include "./schema.h"

#include "defines.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char UNKNOWN_ERROR[] = "unknown error";
static const char OUT_OF_MEMORY_ERROR[] = "out of memory";

[[gnu::cold, gnu::returns_nonnull, nodiscard]]
/**
 * Duplicates an error message string if not NULL, returning a default message when the original is NULL or
 * allocation fails.
 */
static const char *NONNULL errmsg_dup(const char *NULLABLE error_message) {
    if unlikely (error_message == NULL) {
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
    if (errmsg != NULL) {
        *errmsg = errmsg_dup(str);
    }
}

[[gnu::cold]]
/** Copies the current SQLite error message from `db` into `errmsg`, if non-NULL. */
static void errmsg_dup_db(const char *NONNULL errmsg[NULLABLE 1], db_conn *NULLABLE db) {
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
    if unlikely (errmsg == UNKNOWN_ERROR || errmsg == OUT_OF_MEMORY_ERROR) {
        return;
    }

    sqlite3_free((char *) errmsg);
}

[[gnu::regcall, gnu::malloc, gnu::nonnull(1)]]
/**
 * Open a database at `filepath`, either connecting to an existing database or creating a new one whe `create` is true.
 */
db_conn *NULLABLE db_open(const char filepath[NONNULL], const char *NONNULL errmsg[NULLABLE 1], bool create) {
    const int FLAGS = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE | SQLITE_OPEN_EXRESCODE;

    db_conn *db = NULL;
    int rv = sqlite3_open_v2(filepath, &db, create ? FLAGS | SQLITE_OPEN_CREATE : FLAGS, NULL);
    if unlikely (rv != SQLITE_OK) {
        errmsg_dup_db(errmsg, db);
        db_close(db, NULL);
        return NULL;
    }

    return db;
}

/** Connects to the existing database at `filepath`. */
db_conn *NULLABLE db_connect(const char filepath[NONNULL], const char *NONNULL errmsg[NULLABLE 1]) {
    return db_open(filepath, errmsg, false);
}

/** Closes an open database connection. */
bool db_close(db_conn *NONNULL db, const char *NONNULL errmsg[NULLABLE 1]) {
    const int rv = sqlite3_close_v2(db);
    if unlikely (rv != SQLITE_OK) {
        errmsg_dup_rc(errmsg, rv);
        return false;
    }
    return true;
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
static bool db_create_schema(db_conn *NONNULL db, const char *NONNULL errmsg[NULLABLE 1]) {
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
        errmsg_dup_str(errmsg, "could not call at_exit");
        db_shutdown();
        return false;
    }

    db_conn *db = db_open(filepath, errmsg, true);
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
