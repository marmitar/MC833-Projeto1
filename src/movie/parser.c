#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>

#include <yaml.h>

#include "../alloc.h"
#include "../defines.h"
#include "./builder.h"
#include "./movie.h"
#include "./parser.h"

/**
 * YAML parser, with additional information.
 */
struct [[gnu::aligned(ALIGNMENT_OPERATION_PARSER)]] operation_parser {
    /** The libyaml parser. */
    yaml_parser_t yaml;
    /** Indicates that the input data is done. */
    bool done;
    /** External parsing position information. */
    bool in_mapping;
    /** The client socket. */
    int socket;
    /** Reusable movie builder. */
    movie_builder_t *builder;
    /** Internal buffer for error messages. */
    char *NULLABLE error_message;
    /** Allocated size for `error_message`. */
    size_t error_message_capacity;
};

[[gnu::nonnull(1, 2, 4), gnu::hot]]
/**
 * A read handler for libyaml that reads from a file descriptor.
 */
static int sock_read_handler(
    void *NONNULL data,
    unsigned char *NONNULL buffer,
    size_t size,
    size_t *NONNULL size_read
) {
    parser_t *NONNULL parser = aligned_like(struct operation_parser, data);

    ssize_t rv = recv(parser->socket, buffer, size, 0);
    /* Normal data. */
    if likely (rv >= 0) {
        *size_read = (size_t) rv;
        return 1;
        /* Error */
    } else {
        *size_read = 0;
        return 0;
    }
}

/** Initializes YAML parser.  */
parser_t *NULLABLE parser_create(const int sock_fd) {
    movie_builder_t *builder = movie_builder_create();
    if unlikely (builder == NULL) {
        return NULL;
    }

    parser_t *parser = alloc_like(struct operation_parser);
    if unlikely (parser == NULL) {
        movie_builder_destroy(builder);
        return NULL;
    }

    int rv = yaml_parser_initialize(&(parser->yaml));
    if unlikely (rv == 0) {
        free(parser);
        movie_builder_destroy(builder);
        return NULL;
    }

    parser->done = false;
    parser->in_mapping = false;
    parser->builder = builder;
    parser->error_message = NULL;
    parser->error_message_capacity = 0;

    parser->socket = sock_fd;
    yaml_parser_set_encoding(&(parser->yaml), YAML_UTF8_ENCODING);
    yaml_parser_set_input(&(parser->yaml), sock_read_handler, parser);

    return parser;
}

/** Free YAML parser resources. */
void parser_destroy(parser_t *NONNULL parser) {
    yaml_parser_delete(&(parser->yaml));
    movie_builder_destroy(parser->builder);
    if (parser->error_message == NULL) {
        free(parser->error_message);
    }
    free(parser);
}

/** Check if input stream already ended. */
bool parser_finished(const parser_t *NONNULL parser) {
    return unlikely(parser->done);
}

[[gnu::hot, gnu::nonnull(1)]]
/**
 * Input stream was finished successfully.
 */
static inline struct operation parse_done(parser_t *parser) {
    parser->done = true;
    return (struct operation) {.ty = PARSE_DONE};
}

/** Size increase in the error messages. */
#define ERROR_MESSAGE_STEP 1024

static_assert(is_power_of_two((unsigned) ERROR_MESSAGE_STEP));

[[gnu::const]]
/** Calculates $ceil(a / b)$. */
static inline size_t ceil_div(size_t a, size_t b) {
    assume(a != 0);
    assume(b != 0);
    // only works for positive integers
    return 1 + ((a - 1) / b);
}

[[gnu::nonnull(1)]]
/** Realloc the error message to hold at least `request_size + 1` characters. */
static bool error_message_realloc(parser_t *NONNULL parser, size_t request_size) {
    assume(request_size < SIZE_MAX);
    assume(request_size >= parser->error_message_capacity);

    size_t final_capacity = ceil_div(request_size + 1, ERROR_MESSAGE_STEP) * ERROR_MESSAGE_STEP;
    char *error_message = realloc(parser->error_message, final_capacity * sizeof(char));
    if unlikely (error_message == NULL) {
        return false;
    }

    parser->error_message = error_message;
    parser->error_message_capacity = final_capacity;
    return true;
}

