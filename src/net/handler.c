
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "./handler.h"
#include "./parser.h"
#include "../database/database.h"

[[gnu::regcall]]
static bool handle_result(int sock_fd, message errmsg, db_result result) {
    if likely (result == DB_SUCCESS) {
        char ok[] = "server: ok\n";
        send(sock_fd, ok, strlen(ok), 0);
        return false;
    }

    if likely (errmsg != NULL) {
        char response[512];
        snprintf(response, 512, "server: %s\n", errmsg);
        send(sock_fd, response, strlen(response), 0);
        db_free_errmsg(errmsg);
    }

    return unlikely(result == DB_HARD_ERROR);
}

[[gnu::regcall]]
static bool send_movie(void *NONNULL sock_ptr, const struct movie *NULLABLE movie) {
    const int sock_fd = *(const int *) sock_ptr;

    if unlikely (movie == NULL) {
        char msg[] = "server: null\n";
        send(sock_fd, msg, strlen(msg), 0);
        return false;
    }

    char msg[1024] = "movie:\n";
    send(sock_fd, msg, strlen(msg), 0);
    snprintf(msg, 1024, "\tid: %" PRIi64 "\n", movie->id);
    send(sock_fd, msg, strlen(msg), 0);
    snprintf(msg, 1024, "\ttitle: %s\n", movie->title);
    send(sock_fd, msg, strlen(msg), 0);
    snprintf(msg, 1024, "\treleased in: %d\n", movie->release_year);
    send(sock_fd, msg, strlen(msg), 0);
    snprintf(msg, 1024, "\tdirector: %s\n", movie->director);
    send(sock_fd, msg, strlen(msg), 0);
    for (size_t i = 0; movie->genres[i] != NULL; i++) {
        snprintf(msg, 1024, "\tgenre[%zu]: %s\n", i, movie->genres[i]);
        send(sock_fd, msg, strlen(msg), 0);
    }
    send(sock_fd, "\n", strlen("\n"), 0);
    return false;
}

[[gnu::regcall]]
static bool send_summary(void *NONNULL sock_ptr, const struct movie_summary summary) {
    const int sock_fd = *(const int *) sock_ptr;

    char msg[1024];
    snprintf(msg, 1024, "movie[id=%" PRIi64 "]: %s\n", summary.id, summary.title);
    send(sock_fd, msg, strlen(msg), 0);
    return false;
}

bool handle_request(int sock_fd, db_conn *NONNULL db) {
    // parse
    yaml_parser_t parser;
    bool ok = parser_start(&parser, sock_fd);
    if unlikely (!ok) {
        const char msg[] = "server: failed to create YAML parser\n";
        send(sock_fd, msg, strlen(msg), 0);
        close(sock_fd);
        return false;
    }

    bool stop = false;
    bool hard_fail = false;

    bool in_mapping = false;
    while (!stop && !hard_fail) {
        struct operation op = parser_next_op(&parser, &in_mapping);
        if (op.ty == INVALID_OP) {
            stop = true;
            continue;
        }

        message errmsg = NULL;
        db_result result = DB_SUCCESS;
        switch (op.ty) {
            case ADD_MOVIE: {
                char response[256];
                snprintf(
                    response,
                    sizeof(response),
                    "server: received ADD_MOVIE: %s (%d), by %s\n",
                    op.movie->title,
                    op.movie->release_year,
                    op.movie->director
                );
                send(sock_fd, response, strlen(response), 0);
                fputs(response, stderr);

                result = db_register_movie(db, op.movie, &errmsg);
                for (size_t i = 0; op.movie->genres[i] != NULL; i++) {
                    free((char *) op.movie->genres[i]);
                }
                free((char *) op.movie->title);
                free((char *) op.movie->director);
                free(op.movie);
                break;
            }
            case ADD_GENRE: {
                char response[128];
                snprintf(
                    response,
                    sizeof(response),
                    "server: received ADD_GENRE: %s TO id[%" PRIi64 "]\n",
                    op.key.genre,
                    op.key.movie_id
                );
                send(sock_fd, response, strlen(response), 0);
                fputs(response, stderr);

                result = db_add_genres(db, op.key.movie_id, (const char *[2]) {op.key.genre, NULL}, &errmsg);
                free(op.key.genre);
                break;
            }
            case REMOVE_MOVIE: {
                char response[128];
                snprintf(
                    response,
                    sizeof(response),
                    "server: received REMOVE_MOVIE: id[%" PRIi64 "]\n",
                    op.key.movie_id
                );
                send(sock_fd, response, strlen(response), 0);
                fputs(response, stderr);

                result = db_delete_movie(db, op.key.movie_id, &errmsg);
                free(op.key.genre);
                break;
            }
            case GET_MOVIE: {
                char response[128];
                snprintf(response, sizeof(response), "server: received GET_MOVIE: id[%" PRIi64 "]\n", op.key.movie_id);
                send(sock_fd, response, strlen(response), 0);
                fputs(response, stderr);

                struct movie *movie = NULL;
                result = db_get_movie(db, op.key.movie_id, &movie, &errmsg);

                send_movie(&sock_fd, movie);
                free(movie);
                break;
            }
            case LIST_MOVIES: {
                char response[] = "server: received LIST_MOVIES\n";
                send(sock_fd, response, strlen(response), 0);
                fputs(response, stderr);

                result = db_list_movies(db, send_movie, &sock_fd, &errmsg);
                break;
            }
            case SEARCH_BY_GENRE: {
                char response[128];
                snprintf(response, sizeof(response), "server: received SEARCH_BY_GENRE: %s\n", op.key.genre);
                send(sock_fd, response, strlen(response), 0);
                fputs(response, stderr);

                result = db_search_movies_by_genre(db, op.key.genre, send_movie, &sock_fd, &errmsg);
            }
            case LIST_SUMMARIES: {
                char response[128];
                snprintf(response, sizeof(response), "server: received SEARCH_BY_GENRE: %s\n", op.key.genre);
                send(sock_fd, response, strlen(response), 0);
                fputs(response, stderr);

                result = db_list_summaries(db, send_summary, &sock_fd, &errmsg);
            }
            case INVALID_OP:
            default: {
                const char response[] = "server: received an unknown operation, stopping communication\n";
                send(sock_fd, response, strlen(response), 0);
                fputs(response, stderr);
                stop = true;
                break;
            }
        }

        hard_fail = handle_result(sock_fd, errmsg, result);
    }

    yaml_parser_delete(&parser);
    close(sock_fd);
    return hard_fail;
}
