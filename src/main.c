#include "database/database.h"

#include "defines.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

extern int main(void) {
    const char *error = NULL;

    db_conn *db = db_connect("movies.db", &error);
    if unlikely (db == NULL) {
        fprintf(stderr, "Failed to open database: %s\n", error);
        db_free_errmsg(error);
        return EXIT_FAILURE;
    }

    db_close(db, NULL);
    return EXIT_SUCCESS;
}
