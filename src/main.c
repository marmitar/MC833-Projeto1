#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "yaml/parser.h"

#define PORT 12345
#define BACKLOG 5
#define BUFFER_SIZE 4096

int main(int argc, const char *NONNULL const argv[NONNULL]) {
    if unlikely(argc != 2) {
        fprintf(stderr, "usage: %s <yamlfile>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const int fd = open(argv[1], 0);
    if unlikely(fd < 0) {
        perror("open");
    }

    yaml_parser_t parser;
    bool ok = parser_start(&parser, fd);
    if unlikely(!ok) {
        fprintf(stderr, "failed to create YAML parser\n");
        close(fd);
        return EXIT_FAILURE;
    }

    bool in_mapping = false;
    while (true) {
        struct operation op = parser_next_op(&parser, &in_mapping);
        switch (op.ty) {
            case ADD_MOVIE:
                printf("op ty%i: id=%" PRIi64 ", title=%s, director=%s, release_year=%d\n",
                    op.ty, op.movie->title, op.movie->title, op.movie->director, op.movie->release_year);

                for (size_t i = 0; op.movie->genres[i] != NULL; i++) {
                    printf("\tgenre[%zu]=%s\n", i, op.movie->genres[i]);
                    free((char *) op.movie->genres[i]);
                }
                free((char *) op.movie->title);
                free((char *) op.movie->director);
                free(op.movie);
                continue;
            case ADD_GENRE:
            case REMOVE_MOVIE:
            case GET_MOVIE:
            case SEARCH_BY_GENRE:
                printf("op ty%i: id=%" PRIi64 ", genre=%s\n", op.ty, op.key.movie_id, op.key.genre);
                free(op.key.genre);
                continue;
            case LIST_SUMMARIES:
            case LIST_MOVIES:
                printf("op ty%i\n", op.ty);
                continue;
            case INVALID_OP:
            default:
                yaml_parser_delete(&parser);
                close(fd);
                return EXIT_SUCCESS;
        }
    }
}
