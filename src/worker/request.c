#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../database/database.h"
#include "../defines.h"
#include "../movie/movie.h"
#include "../movie/parser.h"
#include "./request.h"

/** Display code in %hhu format. */
#define hhu(code) ((unsigned char) (code))

/** Response length. */
#define RESP_LEN 2048

[[nodiscard("hard errors cannot be ignored")]]
/**
 * Sends a debug response to the client based on `db_result` and `errmsg`.
 *
 * If result == DB_SUCCESS, sends an \"ok\" message. Otherwise, sends the error message and frees it. Returns whether
 * we encountered a hard error that might force the server to stop.
 *
 * @return true if DB_HARD_ERROR was encountered, false otherwise.
 */
static bool handle_result(unsigned long id, int sock_fd, const char *NULLABLE errmsg, db_result_t result) {
    if likely (errmsg != NULL) {
        char response[RESP_LEN];
        (void) snprintf(response, sizeof(response), "server: %s\n\n", errmsg);
        send(sock_fd, response, strlen(response), 0);
        (void) fprintf(stderr, "worker[%zu]: db error: %s\n", id, errmsg);
        db_free_errmsg(errmsg);
    }

    return unlikely(result == DB_HARD_ERROR);
}

[[gnu::hot]]
/** Sends ok to  */
static void send_ok(int sock_fd) {
    char ok[] = "server: ok\n\n";
    send(sock_fd, ok, strlen(ok), 0);
}

[[gnu::hot]]
/**
 * Sends textual movie data back to the client.
 *
 * Formats the fields of `movie` and writes them to the socket. Continues returning false so iteration can keep going,
 * unless you want to stop after the first record.
 */
static void send_movie(int sock_fd, struct movie movie, bool in_list) {
    // Ideally, we should be a single in-memory buffer and send a single data to the client, as to
    // - not clog the SQLite database lock
    // - allow faster communication by the kernel
    // - allow proper uring integration
    char msg[RESP_LEN] = "";
    if (!in_list) {
        (void) snprintf(msg, sizeof(msg), "movie:\n");
        send(sock_fd, msg, strlen(msg), 0);
    }

    (void) snprintf(msg, sizeof(msg), "  %2sid: %" PRIi64 "\n", in_list ? "- " : "", movie.id);
    send(sock_fd, msg, strlen(msg), 0);
    (void) snprintf(msg, sizeof(msg), "  %2stitle: %s\n", in_list ? "  " : "", movie.title);
    send(sock_fd, msg, strlen(msg), 0);
    (void) snprintf(msg, sizeof(msg), "  %2srelease_year: %d\n", in_list ? "  " : "", movie.release_year);
    send(sock_fd, msg, strlen(msg), 0);
    (void) snprintf(msg, sizeof(msg), "  %2sdirector: %s\n", in_list ? "  " : "", movie.director);
    send(sock_fd, msg, strlen(msg), 0);
    if unlikely (movie.genre_count <= 0) {
        (void) snprintf(msg, sizeof(msg), "  %2sgenres: []\n", in_list ? "  " : "");
        send(sock_fd, msg, strlen(msg), 0);
    } else {
        (void) snprintf(msg, sizeof(msg), "  %2sgenres:\n", in_list ? "  " : "");
        send(sock_fd, msg, strlen(msg), 0);
    }
    for (size_t i = 0; i < movie.genre_count; i++) {
        (void) snprintf(msg, sizeof(msg), "  %2s  - %s\n", in_list ? "  " : "", movie.genres[i]);
        send(sock_fd, msg, strlen(msg), 0);
    }
    send(sock_fd, "\n", strlen("\n"), 0);
    free_movie(movie);
}

