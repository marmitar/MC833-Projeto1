#include "database/database.h"

#include "defines.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

[[gnu::nonnull(1, 2), gnu::cold]]
static void print_error(const char operation[NONNULL const], const char *NONNULL error) {
#define BOLD_RED "\033[1;31m"
#define RESET    "\033[0m"
    (void) fprintf(stderr, BOLD_RED "Failed to %s:" RESET " %s\n", operation, error);
    db_free_errmsg(error);
#undef BOLD_RED
#undef RESET
}

[[gnu::nonnull(1), gnu::hot]]
static void run(db_conn *NONNULL conn) {
    (void) conn;
    // TODO
}

extern int main(void) {
    const char *error = NULL;
    bool ok = db_setup("movies.db", &error);
    if unlikely (!ok) {
        print_error("setup database", error);
        return EXIT_FAILURE;
    }

    db_conn *conn = db_connect("movies.db", &error);
    if unlikely (conn == NULL) {
        print_error("connect to database", error);
        return EXIT_FAILURE;
    }

    run(conn);

    ok = db_disconnect(conn, &error);
    if unlikely (!ok) {
        print_error("disconnect to database", error);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
