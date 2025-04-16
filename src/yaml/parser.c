#include "../defines.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <yaml.h>

#include "./parser.h"
#include "../database/database.h"

#define PTR_FROM_INT(x) ((void *)(intptr_t)(x))
#define INT_FROM_PTR(p) ((int)(intptr_t)(p))

static int fd_read_handler(
    void *data,
    unsigned char *NONNULL buffer,
    size_t size,
    size_t *NONNULL size_read
) {
    const int fd = INT_FROM_PTR(data);
    ssize_t rv = read(fd, buffer, size);
    if unlikely (rv < 0) {
        *size_read = 0;
        return 0;
    } else {
        *size_read = (size_t) rv;
        return 1;
    }
}

bool parser_start(yaml_parser_t *NONNULL parser, const int fd) {
    int rv = yaml_parser_initialize(parser);
    if unlikely(rv == 0) {
        return false;
    }

    void *data = PTR_FROM_INT(fd); // NOLINT(performance-no-int-to-ptr)
    yaml_parser_set_encoding(parser, YAML_UTF8_ENCODING);
    yaml_parser_set_input(parser, fd_read_handler, data);
    return true;
}

static inline bool streq(const yaml_char_t *NONNULL restrict key, const char *expected) {
    return strcmp((const char *) key, expected) == 0;
}

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

[[gnu::regcall, gnu::pure, gnu::nonnull(1)]]
/** Calculate the size of a NULL terminated list of strings. */
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

enum current_key {
    NONE,
    ID_KEY,
    TITLE_KEY,
    GENRE_KEY,
    DIRECTOR_KEY,
    YEAR_KEY,
    OTHER_KEY,
};

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
    return (struct operation) { .ty = INVALID_OP };
}