[[gnu::format(printf, 2, 3), gnu::nonnull(1, 2)]]
/** Builds the error message in the reusable buffer. */
static const char *NULLABLE error_message_printf(parser_t *NONNULL parser, const char *NONNULL restrict format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);

    int rv = vsnprintf(parser->error_message, parser->error_message_capacity, format, args);
    va_end(args);

    if unlikely (rv < 0) {
        va_end(copy);
        return NULL;
    }

    const size_t bytes = (size_t) rv;
    if likely (bytes < parser->error_message_capacity) {
        va_end(copy);
        return parser->error_message;
    }

    bool ok = error_message_realloc(parser, bytes);
    if unlikely (!ok) {
        va_end(copy);
        return NULL;
    }

    rv = vsnprintf(parser->error_message, parser->error_message_capacity, format, copy);
    va_end(copy);

    if unlikely (rv < 0) {
        return NULL;
    }

    assume(((size_t) rv) < parser->error_message_capacity);
    return parser->error_message;
}

[[gnu::cold, gnu::nonnull(1)]]
/**
 * Returns an operation with ty=PARSE_ERROR, for libyaml errors.
 */
static struct operation parse_fail(parser_t *NONNULL parser) {
    assume(parser->yaml.problem != NULL);

    const char *error_message;
    if (parser->yaml.context != NULL) {
        error_message = error_message_printf(
            parser,
            "%s at %zu:%zu, %s at %zu:%zu",
            parser->yaml.problem,
            parser->yaml.problem_mark.line,
            parser->yaml.problem_mark.column,
            parser->yaml.context,
            parser->yaml.context_mark.line,
            parser->yaml.context_mark.column
        );
    } else {
        error_message = error_message_printf(
            parser,
            "%s at %zu:%zu",
            parser->yaml.problem,
            parser->yaml.problem_mark.line,
            parser->yaml.problem_mark.column
        );
    }

    // allocation failure, just send the raw message
    if unlikely (error_message == NULL) {
        error_message = parser->yaml.problem;
    }
    // reset the error message for reuse later
    parser->yaml.problem = NULL;
    parser->yaml.context = NULL;

    return (struct operation) {.ty = PARSE_ERROR, .error_message = error_message};
}

[[gnu::hot, gnu::nonnull(1, 3)]]
/**
 * Returns an operation with ty=PARSE_ERROR, suitable for our custom errors.
 */
static inline struct operation parse_invalid(
    parser_t *NONNULL parser,
    yaml_mark_t position,
    const char *NONNULL message
) {
    const char *error_message = error_message_printf(parser, "%s at %zu:%zu", message, position.line, position.column);
    if unlikely (error_message == NULL) {
        error_message = message;
    }

    return (struct operation) {.ty = PARSE_ERROR, .error_message = error_message};
}

[[gnu::nonnull(1)]]
/** Consume all events until the is found. */
static struct operation parse_consume(parser_t *NONNULL parser, bool is_sequence, struct operation result) {
    size_t mapping_stack = is_sequence ? 0 : 1;
    size_t sequence_stack = is_sequence ? 1 : 0;

    while (!parser_finished(parser)) {
        yaml_event_t event;
        int rv = yaml_parser_parse(&(parser->yaml), &event);
        if unlikely (rv == 0) {
            return parse_fail(parser);
        }

        const yaml_mark_t position = event.start_mark;
        const yaml_event_type_t type = event.type;
        yaml_event_delete(&event);

        switch (type) {
            case YAML_MAPPING_START_EVENT:
                mapping_stack += 1;
                continue;

            case YAML_MAPPING_END_EVENT:
                if unlikely (mapping_stack == 0) {
                    return parse_invalid(parser, position, "unexpected end of mapping");
                }
                mapping_stack -= 1;
                if likely (mapping_stack == 0 && sequence_stack == 0) {
                    return result;
                }
                continue;

            case YAML_SEQUENCE_START_EVENT:
                sequence_stack += 1;
                continue;

            case YAML_SEQUENCE_END_EVENT:
                if unlikely (sequence_stack == 0) {
                    return parse_invalid(parser, position, "unexpected end of sequence");
                }
                sequence_stack -= 1;
                if likely (mapping_stack == 0 && sequence_stack == 0) {
                    return result;
                }
                continue;

            case YAML_STREAM_END_EVENT:
                return parse_done(parser);

            case YAML_STREAM_START_EVENT:
            case YAML_DOCUMENT_START_EVENT:
            case YAML_DOCUMENT_END_EVENT:
                return parse_invalid(parser, position, "unexpected end of document");

            case YAML_NO_EVENT:
            case YAML_ALIAS_EVENT:
            case YAML_SCALAR_EVENT:
                continue;
        }
    }

