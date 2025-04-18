#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>

#include <yaml.h>

#include "../database/database.h"
#include "../defines.h"
#include "./parser.h"

[[gnu::nonnull(1), gnu::hot]]
/**
 * A read handler for libyaml that reads from a file descriptor.
 */
static int sock_read_handler(void *data, unsigned char *NONNULL buffer, size_t size, size_t *NONNULL size_read) {
    const int sock_fd = INT_FROM_PTR(data);
    ssize_t rv = recv(sock_fd, buffer, size, 0);
    if unlikely (rv < 0) {
        *size_read = 0;
        return 0;
    } else {
        *size_read = (size_t) rv;
        return 1;
    }
}

/** Initializes YAML parser.  */
bool parser_start(yaml_parser_t *NONNULL parser, const int sock_fd) {
    int rv = yaml_parser_initialize(parser);
    if unlikely (rv == 0) {
        return false;
    }

    void *data = PTR_FROM_INT(sock_fd);
    yaml_parser_set_encoding(parser, YAML_UTF8_ENCODING);
    yaml_parser_set_input(parser, sock_read_handler, data);
    return true;
}

[[nodiscard("useless call if discarded"), gnu::regcall, gnu::pure, gnu::nonnull(1, 2)]]
/**
 * Checks if `key` matches `expected`, ignoring type qualifiers.
 */
static inline bool streq(const yaml_char_t *NONNULL restrict key, const char *expected) {
    return strcmp((const char *) key, expected) == 0;
}

[[nodiscard("useless call if discarded"), gnu::regcall, gnu::const, gnu::nonnull(1)]]
/**
 * Parses the operation type from a given YAML scalar key.
 *
 * For example, \"add_movie\" => ADD_MOVIE.
 */
enum operation_ty parse_ty(const yaml_char_t *NONNULL key) {
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
        return INVALID_OP;
    }
}

/**
 * Computes the length of a NULL-terminated array of strings.
 */
[[nodiscard("useless call if discarded"), gnu::regcall, gnu::pure, gnu::nonnull(1)]]
static size_t list_len(const char *NULLABLE const list[NONNULL]) {
    if unlikely (list == NULL) {
        return 0;
    }

    size_t len = 0;
    while (list[len] != NULL) {
        len++;
    }
    return len;
}

/**
 * Parses a 64-bit integer from the string `str`.
 *
 * @param str  Non-null input string to parse.
 * @param out  Output parameter for the parsed int64_t.
 * @return true on success, false on invalid format or out of range.
 */