[[gnu::hot, gnu::nonnull(3, 4)]]
/** Sends multiple movies at once. */
static void send_movie_list(int sock_fd, size_t count, struct movie movie[NONNULL count], const char *NONNULL key) {
    char msg[RESP_LEN] = "";
    (void) snprintf(msg, sizeof(msg), "---\n%s:\n\n", key);
    send(sock_fd, msg, strlen(msg), 0);

    for (size_t i = 0; i < count; i++) {
        send_movie(sock_fd, movie[i], true);
    }
    free(movie);

    const char END_DOCUMENT[] = "...\n";
    send(sock_fd, END_DOCUMENT, strlen(END_DOCUMENT), 0);
}

[[gnu::hot, gnu::nonnull(3)]]
/** Sends multiple summaries at once. */
static void send_summary_list(int sock_fd, size_t count, struct movie_summary summary[NONNULL count]) {
    char msg[RESP_LEN] = "";
    (void) snprintf(msg, sizeof(msg), "---\n%s:\n", "summaries");
    send(sock_fd, msg, strlen(msg), 0);

    for (size_t i = 0; i < count; i++) {
        (void) snprintf(msg, sizeof(msg), "  - { id: %" PRIi64 ", title: '%s' }\n", summary[i].id, summary[i].title);
        send(sock_fd, msg, strlen(msg), 0);
    }
    free(summary);

    const char END_DOCUMENT[] = "...\n";
    send(sock_fd, END_DOCUMENT, strlen(END_DOCUMENT), 0);
}

/** Length for an IP text representation. */
#define MAX_IP_LEN 32

/** String representation of an IP address. */
struct [[gnu::aligned(MAX_IP_LEN)]] ip_string {
    char ip[MAX_IP_LEN];
};

/** Writes the client IP in human readable format. */
static struct ip_string get_peer_ip(int sock_fd) {
    static constexpr const struct ip_string UNKNOWN = {.ip = "<unknown>"};

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = INADDR_ANY,
    };
    socklen_t sock_len = sizeof(addr);

    int rv = getpeername(sock_fd, &addr, &sock_len);
    if unlikely (rv != 0) {
        return UNKNOWN;
    }

    struct ip_string str = {.ip = ""};
    const char *p = inet_ntop(AF_INET, &(addr.sin_addr), str.ip, MAX_IP_LEN);
    if unlikely (p == NULL) {
        return UNKNOWN;
    }

    assert(strlen(str.ip) < MAX_IP_LEN);
    return str;
}

/**
 * Main function to handle all YAML-based requests on a single client socket.
 *
 * Uses parser_start() to read YAML operations, dispatches to appropriate db_* calls,
 * and sends textual responses. Closes the socket at the end.
 *
 * @param sock_fd The socket file descriptor for this client.
 * @param db      A non-null pointer to the database connection.
 * @return true if request was handled successfully, or false if a hard error was encountered (server might stop).
 */