    return result;
}

[[nodiscard("useless call if discarded"), gnu::pure, gnu::nonnull(1, 2)]]
/**
 * Checks if `key` matches `expected`, ignoring type qualifiers.
 */
static inline bool streq(const yaml_char_t *NONNULL restrict key, const char *NONNULL restrict expected) {
    return strcmp((const char *) key, expected) == 0;
}

[[nodiscard("useless call if discarded"), gnu::const, gnu::nonnull(1)]]
/**
 * Parses the operation type from a given YAML scalar key.
 *
 * For example, \"add_movie\" => ADD_MOVIE.
 */
static enum operation_ty parse_ty(const yaml_char_t *NONNULL key) {
    if (streq(key, "add_movie") || streq(key, "1")) {
        return ADD_MOVIE;
    } else if (streq(key, "add_genre") || streq(key, "2")) {
        return ADD_GENRE;
    } else if (streq(key, "remove_movie") || streq(key, "3")) {
        return REMOVE_MOVIE;
    } else if (streq(key, "list_summaries") || streq(key, "4")) {
        return LIST_SUMMARIES;
    } else if (streq(key, "list_movies") || streq(key, "5")) {
        return LIST_MOVIES;
    } else if (streq(key, "get_movie") || streq(key, "6")) {
        return GET_MOVIE;
    } else if (streq(key, "search_by_genre") || streq(key, "7")) {
        return SEARCH_BY_GENRE;
    } else {
        return PARSE_ERROR;
    }
}

[[nodiscard("uninitialized output if false"), gnu::hot, gnu::nonnull(1, 2)]]
/**
 * Parses a 64-bit integer from the string `str`.
 *
 * @param str  Non-null input string to parse.
 * @param out  Output parameter for the parsed int64_t.
 * @return true on success, false on invalid format or out of range.
 */
static bool parse_i64(const char *NONNULL str, int64_t *NONNULL out) {
    if unlikely (str[0] == '\0') {
        return false;
    }

    char *end;
    errno = 0;

    constexpr int AS_DECIMAL = 10;
    long long val = strtoll(str, &end, AS_DECIMAL);
    if unlikely (errno == ERANGE || val < INT64_MIN || val > INT64_MAX || end == str || *end != '\0') {
        return false;
    }

    *out = (int64_t) val;
    return true;
}

/**
 * Mapping from key strings to an enum that identifies which field we're parsing.
 */
enum [[gnu::packed]] current_key {
    NONE,
    ID_KEY,
    TITLE_KEY,
    GENRE_KEY,
    DIRECTOR_KEY,
    YEAR_KEY,
    OTHER_KEY,
};

[[nodiscard("useless call if discarded"), gnu::pure, gnu::nonnull(1)]]
/**
 * Converts a YAML scalar key into the corresponding enum current_key.
 *
 * @param key The YAML scalar key (e.g., \"title\", \"id\", \"year\", etc.)
 * @return The associated enum value (e.g., TITLE_KEY), or OTHER_KEY if unknown.
 */
static enum current_key parse_key(const yaml_char_t *NONNULL key) {
    if (streq(key, "id")) {
        return ID_KEY;
    } else if (streq(key, "title")) {
        return TITLE_KEY;
    } else if (streq(key, "genre") || streq(key, "genres")) {
        return GENRE_KEY;
    } else if (streq(key, "director")) {
        return DIRECTOR_KEY;
    } else if (streq(key, "year") || streq(key, "release_year")) {
        return YEAR_KEY;
    } else {
        return OTHER_KEY;
    }
}