[[nodiscard("uninitialized output if false"), gnu::regcall]]
static bool parse_i64(const char *NONNULL str, int64_t *NONNULL out) {
    if unlikely (str == NULL || str[0] == '\0') {
        return false;
    }

    char *end = NULL;
    errno = 0;

    long long val = strtoll(str, &end, 10);
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

/**
 * Converts a YAML scalar key into the corresponding enum current_key.
 *
 * @param key The YAML scalar key (e.g., \"title\", \"id\", \"year\", etc.)
 * @return The associated enum value (e.g., TITLE_KEY), or OTHER_KEY if unknown.
 */
[[nodiscard("useless call if discarded"), gnu::regcall, gnu::pure]]
enum current_key parse_key(const yaml_char_t *NONNULL key) {
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

[[gnu::regcall]]
/**
 * Helper function to free up to two single strings (`s1`, `s2`) and a NULL-terminated array of strings `ss`.
 *
 * Returns an operation with ty=INVALID_OP, suitable for error returns.
 */
static struct operation parse_fail(char *NULLABLE s1, char *NULLABLE s2, char *NULLABLE *NULLABLE ss) {
    if (s1 != NULL) {
        free(s1);
    }
    if (s2 != NULL) {
        free(s2);
    }
    if (ss != NULL) {
        for (size_t i = 0; ss[i] != NULL; i++) {
            free(ss[i]);
        }
        free((void *) ss);
    }
    return (struct operation) {.ty = INVALID_OP};
}

[[nodiscard("allocated memory must be freed"), gnu::regcall, gnu::nonnull(1), gnu::malloc]]
/**
 * Parses a sequence of genres from the YAML stream.
 *
 * Expects a sequence like:
 *   genres:
 *     - Sci-Fi
 *     - Comedy
 *
 * Returns a NULL-terminated array of strings on success, or NULL if error.
 */
static char *NULLABLE *NULLABLE parse_genre_list(yaml_parser_t *NONNULL parser) {
    size_t capacity = 8;
    char *NULLABLE *genres = (char **) malloc(capacity * sizeof(char *));
    if unlikely (genres == NULL) {
        return NULL;
    }

    size_t len = 0;
    genres[len] = NULL;

    bool in_list = false;
    while (true) {
        yaml_event_t event;
        int rv = yaml_parser_parse(parser, &event);
        if unlikely (rv == 0) {
            parse_fail(NULL, NULL, genres);
            return NULL;
        }

        switch (event.type) {
            case YAML_SCALAR_EVENT:
                // If we haven't encountered SEQUENCE_START, that's invalid
                if unlikely (!in_list) {
                    yaml_event_delete(&event);
                    parse_fail(NULL, NULL, genres);
                    return NULL;
                }

                // Expand array if needed
                if unlikely (len >= capacity - 1) {
                    capacity += 8;
                    char **ptr = (char **) realloc((void *) genres, capacity * sizeof(char *));
                    if unlikely (ptr == NULL) {
                        yaml_event_delete(&event);
                        parse_fail(NULL, NULL, genres);
                        return NULL;
                    }
                    genres = ptr;
                }

                genres[len] = strdup((char *) event.data.scalar.value);
                yaml_event_delete(&event);

                if unlikely (genres[len] == NULL) {
                    parse_fail(NULL, NULL, genres);
                    return NULL;
                }
                genres[++len] = NULL;
                continue;

            case YAML_SEQUENCE_START_EVENT:
                yaml_event_delete(&event);
                if unlikely (in_list) {
                    parse_fail(NULL, NULL, genres);
                    return NULL;
                }
                in_list = true;
                continue;

            case YAML_NO_EVENT:
            case YAML_SEQUENCE_END_EVENT:
                yaml_event_delete(&event);
                return genres;

            case YAML_ALIAS_EVENT:
                yaml_event_delete(&event);
                continue;

            // If we hit any other event, it's unexpected for a simple genre list
            case YAML_STREAM_START_EVENT:
            case YAML_STREAM_END_EVENT:
            case YAML_DOCUMENT_START_EVENT:
            case YAML_DOCUMENT_END_EVENT:
            case YAML_MAPPING_START_EVENT:
            case YAML_MAPPING_END_EVENT:
            default:
                yaml_event_delete(&event);
                parse_fail(NULL, NULL, genres);
                return NULL;
        }
    }
}

[[nodiscard("allocated memory must be freed"), gnu::regcall, gnu::nonnull(1)]]
/**
 * Parses a YAML mapping containing a new movie:
 *  title, director, year, and genres fields are required.
 *
 * Returns an operation with ty=ADD_MOVIE (or similar) and a pointer to a newly allocated struct movie.
 * If anything goes wrong, returns an operation with ty=INVALID_OP.
 */
static struct operation parse_movie(yaml_parser_t *NONNULL parser, enum operation_ty ty) {
    char *title = NULL;
    char *director = NULL;
    int64_t release_year = 0;
    char *NULLABLE *genres = NULL;

    bool needs_year = true;

    bool in_mapping = false;
    enum current_key key = NONE;
    while (true) {
        yaml_event_t event;
        int rv = yaml_parser_parse(parser, &event);
        if unlikely (rv == 0) {
            return parse_fail(title, director, genres);
        }

        switch (event.type) {
            case YAML_SCALAR_EVENT:
                switch (key) {
                    case NONE:
                        if (in_mapping) {
                            key = parse_key(event.data.scalar.value);
                            yaml_event_delete(&event);

                            // If we detect it's the GENRE_KEY, parse a sequence
                            if (key == GENRE_KEY) {
                                key = NONE;
                                if (genres == NULL) {
                                    genres = parse_genre_list(parser);
                                    if unlikely (genres == NULL) {
                                        return parse_fail(title, director, genres);
                                    }
                                } else {
                                    // parse_genre_list again? Probably an error if repeated
                                    char **temp = parse_genre_list(parser);
                                    if (temp != NULL) {
                                        parse_fail(NULL, NULL, temp);
                                    }
                                }
                            }
                            continue;
                        } else {
                            // Not inside a mapping => error
                            yaml_event_delete(&event);
                            return parse_fail(title, director, genres);
                        }
                        break;

                    case YEAR_KEY:
                        if (needs_year) {
                            needs_year = !parse_i64((const char *) event.data.scalar.value, &release_year);
                        }
                        break;

                    case TITLE_KEY:
                        if (title == NULL) {
                            title = strdup((const char *) event.data.scalar.value);
                        }
                        break;

                    case DIRECTOR_KEY:
                        if (director == NULL) {
                            director = strdup((const char *) event.data.scalar.value);
                        }
                        break;

                    // If we see GENRE_KEY again, that's unexpected
                    case GENRE_KEY:
                        yaml_event_delete(&event);
                        return parse_fail(title, director, genres);

                    case ID_KEY:
                    case OTHER_KEY:
                    default:
                        break;
                }

                yaml_event_delete(&event);
                key = NONE;
                continue;

            case YAML_ALIAS_EVENT:
                yaml_event_delete(&event);
                continue;

            case YAML_MAPPING_START_EVENT:
                yaml_event_delete(&event);
                if unlikely (in_mapping) {
                    return parse_fail(title, director, genres);
                }
                in_mapping = true;
                continue;

            case YAML_NO_EVENT:
            case YAML_MAPPING_END_EVENT:
                yaml_event_delete(&event);
                // try to close the current operation
                if likely (title != NULL && director != NULL && needs_year && genres != NULL) {
                    size_t len = list_len((const char *const *) genres);
                    struct movie *movie = malloc(sizeof(struct movie) + (len + 1) * sizeof(char *));
                    if unlikely (movie == NULL) {
                        return parse_fail(title, director, genres);
                    }

                    movie->id = 0;
                    movie->title = title;
                    movie->director = director;
                    movie->release_year = (int) release_year;
                    for (size_t i = 0; i < len; i++) {
                        movie->genres[i] = genres[i];
                    }
                    movie->genres[len] = NULL;
                    free((void *) genres);

                    return (struct operation) {.ty = ty, .movie = movie};
                } else if (event.type == YAML_MAPPING_END_EVENT) {
                    // mapping finshed, operation should have been finished
                    return parse_fail(title, director, genres);
                } else {
                    continue;
                }

            // If we hit any other event, it's not valid for a single-level movie mapping
            case YAML_DOCUMENT_END_EVENT:
            case YAML_DOCUMENT_START_EVENT:
            case YAML_STREAM_END_EVENT:
            case YAML_STREAM_START_EVENT:
            case YAML_SEQUENCE_START_EVENT:
            case YAML_SEQUENCE_END_EVENT:
            default:
                yaml_event_delete(&event);
                return parse_fail(title, director, genres);
        }
    }
}

[[nodiscard("allocated memory must be freed"), gnu::regcall, gnu::nonnull(1)]]
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
 * @return An operation with a struct movie_key containing the relevant fields, or INVALID_OP on error.
 */
static struct operation
    parse_movie_key(yaml_parser_t *NONNULL parser, enum operation_ty ty, bool needs_id, bool needs_genre) {
    char *genre = NULL;
    int64_t id = 0;

    bool in_mapping = false;
    enum current_key key = NONE;
    while (true) {
        yaml_event_t event;
        int rv = yaml_parser_parse(parser, &event);
        if unlikely (rv == 0) {
            return parse_fail(genre, NULL, NULL);
        }

        switch (event.type) {
            case YAML_SCALAR_EVENT:
                switch (key) {
                    case NONE:
                        if (in_mapping) {
                            key = parse_key(event.data.scalar.value);
                            yaml_event_delete(&event);
                            continue;
                        } else if (needs_id && !needs_genre) {
                            // e.g., remove_movie wants just an ID
                            bool ok = parse_i64((const char *) event.data.scalar.value, &id);
                            yaml_event_delete(&event);

                            if unlikely (!ok) {
                                return parse_fail(genre, NULL, NULL);
                            }
                            return (struct operation) {
                                .ty = ty,
                                .key = {.movie_id = id, .genre = genre}
                            };
                        } else if (!needs_id && needs_genre) {
                            // e.g., search_by_genre wants just a genre
                            genre = strdup((const char *) event.data.scalar.value);
                            yaml_event_delete(&event);

                            if unlikely (genre == NULL) {
                                return parse_fail(genre, NULL, NULL);
                            }
                            return (struct operation) {
                                .ty = ty,
                                .key = {.movie_id = id, .genre = genre}
                            };
                        } else {
                            // mismatch
                            yaml_event_delete(&event);
                            return parse_fail(genre, NULL, NULL);
                        }

                    case ID_KEY:
                        if (needs_id) {
                            needs_id = !parse_i64((const char *) event.data.scalar.value, &id);
                        }
                        break;

                    case GENRE_KEY:
                        if (needs_genre) {
                            genre = strdup((const char *) event.data.scalar.value);
                            needs_genre = (genre == NULL);
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

            case YAML_ALIAS_EVENT:
                yaml_event_delete(&event);
                continue;

            case YAML_MAPPING_START_EVENT:
                yaml_event_delete(&event);
                if unlikely (in_mapping) {
                    return parse_fail(genre, NULL, NULL);
                }
                in_mapping = true;
                continue;

            case YAML_NO_EVENT:
                yaml_event_delete(&event);
                // If we needed ID or genre and didn't parse it, fail
                if (in_mapping || needs_id || needs_genre) {
                    continue;
                }
                return (struct operation) {
                    .ty = ty,
                    .key = {.movie_id = id, .genre = genre}
                };

            case YAML_MAPPING_END_EVENT:
                yaml_event_delete(&event);
                // If we needed ID or genre and didn't parse it, fail
                if unlikely (!in_mapping || needs_id || needs_genre) {
                    return parse_fail(genre, NULL, NULL);
                }
                return (struct operation) {
                    .ty = ty,
                    .key = {.movie_id = id, .genre = genre}
                };

            case YAML_DOCUMENT_END_EVENT:
            case YAML_DOCUMENT_START_EVENT:
            case YAML_SEQUENCE_END_EVENT:
            case YAML_SEQUENCE_START_EVENT:
            case YAML_STREAM_END_EVENT:
            case YAML_STREAM_START_EVENT:
            default:
                yaml_event_delete(&event);
                return parse_fail(genre, NULL, NULL);
        }
    }
}

/** Reads the next operation from the YAML parser */
struct operation parser_next_op(yaml_parser_t *NONNULL parser, bool *NONNULL was_in_mapping) {
    enum operation_ty ty = INVALID_OP;

    bool in_mapping = *was_in_mapping;
    while (true) {
        yaml_event_t event;
        int rv = yaml_parser_parse(parser, &event);
        if unlikely (rv == 0) {
            return parse_fail(NULL, NULL, NULL);
        }

        switch (event.type) {
            case YAML_SCALAR_EVENT:
                // The scalar event is presumably the operation name
                ty = parse_ty(event.data.scalar.value);
                yaml_event_delete(&event);

                if (in_mapping) {
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
                        case INVALID_OP:
                        default:
                            return parse_fail(NULL, NULL, NULL);
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
                        case INVALID_OP:
                        default:
                            return parse_fail(NULL, NULL, NULL);
                    }
                }

            case YAML_MAPPING_START_EVENT:
                yaml_event_delete(&event);
                if unlikely (in_mapping) {
                    return parse_fail(NULL, NULL, NULL);
                }
                in_mapping = true;
                *was_in_mapping = in_mapping;
                continue;

            case YAML_MAPPING_END_EVENT:
                if (!in_mapping) {
                    return parse_fail(NULL, NULL, NULL);
                }
                in_mapping = false;
                *was_in_mapping = in_mapping;
                continue;

            case YAML_STREAM_START_EVENT:
            case YAML_DOCUMENT_START_EVENT:
            case YAML_ALIAS_EVENT:
            case YAML_SEQUENCE_START_EVENT:
            case YAML_SEQUENCE_END_EVENT:
                // just consume & ignore
                yaml_event_delete(&event);
                continue;

            case YAML_NO_EVENT:
            case YAML_STREAM_END_EVENT:
            case YAML_DOCUMENT_END_EVENT:
            default:
                yaml_event_delete(&event);
                return parse_fail(NULL, NULL, NULL);
        }
    }
}