bool handle_request(size_t id, int sock_fd, db_conn_t *NONNULL db, atomic_bool *NONNULL shutdown_requested) {
    (void) fprintf(stderr, "worker[%zu]: handling socket %d, peer ip %s\n", id, sock_fd, get_peer_ip(sock_fd).ip);

    parser_t *parser = parser_create(shutdown_requested, sock_fd);
    if unlikely (parser == NULL) {
        const char msg[] = "server: failed to create YAML parser\n\n";
        send(sock_fd, msg, strlen(msg), 0);
        close(sock_fd);
        return false;
    }

    bool hard_fail = false;
    while (!parser_finished(parser) && !hard_fail) {
        struct operation op = parser_next_op(parser);

        const char *errmsg = NULL;
        db_result_t result;
        switch (op.ty) {
            case PARSE_DONE: {
                result = DB_SUCCESS;
                break;
            }
            case ADD_MOVIE: {
                char response[RESP_LEN] = "\n";
                (void) snprintf(
                    response,
                    sizeof(response),
                    "server: received ADD_MOVIE: %s (%d), by %s\n",
                    op.movie.title,
                    op.movie.release_year,
                    op.movie.director
                );
                send(sock_fd, response, strlen(response), 0);

                result = db_register_movie(db, &(op.movie), &errmsg);
                free_movie(op.movie);
                if likely (result == DB_SUCCESS) {
                    send_ok(sock_fd);
                }
                break;
            }
            case ADD_GENRE: {
                char response[RESP_LEN] = "\n";
                (void) snprintf(
                    response,
                    sizeof(response),
                    "server: received ADD_GENRE: %s TO id[%" PRIi64 "]\n",
                    op.key.genre,
                    op.key.movie_id
                );
                send(sock_fd, response, strlen(response), 0);

                result = db_add_genre(db, op.key.movie_id, op.key.genre, &errmsg);
                if likely (result == DB_SUCCESS) {
                    send_ok(sock_fd);
                }
                break;
            }
            case REMOVE_MOVIE: {
                char response[RESP_LEN] = "\n";
                (void) snprintf(
                    response,
                    sizeof(response),
                    "server: received REMOVE_MOVIE: id[%" PRIi64 "]\n",
                    op.key.movie_id
                );
                send(sock_fd, response, strlen(response), 0);

                result = db_delete_movie(db, op.key.movie_id, &errmsg);
                if likely (result == DB_SUCCESS) {
                    send_ok(sock_fd);
                }
                break;
            }
            case GET_MOVIE: {
                char response[RESP_LEN] = "\n";
                (void) snprintf(
                    response,
                    sizeof(response),
                    "server: received GET_MOVIE: id[%" PRIi64 "]\n",
                    op.key.movie_id
                );
                send(sock_fd, response, strlen(response), 0);

                struct movie movie;
                result = db_get_movie(db, op.key.movie_id, &movie, &errmsg);
                if likely (result == DB_SUCCESS) {
                    send_movie(sock_fd, movie, false);
                }
                break;
            }
            case LIST_MOVIES: {
                char response[RESP_LEN] = "server: received LIST_MOVIES\n";
                send(sock_fd, response, strlen(response), 0);

                size_t list_size;
                struct movie *list;
                result = db_list_movies(db, &list, &list_size, &errmsg);
                if likely (result == DB_SUCCESS) {
                    send_movie_list(sock_fd, list_size, list, "movies");
                }
                break;
            }
            case SEARCH_BY_GENRE: {
                char response[RESP_LEN] = "\n";
                (void) snprintf(response, sizeof(response), "server: received SEARCH_BY_GENRE: %s\n", op.key.genre);
                send(sock_fd, response, strlen(response), 0);

                size_t list_size;
                struct movie *list;
                result = db_search_movies_by_genre(db, op.key.genre, &list, &list_size, &errmsg);
                if likely (result == DB_SUCCESS) {
                    send_movie_list(sock_fd, list_size, list, "selected_movies");
                }
                break;
            }
            case LIST_SUMMARIES: {
                char response[RESP_LEN] = "\n";
                (void) snprintf(response, sizeof(response), "server: received LIST_SUMMARIES\n");
                send(sock_fd, response, strlen(response), 0);

                size_t list_size;
                struct movie_summary *list;
                result = db_list_summaries(db, &list, &list_size, &errmsg);
                if likely (result == DB_SUCCESS) {
                    send_summary_list(sock_fd, list_size, list);
                }
                break;
            }
            case PARSE_ERROR: {
                char response[RESP_LEN] = "\n";
                (void) snprintf(response, sizeof(response), "server: parsing error: %s\n\n", op.error_message);
                send(sock_fd, response, strlen(response), 0);

                result = DB_SUCCESS;
                break;
            }
            default: {
                const char response[RESP_LEN] = "server: unexpected error\n\n";
                send(sock_fd, response, strlen(response), 0);

                result = DB_SUCCESS;
                break;
            }
        }

        hard_fail = handle_result(id, sock_fd, errmsg, result);
        (void) fprintf(
            stderr,
            "worker[%zu]: op.ty=%hhu, finished=%hhu, hard_fail=%hhu, result=%hhu\n",
            id,
            hhu(op.ty),
            hhu(parser_finished(parser)),
            hhu(hard_fail),
            hhu(result)
        );
    }

    parser_destroy(parser);
    close(sock_fd);
    return !hard_fail;
}