[[nodiscard("allocated memory must be freed"), gnu::nonnull(1)]]
/**
 * Parses a sequence of genres from the YAML stream.
 *
 * Expects a sequence like:
 *   genres:
 *     - Sci-Fi
 *     - Comedy
 *
 * Returns `PARSE_DONE` on success, `PARSE_ERROR` on error.
 */
static struct operation parse_genre_list(parser_t *NONNULL parser) {
    const bool ignore = movie_builder_has_genres(parser->builder);
    if likely (!ignore) {
        movie_builder_start_genres(parser->builder);
    }

    struct operation last_error = {.ty = PARSE_DONE};

    bool in_list = false;
    while (!parser_finished(parser)) {
        yaml_event_t event;
        int rv = yaml_parser_parse(&(parser->yaml), &event);
        if unlikely (rv == 0) {
            return parse_fail(parser);
        }

        const yaml_mark_t position = event.start_mark;
        switch (event.type) {
            case YAML_SCALAR_EVENT:
                if likely (!ignore) {
                    const char *NONNULL genre = (const char *) event.data.scalar.value;
                    bool ok = movie_builder_add_genre(parser->builder, strlen(genre), genre);
                    if unlikely (!ok) {
                        last_error = parse_invalid(parser, position, "out of memory when adding a genre");
                    }
                }
                yaml_event_delete(&event);

                if unlikely (!in_list) {
                    return last_error;
                }
                continue;

            case YAML_SEQUENCE_START_EVENT:
                yaml_event_delete(&event);
                if unlikely (in_list) {
                    struct operation error = parse_invalid(parser, position, "internal sequence in genre list invalid");
                    last_error = parse_consume(parser, true, error);
                } else {
                    in_list = true;
                }
                continue;

            case YAML_SEQUENCE_END_EVENT:
                yaml_event_delete(&event);
                return last_error;

            case YAML_MAPPING_START_EVENT:
                yaml_event_delete(&event);
                struct operation error = parse_invalid(parser, position, "mapping unsupported in genre list");
                last_error = parse_consume(parser, false, error);
                continue;

            case YAML_NO_EVENT:
            case YAML_ALIAS_EVENT:
                // just consume & ignore
                yaml_event_delete(&event);
                continue;

            case YAML_STREAM_END_EVENT:
                parse_done(parser);
                [[fallthrough]];
            case YAML_DOCUMENT_END_EVENT:
            case YAML_DOCUMENT_START_EVENT:
            case YAML_MAPPING_END_EVENT:
            case YAML_STREAM_START_EVENT:
            default:
                yaml_event_delete(&event);
                if likely (last_error.ty == PARSE_ERROR) {
                    return last_error;
                } else {
                    return parse_invalid(parser, position, "document ended unexpectedly");
                }
        }
    }

    if likely (last_error.ty == PARSE_ERROR) {
        return last_error;
    } else {
        return (struct operation) {.ty = PARSE_ERROR, .error_message = "document ended unexpectedly"};
    }
}

[[gnu::pure, gnu::hot, gnu::nonnull(1)]]
/** Check if movie input is complete. */
static inline bool is_movie_done(const movie_builder_t *NONNULL builder) {
    return movie_builder_has_id(builder) && movie_builder_has_title(builder) && movie_builder_has_director(builder)
        && movie_builder_has_release_year(builder) && movie_builder_has_genres(builder);
}

[[gnu::hot, gnu::nonnull(1)]]
/** Try to build the movie input. */
static struct operation parse_movie_done(const movie_builder_t *NONNULL builder, enum operation_ty ty) {
    assume(is_movie_done(builder));

    struct movie movie;
    bool ok = movie_builder_take_current_movie(builder, &movie);
    if unlikely (!ok) {
        return (struct operation) {.ty = PARSE_ERROR, .error_message = "out of memory for movie input"};
    }

    return (struct operation) {.ty = ty, .movie = movie};
}

