/** Database operations. */
#include "./sqlite_source.h"  // must be included first for defines
#define database_connection sqlite3
#include "./database.h"

#include "defines.h"
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
/** Copies the current SQLite error message from `db` into `errmsg`, if non-NULL. */
static void errmsg_dup_db(db_conn *NULLABLE db, const char *NONNULL errmsg[NULLABLE 1]) {
    if (errmsg != NULL) {
        *errmsg = errmsg_dup(sqlite3_errmsg(db));
    }
}

[[gnu::cold]]
/** Copies the SQLite error string for the error code `rc` into `errmsg`, if non-NULL. */
static void errmsg_dup_rc(const int rc, const char *NONNULL errmsg[NULLABLE 1]) {
    if (errmsg != NULL) {
        *errmsg = errmsg_dup(sqlite3_errstr(rc));
    }
}

/** Frees a dynamically allocated error message string. */
void db_free_errmsg(const char *NONNULL errmsg) {
    // this string should have been "allocated" with `errmsg_dup`, which might return these static strings
    if unlikely (errmsg == UNKNOWN_ERROR || errmsg == OUT_OF_MEMORY_ERROR) {
        return;
    }

    sqlite3_free((char *) errmsg);
}

/** Connects to the database at `filepath`. */
db_conn *NULLABLE db_connect(const char filepath[NONNULL], const char *NONNULL errmsg[NULLABLE 1]) {
    int rv = sqlite3_initialize();
    if unlikely (rv != SQLITE_OK) {
        errmsg_dup_rc(rv, errmsg);
        return NULL;
    }

    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE
        | SQLITE_OPEN_EXRESCODE;
    db_conn *db = NULL;

    rv = sqlite3_open_v2(filepath, &db, flags, NULL);
    if unlikely (rv != SQLITE_OK) {
        errmsg_dup_db(db, errmsg);
        db_close(db, NULL);
        return NULL;
    }

    return db;
}

/** Closes an open database connection. */
void db_close(db_conn *NONNULL db, const char *NONNULL errmsg[NULLABLE 1]) {
    const int rv = sqlite3_close_v2(db);
    if unlikely (rv != SQLITE_OK) {
        errmsg_dup_rc(rv, errmsg);
    }
}