static char *NULLABLE *NULLABLE parse_genre_list(yaml_parser_t *NONNULL parser) {
    size_t capacity = 8;
    char *NULLABLE *genres = (char **) malloc(capacity * sizeof(char *));
    if unlikely(genres == NULL) {
        return NULL;
    }

    size_t len = 0;
    genres[len] = NULL;

    bool in_list = false;
    while (true) {
        yaml_event_t event;
        int rv = yaml_parser_parse(parser, &event);
        if unlikely(rv == 0) {
            parse_fail(NULL, NULL, genres);
            return NULL;
        }

        switch (event.type) {
            case YAML_SCALAR_EVENT:
                if unlikely(!in_list) {
                    yaml_event_delete(&event);
                    parse_fail(NULL, NULL, genres);
                    return NULL;
                }

                if unlikely(len >= capacity - 1) {
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

                if unlikely(genres[len] == NULL) {
                    parse_fail(NULL, NULL, genres);
                    return NULL;
                }
                genres[++len] = NULL;
                continue;
            case YAML_SEQUENCE_START_EVENT:
                yaml_event_delete(&event);

                if unlikely(in_list) {
                    parse_fail(NULL, NULL, genres);
                    return NULL;
                }
                in_list = true;
                continue;
            case YAML_SEQUENCE_END_EVENT:
                yaml_event_delete(&event);
                return genres;
            case YAML_NO_EVENT:
            case YAML_ALIAS_EVENT:
                yaml_event_delete(&event);
                continue;
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
};

static struct operation parse_movie(
    yaml_parser_t *NONNULL parser,
    enum operation_ty ty
) {
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
        if unlikely(rv == 0) {
            return parse_fail(title, director, genres);
        }

        switch (event.type) {
            case YAML_SCALAR_EVENT:
                switch (key) {
                    case NONE:
                        if (in_mapping) {
                            key = parse_key(event.data.scalar.value);
                            yaml_event_delete(&event);

                            if (key == GENRE_KEY) {
                                key = NONE;

                                if (genres == NULL) {
                                    genres = parse_genre_list(parser);
                                    if unlikely(genres == NULL) {
                                        return parse_fail(title, director, genres);
                                    }
                                } else {
                                    char **temp = parse_genre_list(parser);
                                    if (temp != NULL) {
                                        parse_fail(NULL, NULL, temp);
                                    }
                                }
                            }
                            continue;
                        } else {
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
            case YAML_NO_EVENT:
            case YAML_ALIAS_EVENT:
                yaml_event_delete(&event);
                continue;
            case YAML_MAPPING_START_EVENT:
                yaml_event_delete(&event);
                if unlikely(in_mapping) {
                    return parse_fail(title, director, genres);
                }
                in_mapping = true;
                continue;
            case YAML_MAPPING_END_EVENT:
                yaml_event_delete(&event);
                if unlikely(title == NULL || director == NULL || needs_year || genres == NULL) {
                    return parse_fail(title, director, genres);
                } else {
                    size_t len = list_len((const char *const *) genres);
                    struct movie *movie = malloc(sizeof(struct movie) + (len + 1) * sizeof(char *));
                    if unlikely(movie == NULL){
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

                    return (struct operation) {
                        .ty = ty,
                        .movie = movie
                    };
                }
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

static struct operation parse_movie_key(
    yaml_parser_t *NONNULL parser,
    enum operation_ty ty,
    bool needs_id,
    bool needs_genre
) {
    char *genre = NULL;
    int64_t id = 0;

    bool in_mapping = false;
    enum current_key key = NONE;
    while (true) {
        yaml_event_t event;
        int rv = yaml_parser_parse(parser, &event);
        if unlikely(rv == 0) {
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
                            bool ok = parse_i64((const char *) event.data.scalar.value, &id);
                            yaml_event_delete(&event);

                            if unlikely (!ok) {
                                return parse_fail(genre, NULL, NULL);
                            }
                            return (struct operation) {
                                .ty = ty,
                                .key = (struct movie_key) { .movie_id = id, .genre = genre }
                            };
                        } else if (!needs_id && needs_genre) {
                            genre = strdup((const char *) event.data.scalar.value);
                            yaml_event_delete(&event);

                            if unlikely (genre == NULL) {
                                return parse_fail(genre, NULL, NULL);
                            }
                            return (struct operation) {
                                .ty = ty,
                                .key = (struct movie_key) { .movie_id = id, .genre = genre }
                            };
                        } else {
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
            case YAML_NO_EVENT:
            case YAML_ALIAS_EVENT:
                yaml_event_delete(&event);
                continue;
            case YAML_MAPPING_START_EVENT:
                yaml_event_delete(&event);

                if unlikely(in_mapping) {
                    return parse_fail(genre, NULL, NULL);
                }
                in_mapping = true;
                continue;
            case YAML_MAPPING_END_EVENT:
                yaml_event_delete(&event);

                if unlikely(!in_mapping || needs_id || needs_genre) {
                    return parse_fail(genre, NULL, NULL);
                }

                return (struct operation) {
                    .ty = ty,
                    .key = (struct movie_key) { .movie_id = id, .genre = genre }
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

struct operation parser_next_op(yaml_parser_t *NONNULL parser, bool *NONNULL was_in_mapping) {
    enum operation_ty ty = INVALID_OP;

    bool in_mapping = *was_in_mapping;
    while (true) {
        yaml_event_t event;
        int rv = yaml_parser_parse(parser, &event);
        if unlikely(rv == 0) {
            return parse_fail(NULL, NULL, NULL);
        }

        switch (event.type) {
            case YAML_SCALAR_EVENT:
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
                    switch (ty) {
                        case LIST_SUMMARIES:
                        case LIST_MOVIES:
                            return (struct operation) { .ty = ty };
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
                if unlikely(in_mapping) {
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
            case YAML_NO_EVENT:
            case YAML_STREAM_START_EVENT:
            case YAML_DOCUMENT_START_EVENT:
            case YAML_ALIAS_EVENT:
            case YAML_SEQUENCE_START_EVENT:
            case YAML_SEQUENCE_END_EVENT:
                yaml_event_delete(&event);
                continue;
            case YAML_STREAM_END_EVENT:
            case YAML_DOCUMENT_END_EVENT:
            default:
                yaml_event_delete(&event);
                return parse_fail(NULL, NULL, NULL);
        }
    }
}