[[gnu::nonnull(1, 2)]]
/** Parse a the movie title from a scalar value. */
static struct operation parse_movie_title(
    parser_t *NONNULL parser,
    const yaml_char_t *NONNULL value,
    yaml_mark_t position,
    struct operation last_error
) {
    const char *NONNULL title = (const char *) value;
    bool ok = movie_builder_set_title(parser->builder, strlen(title), title);
    if unlikely (!ok) {
        return parse_invalid(parser, position, "out of memory for title input");
    }
    return last_error;
}

[[gnu::nonnull(1, 2)]]
/** Parse a the movie director from a scalar value. */
static struct operation parse_movie_director(
    parser_t *NONNULL parser,
    const yaml_char_t *NONNULL value,
    yaml_mark_t position,
    struct operation last_error
) {
    const char *NONNULL director = (const char *) value;
    bool ok = movie_builder_set_director(parser->builder, strlen(director), director);
    if unlikely (!ok) {
        return parse_invalid(parser, position, "out of memory for director input");
    }
    return last_error;
}

[[gnu::nonnull(1, 2)]]
/** Parse a the movie director from a scalar value. */
static struct operation parse_movie_year(
    parser_t *NONNULL parser,
    const yaml_char_t *NONNULL value,
    yaml_mark_t position,
    struct operation last_error
) {
    int64_t release_year;
    bool ok = parse_i64((const char *) value, &release_year);
    if unlikely (!ok) {
        return parse_invalid(parser, position, "release year is not a valid integer");
    } else if unlikely (release_year < INT_MIN || release_year > INT_MAX) {
        return parse_invalid(parser, position, "release year out of range");
    }

    movie_builder_set_release_year(parser->builder, (int) release_year);
    return last_error;
}

[[nodiscard("allocated memory must be freed"), gnu::nonnull(1)]]
/**
 * Parses a YAML mapping containing a new movie:
 *  title, director, year, and genres fields are required.
 *
 * Returns an operation with ty=ADD_MOVIE (or similar) and a pointer to a newly allocated struct movie.
 * If anything goes wrong, returns an operation with ty=PARSE_ERROR.
 */
static struct operation parse_movie(parser_t *NONNULL parser, enum operation_ty ty) {
    movie_builder_reset(parser->builder);
    movie_builder_set_id(parser->builder, 0);

    bool in_mapping = false;
    enum current_key key = NONE;
    struct operation last_error = {.ty = PARSE_DONE};

