#ifndef SRC_WORKER_PARSER_H
/** Database operation parser. */
#define SRC_WORKER_PARSER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../defines.h"
#include "./movie.h"

/**
 * Enum for high-level operations that can be parsed from YAML.
 */
enum [[gnu::packed]] operation_ty {
    PARSE_ERROR = -1,
    PARSE_DONE = 0,
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
struct operation {  // NOLINT(altera-struct-pack-align)
    union {
        /**
         * If `ty` is something that requires a full movie definition (e.g. ADD_MOVIE), this is a pointer to a
         * newly allocated `struct movie`.
         */
        struct movie movie;

        /**
         * If `ty` is an operation that just needs an ID and/or a genre, store them here in `movie_id` and `genre`.
         */
        struct movie_key {  // NOLINT(altera-struct-pack-align)
            int64_t movie_id;
            const char *NULLABLE genre;
        } key;

        /**
         * Error message for
         */
        const char *NULLABLE error_message;
    };
    enum operation_ty ty;
};

/** Optimal alignment for `parser_t`. */
#define ALIGNMENT_OPERATION_PARSER 128

/** Opaque parser type. */
typedef struct operation_parser parser_t [[gnu::aligned(ALIGNMENT_OPERATION_PARSER)]];

[[nodiscard("must be freed"), gnu::assume_aligned(ALIGNMENT_OPERATION_PARSER), gnu::malloc, gnu::leaf, gnu::nothrow]]
/**
 * Initializes a YAML parser to read from the file descriptor `sock_fd`.
 *
 * @return the parser on success, or `NULL` on allocation failures.
 */
parser_t *NULLABLE parser_create(atomic_bool *NONNULL shutdown_requested, int sock_fd);

[[gnu::pure, gnu::nonnull(1), gnu::leaf, gnu::nothrow]]
/**
 * Check if input stream already ended.
 */
bool parser_finished(const parser_t *NONNULL parser);

[[gnu::nonnull(1), gnu::leaf, gnu::nothrow]]
/**
 * Free memory used by the YAML parser.
 */
void parser_destroy(parser_t *NONNULL parser);

[[nodiscard("must be freed"), gnu::nonnull(1), gnu::hot, gnu::leaf, gnu::nothrow]]
/**
 * Reads the next operation from the YAML parser, which may be outside or inside a mapping.
 *
 * If we are inside a mapping, we expect a key that identifies the operation type, and then we parse its parameters.
 * If we are outside, we only allow certain operation types (e.g., list).
 *
 * @return An operation describing what to do (with any associated data), or PARSE_ERROR on error.
 */
struct operation parser_next_op(parser_t *NONNULL parser);

#endif  // SRC_WORKER_PARSER_H
