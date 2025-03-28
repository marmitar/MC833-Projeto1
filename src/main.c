#include "sqlite.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

const uint16_t SERV_PORT = 8080;

static void str_echo(int fd) {}

int main(int argc, char **argv) {
    struct sockaddr_in servaddr;
    const int sock_fd = socket(PF_INET, SOCK_STREAM, 0);
    memset(&servaddr, 0, sizeof(servaddr));

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(SERV_PORT);
    bind(sock_fd, (struct sockaddr *) &servaddr, sizeof(servaddr));
    listen(sock_fd, 0);

    while (true) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        const int new_fd = accept(sock_fd, (struct sockaddr *) &cliaddr, &clilen);

        const pid_t childpid = fork();
        if (childpid == 0) {
            /* child process */
            close(sock_fd);
            str_echo(new_fd);
            exit(0);
        }

        close(new_fd);
    }

    return EXIT_SUCCESS;
}