    while (!parser_finished(parser)) {
        yaml_event_t event;
        int rv = yaml_parser_parse(&(parser->yaml), &event);
        if unlikely (rv == 0) {
            if likely (is_movie_done(parser->builder)) {
                return parse_movie_done(parser->builder, ty);
            } else {
                return parse_fail(parser);
            }
        }

        const yaml_mark_t position = event.start_mark;
        switch (event.type) {
            case YAML_SCALAR_EVENT:
                switch (key) {
                    case NONE:
                        if likely (in_mapping) {
                            key = parse_key(event.data.scalar.value);
                            yaml_event_delete(&event);

                            // If we detect it's the GENRE_KEY, parse a sequence
                            if (key == GENRE_KEY) {
                                key = NONE;

                                struct operation error = parse_genre_list(parser);
                                if unlikely (error.ty == PARSE_ERROR) {
                                    last_error = error;
                                }
                            }
                            continue;
                        } else {
                            // Not inside a mapping => error
                            last_error = parse_invalid(parser, position, "invalid movie input, not inside a mapping");
                        }

                    case TITLE_KEY:
                        if (!movie_builder_has_title(parser->builder)) {
                            last_error = parse_movie_title(parser, event.data.scalar.value, position, last_error);
                        }
                        break;

                    case DIRECTOR_KEY:
                        if (!movie_builder_has_director(parser->builder)) {
                            last_error = parse_movie_director(parser, event.data.scalar.value, position, last_error);
                        }
                        break;

                    case YEAR_KEY:
                        if (!movie_builder_has_release_year(parser->builder)) {
                            last_error = parse_movie_year(parser, event.data.scalar.value, position, last_error);
                        }
                        break;

                    case GENRE_KEY:
                        last_error = parse_invalid(parser, position, "unexpected genre key");
                        break;

                    case ID_KEY:
                    case OTHER_KEY:
                    default:
                        break;
                }

                yaml_event_delete(&event);
                key = NONE;
                continue;

            case YAML_MAPPING_START_EVENT:
                yaml_event_delete(&event);
                if unlikely (in_mapping) {
                    struct operation error = parse_invalid(parser, position, "internal mapping invalid");
                    last_error = parse_consume(parser, false, error);
                } else {
                    in_mapping = true;
                }
                continue;

            case YAML_MAPPING_END_EVENT:
                yaml_event_delete(&event);
                if unlikely (!in_mapping) {
                    // this is a top level event that shouldn't have landed here
                    parser->in_mapping = false;
                }

                if likely (is_movie_done(parser->builder)) {
                    return parse_movie_done(parser->builder, ty);
                } else if likely (last_error.ty == PARSE_ERROR) {
                    return last_error;
                } else {
                    return parse_invalid(parser, position, "operation incomplete");
                }

            case YAML_SEQUENCE_START_EVENT:
                yaml_event_delete(&event);
                struct operation error = parse_invalid(parser, position, "sequence unsupported in this operation");
                last_error = parse_consume(parser, true, error);
                continue;

            case YAML_NO_EVENT:
            case YAML_ALIAS_EVENT:
                // just consume & ignore
                yaml_event_delete(&event);
                continue;

            case YAML_STREAM_END_EVENT:
                parse_done(parser);
                [[fallthrough]];
            case YAML_DOCUMENT_END_EVENT:
            case YAML_DOCUMENT_START_EVENT:
            case YAML_SEQUENCE_END_EVENT:
            case YAML_STREAM_START_EVENT:
            default:
                yaml_event_delete(&event);
                if likely (is_movie_done(parser->builder)) {
                    return parse_movie_done(parser->builder, ty);
                } else if likely (last_error.ty == PARSE_ERROR) {
                    return last_error;
                } else {
                    return parse_invalid(parser, position, "document ended unexpectedly");
                }
        }
    }

    if likely (is_movie_done(parser->builder)) {
        return parse_movie_done(parser->builder, ty);
    } else if likely (last_error.ty == PARSE_ERROR) {
        return last_error;
    } else {
        return (struct operation) {.ty = PARSE_ERROR, .error_message = "document ended unexpectedly"};
    }
}

[[gnu::pure, gnu::hot, gnu::nonnull(1)]]
/** Check if movie key is ready. */
static inline bool is_movie_key_done(const movie_builder_t *NONNULL builder) {
    return movie_builder_has_id(builder) && movie_builder_has_title(builder);
}

[[gnu::pure, gnu::hot, gnu::nonnull(1)]]
/** Try to build the movie key input. */
static struct operation parse_movie_key_done(const movie_builder_t *NONNULL builder, enum operation_ty ty) {
    assume(is_movie_key_done(builder));

    struct movie_summary summary;
    movie_builder_take_current_summary(builder, &summary);
    return (struct operation) {
        .ty = ty,
        .key = {.movie_id = summary.id, .genre = summary.title}
    };
}

[[gnu::nonnull(1, 2)]]
/** Parse a single movie id from a scalar value. */
static struct operation parse_movie_key_id(
    parser_t *NONNULL parser,
    const yaml_char_t *NONNULL value,
    yaml_mark_t position,
    struct operation last_error
) {
    int64_t id;
    bool ok = parse_i64((const char *) value, &id);
    if unlikely (!ok) {
        return parse_invalid(parser, position, "movie id is not a valid integer");
    }

    movie_builder_set_id(parser->builder, id);
    return last_error;
}

[[gnu::nonnull(1, 2)]]
/** Parse a single genre string from a scalar value. */
static struct operation parse_movie_key_genre(
    parser_t *NONNULL parser,
    const yaml_char_t *NONNULL value,
    yaml_mark_t position,
    struct operation last_error
) {
    const char *NONNULL genre = (const char *) value;
    bool ok = movie_builder_set_title(parser->builder, strlen(genre), genre);
    if unlikely (!ok) {
        return parse_invalid(parser, position, "out of memory for genre input");
    }
    return last_error;
}

