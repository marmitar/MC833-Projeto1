#ifndef SRC_PARSER_H
/** Database operation parser. */
#define SRC_PARSER_H

#include "../defines.h"
#include <stdint.h>
#include <yaml.h>

#include "../database/database.h"

enum operation_ty {
    INVALID_OP = 0,
    ADD_MOVIE = 1,
    ADD_GENRE = 2,
    REMOVE_MOVIE = 3,
    LIST_SUMMARIES = 4,
    LIST_MOVIES = 5,
    GET_MOVIE = 6,
    SEARCH_BY_GENRE = 7,
};

struct operation {
    enum operation_ty ty;
    union {
        struct movie *NONNULL movie;
        struct movie_key {
            int64_t movie_id;
            char *NULLABLE genre;
        } key;
    };
};

bool parser_start(yaml_parser_t *NONNULL parser, int fd);

struct operation parser_next_op(yaml_parser_t *NONNULL parser, bool *NONNULL in_mapping);

#endif // SRC_PARSER_H
