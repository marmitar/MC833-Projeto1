#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>  // IWYU pragma: keep

#include "./database/database.h"
#include "./defines.h"
#include "./worker/worker.h"

[[gnu::cold]]
/** Set up server socket and start listening. */
static int start_server(void) {
    static constexpr const uint16_t PORT = 12'345;
    static constexpr const int BACKLOG = 32;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if unlikely (server_fd < 0) {
        perror("socket");
        return -1;
    }

    int yes = 1;
    int rv = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if unlikely (rv != 0) {
        perror("setsockopt");
        close(server_fd);
        return -1;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if unlikely (bind(server_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }
    if unlikely (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    printf("server listening on port %d\n", PORT);
    return server_fd;
}

extern int main(void) {
    // initialize sqlite
    const char *errmsg = NULL;
    bool setup_ok = db_setup(DATABASE, &errmsg);
    if unlikely (!setup_ok) {
        (void) fprintf(stderr, "db_setup: %s\n", errmsg);
        db_free_errmsg(errmsg);
        return EXIT_FAILURE;
    }

    // initialize worker threads
    setup_ok = workers_start();
    if unlikely (!setup_ok) {
        perror("workers_start");
        return EXIT_FAILURE;
    }

    // initialize socket
    const int server_fd = start_server();
    if unlikely (server_fd < 0) {
        workers_stop();
        return EXIT_FAILURE;
    }

    // start accepting
    while (likely(!was_shutdown_requested())) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *) &client_addr, &addrlen);
        if unlikely (client_fd < 0) {
            (void) fprintf(stderr, "main: accept failed: %s\n", strerrordesc_np(errno));
            continue;
        }

        static constexpr const size_t ADDR_LEN = 32;
        char address_str[ADDR_LEN] = "<unknown>";
        (void) inet_ntop(AF_INET, &(client_addr.sin_addr), address_str, ADDR_LEN);
        (void) fprintf(stderr, "main: client accepted: %s\n", address_str);

        static constexpr const struct timeval SOCKET_TIMEOUT = {.tv_sec = 60, .tv_usec = 0};
        int rv0 = setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &SOCKET_TIMEOUT, sizeof(SOCKET_TIMEOUT));
        int rv1 = setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &SOCKET_TIMEOUT, sizeof(SOCKET_TIMEOUT));
        if unlikely (rv0 != 0 || rv1 != 0) {
            (void) fprintf(stderr, "main: could not set timeout for %s, ending communications early\n", address_str);
            close(client_fd);
            continue;
        }

        static constexpr const unsigned MAX_RETRIES = 512;
        bool ok = workers_add_work(client_fd, MAX_RETRIES);
        if unlikely (!ok) {
            (void) fprintf(stderr, "main: no worker thread to handle %s, ignoring client\n", address_str);
            close(client_fd);
            continue;
        }
    }

    if likely (was_shutdown_requested()) {
        (void) fprintf(stderr, "main: shutdown requested\n");
    }
    close(server_fd);
    workers_stop();
    return EXIT_SUCCESS;
}