[[nodiscard("allocated memory must be freed"), gnu::nonnull(1)]]
/**
 * Parses a smaller mapping that either needs an ID and/or a genre.
 *
 * For example, \"remove_movie\" might just need the ID. \"add_genre\" might need both the ID and a new genre string.
 *
 * @param parser      The YAML parser.
 * @param ty          The operation type to assign.
 * @param needs_id    Whether an \"id\" field is mandatory.
 * @param needs_genre Whether a \"genre\" field is mandatory.
 *
 * @return An operation with a struct movie_key containing the relevant fields, or PARSE_ERROR on error.
 */
static struct operation parse_movie_key(
    parser_t *NONNULL parser,
    enum operation_ty ty,
    const bool needs_id,
    const bool needs_genre
) {
    movie_builder_reset(parser->builder);
    if (!needs_id) {
        movie_builder_set_id(parser->builder, 0);
    }
    if (!needs_genre) {
        // using summary title as input genre
        movie_builder_set_title(parser->builder, 0, "");
    }

    bool in_mapping = false;
    enum current_key key = NONE;
    struct operation last_error = {.ty = PARSE_DONE};

    while (!parser_finished(parser)) {
        yaml_event_t event;
        int rv = yaml_parser_parse(&(parser->yaml), &event);
        if unlikely (rv == 0) {
            if likely (is_movie_key_done(parser->builder)) {
                return parse_movie_key_done(parser->builder, ty);
            } else {
                return parse_fail(parser);
            }
        }

        const yaml_mark_t position = event.start_mark;
        switch (event.type) {
            case YAML_SCALAR_EVENT:
                switch (key) {
                    case NONE:
                        if (in_mapping) {
                            key = parse_key(event.data.scalar.value);
                        } else if (!movie_builder_has_id(parser->builder) && movie_builder_has_title(parser->builder)) {
                            // e.g., remove_movie wants just an ID
                            last_error = parse_movie_key_id(parser, event.data.scalar.value, position, last_error);
                        } else if (movie_builder_has_id(parser->builder) && !movie_builder_has_title(parser->builder)) {
                            // just a genre for searching
                            last_error = parse_movie_key_genre(parser, event.data.scalar.value, position, last_error);
                        } else {
                            // mismatch
                            last_error = parse_invalid(parser, position, "invalid input for operation");
                        }

                        yaml_event_delete(&event);
                        continue;

                    case ID_KEY:
                        if likely (!movie_builder_has_id(parser->builder)) {
                            last_error = parse_movie_key_id(parser, event.data.scalar.value, position, last_error);
                        }
                        break;

                    case GENRE_KEY:
                        if likely (!movie_builder_has_title(parser->builder)) {
                            last_error = parse_movie_key_genre(parser, event.data.scalar.value, position, last_error);
                        }
                        break;

                    case TITLE_KEY:
                    case DIRECTOR_KEY:
                    case YEAR_KEY:
                    case OTHER_KEY:
                    default:
                        break;
                }

                yaml_event_delete(&event);
                key = NONE;
                continue;

            case YAML_MAPPING_START_EVENT:
                yaml_event_delete(&event);
                if unlikely (in_mapping) {
                    struct operation error = parse_invalid(parser, position, "internal mapping invalid");
                    last_error = parse_consume(parser, false, error);
                } else {
                    in_mapping = true;
                }
                continue;

            case YAML_MAPPING_END_EVENT:
                yaml_event_delete(&event);
                if unlikely (!in_mapping) {
                    // top-level event that landed here
                    parser->in_mapping = false;
                }

                if likely (is_movie_key_done(parser->builder)) {
                    return parse_movie_key_done(parser->builder, ty);
                } else if likely (last_error.ty == PARSE_ERROR) {
                    return last_error;
                } else {
                    return parse_invalid(parser, position, "operation incomplete");
                }

            case YAML_SEQUENCE_START_EVENT:
                yaml_event_delete(&event);
                struct operation error = parse_invalid(parser, position, "sequence unsupported in this operation");
                last_error = parse_consume(parser, true, error);
                continue;

            case YAML_NO_EVENT:
            case YAML_ALIAS_EVENT:
                // just consume & ignore
                yaml_event_delete(&event);
                continue;

            case YAML_STREAM_END_EVENT:
                parse_done(parser);
                [[fallthrough]];
            case YAML_DOCUMENT_END_EVENT:
            case YAML_DOCUMENT_START_EVENT:
            case YAML_SEQUENCE_END_EVENT:
            case YAML_STREAM_START_EVENT:
            default:
                yaml_event_delete(&event);
                if likely (is_movie_key_done(parser->builder)) {
                    return parse_movie_key_done(parser->builder, ty);
                } else if likely (last_error.ty == PARSE_ERROR) {
                    return last_error;
                } else {
                    return parse_invalid(parser, position, "document ended unexpectedly");
                }
        }
    }

    if likely (is_movie_done(parser->builder)) {
        return parse_movie_done(parser->builder, ty);
    } else if likely (last_error.ty == PARSE_ERROR) {
        return last_error;
    } else {
        return (struct operation) {.ty = PARSE_ERROR, .error_message = "document ended unexpectedly"};
    }
}

