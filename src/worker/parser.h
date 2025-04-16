#ifndef SRC_NET_PARSER_H
/** Database operation parser. */
#define SRC_NET_PARSER_H

#include <stdint.h>
#include <yaml.h>

#include "../database/database.h"
#include "../defines.h"

/**
 * Enum for high-level operations that can be parsed from YAML.
 */
enum [[gnu::packed]] operation_ty {
    INVALID_OP = 0,
    ADD_MOVIE = 1,
    ADD_GENRE = 2,
    REMOVE_MOVIE = 3,
    LIST_SUMMARIES = 4,
    LIST_MOVIES = 5,
    GET_MOVIE = 6,
    SEARCH_BY_GENRE = 7,
};

/**
 * A parsed operation that either points to a `struct movie` or a movie/genre key.
 */
struct operation {
    enum operation_ty ty;
    union {
        /**
         * If `ty` is something that requires a full movie definition (e.g. ADD_MOVIE), this is a pointer to a
         * newly allocated `struct movie`.
         */
        struct movie *NONNULL movie;

        /**
         * If `ty` is an operation that just needs an ID and/or a genre, store them here in `movie_id` and `genre`.
         */
        struct movie_key {
            int64_t movie_id;
            char *NULLABLE genre;
        } key;
    };
};

[[gnu::regcall, gnu::nonnull(1), gnu::leaf, gnu::nothrow]]
/**
 * Initializes a YAML parser to read from the file descriptor `fd`.
 *
 * @return true on success, false otherwise.
 */
bool parser_start(yaml_parser_t *NONNULL parser, int sock_fd);

[[gnu::regcall, gnu::nonnull(1, 2), gnu::leaf, gnu::nothrow]]
/**
 * Reads the next operation from the YAML parser, which may be outside or inside a mapping.
 *
 * If we are inside a mapping, we expect a key that identifies the operation type, and then we parse its parameters.
 * If we are outside, we only allow certain operation types (e.g., list).
 *
 * @param parser        The YAML parser to read from.
 * @param was_in_mapping Pointer to a boolean that indicates if we're currently in a mapping context.
 *
 * @return An operation describing what to do (with any associated data), or INVALID_OP on error.
 */
struct operation parser_next_op(yaml_parser_t *NONNULL parser, bool *NONNULL in_mapping);

#endif  // SRC_NET_PARSER_H
