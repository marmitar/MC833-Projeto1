#include "database/database.h"

#include "defines.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

extern int main(void) {
    const char *error = NULL;
    bool ok = db_setup("movies.db", &error);
    if unlikely (!ok) {
        fprintf(stderr, "Failed to setup database: %s\n", error);
        db_free_errmsg(error);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