/** Reads the next operation from the YAML parser */
struct operation parser_next_op(parser_t *NONNULL parser) {
    enum operation_ty ty = PARSE_ERROR;

    while (!parser_finished(parser)) {
        yaml_event_t event;
        int rv = yaml_parser_parse(&(parser->yaml), &event);
        if unlikely (rv == 0) {
            return parse_fail(parser);
        }

        const yaml_mark_t position = event.start_mark;
        switch (event.type) {
            case YAML_SCALAR_EVENT:
                // The scalar event is presumably the operation name
                ty = parse_ty(event.data.scalar.value);
                yaml_event_delete(&event);

                if (parser->in_mapping) {
                    switch (ty) {
                        case ADD_MOVIE:
                            return parse_movie(parser, ty);
                        case ADD_GENRE:
                            return parse_movie_key(parser, ty, true, true);
                        case GET_MOVIE:
                        case REMOVE_MOVIE:
                            return parse_movie_key(parser, ty, true, false);
                        case SEARCH_BY_GENRE:
                            return parse_movie_key(parser, ty, false, true);
                        case LIST_SUMMARIES:
                        case LIST_MOVIES:
                            return parse_movie_key(parser, ty, false, false);
                        case PARSE_ERROR:
                        default:
                            return parse_invalid(parser, position, "unrecognized operation key");
                    }
                } else {
                    // If we're outside a mapping, only certain ops are valid
                    switch (ty) {
                        case LIST_SUMMARIES:
                        case LIST_MOVIES:
                            return (struct operation) {.ty = ty};
                        case GET_MOVIE:
                        case REMOVE_MOVIE:
                        case SEARCH_BY_GENRE:
                        case ADD_MOVIE:
                        case ADD_GENRE:
                            return parse_invalid(parser, position, "operation requires a dictionary");
                        case PARSE_ERROR:
                        default:
                            return parse_invalid(parser, position, "unrecognized operation key");
                    }
                }

            case YAML_MAPPING_START_EVENT:
                yaml_event_delete(&event);
                if unlikely (parser->in_mapping) {
                    return parse_invalid(parser, position, "another operation start without finishing the first one");
                }
                parser->in_mapping = true;
                continue;

            case YAML_MAPPING_END_EVENT:
                yaml_event_delete(&event);
                if unlikely (!parser->in_mapping) {
                    return parse_invalid(parser, position, "finishing an unstarted operation");
                }
                parser->in_mapping = false;
                continue;

            case YAML_NO_EVENT:
            case YAML_ALIAS_EVENT:
            case YAML_STREAM_START_EVENT:
            case YAML_DOCUMENT_START_EVENT:
            case YAML_DOCUMENT_END_EVENT:
            case YAML_SEQUENCE_START_EVENT:
            case YAML_SEQUENCE_END_EVENT:
                // just consume & ignore
                yaml_event_delete(&event);
                continue;

            case YAML_STREAM_END_EVENT:
            default:
                yaml_event_delete(&event);
                return parse_done(parser);
        }
    }

    return parse_done(parser);
}
