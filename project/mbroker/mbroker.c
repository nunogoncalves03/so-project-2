#include "logging.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PIPENAME_SIZE 256

// argv[1] = register_pipe, argv[2] = max_sessions
int main(int argc, char **argv) {
    (void)argc;
    int register_pipe_fd;

    // Remove pipe if it exists
    if (unlink(argv[1]) != 0 && errno != ENOENT) {
        fprintf(stderr, "[ERR]: unlink(%s) failed: %s\n", argv[1],
                strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Create the register_pipe
    if (mkfifo(argv[1], 0640) != 0) {
        fprintf(stderr, "[ERR] mkfifo failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Open it for reading
    if ((register_pipe_fd = open(argv[1], O_RDONLY)) == -1) {
        fprintf(stderr, "[ERR] Open failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    char buffer[PIPENAME_SIZE]; // TODO: mudar valores para os do protocolo

    // Read the pub_pipe path, from where we will be receiving messages
    ssize_t ret = read(register_pipe_fd, buffer, PIPENAME_SIZE);

    if (ret == 0) {
        // ret == 0 indicates EOF
        fprintf(stderr, "[INFO]: pipe closed\n");
    } else if (ret == -1) {
        // ret == -1 indicates error
        fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (close(register_pipe_fd) == -1) {
        fprintf(stderr, "[ERR]: Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    int pub_pipe_fd;
    // Open the pub_pipe for reading
    if ((pub_pipe_fd = open(buffer, O_RDONLY)) == -1) {
        fprintf(stderr, "[ERR] Open failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Read incoming messages until pub closes its pipe
    while (1) {
        ret = read(pub_pipe_fd, buffer, PIPENAME_SIZE);

        if (ret == 0) {
            // ret == 0 indicates EOF
            break;
        } else if (ret == -1) {
            // ret == -1 indicates error
            fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    if (close(pub_pipe_fd) == -1) {
        fprintf(stderr, "[ERR]: Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    return 0;
}
