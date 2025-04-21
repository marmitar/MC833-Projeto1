#ifndef SRC_WORKER_PARSER_H
/** Database operation parser. */
#define SRC_WORKER_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yaml.h>

#include "../defines.h"
#include "./movie.h"

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

/** Optimal alignment for `struct operation`. */
#define ALIGNMENT_OPERATION_STRUCT 32

/**
 * A parsed operation that either points to a `struct movie` or a movie/genre key.
 */
struct operation {  // NOLINT(altera-struct-pack-align)
    union {
        /**
         * If `ty` is something that requires a full movie definition (e.g. ADD_MOVIE), this is a pointer to a
         * newly allocated `struct movie`.
         */
        struct movie *NONNULL movie;

        /**
         * If `ty` is an operation that just needs an ID and/or a genre, store them here in `movie_id` and `genre`.
         */
        struct movie_key {  // NOLINT(altera-struct-pack-align)
            int64_t movie_id;
            char *NULLABLE genre;
        } key;
    };
    enum operation_ty ty;
};

[[nodiscard("uninitialized parser if false"), gnu::nonnull(1), gnu::leaf, gnu::nothrow]]
/**
 * Initializes a YAML parser to read from the file descriptor `fd`.
 *
 * @return true on success, false otherwise.
 */
bool parser_start(yaml_parser_t *NONNULL parser, int sock_fd);

[[nodiscard("allocated memory must be freed"), gnu::nonnull(1, 2), gnu::hot, gnu::leaf, gnu::nothrow]]
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

#endif  // SRC_WORKER_PARSER_H
