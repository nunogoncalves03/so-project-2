#include "logging.h"
#include "operations.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MSG_MAX_SIZE 1024
#define PIPENAME_SIZE 256
#define BOX_NAME "/box"

void pub_fn(char *pub_pipe_path) {
    int pub_pipe_fd, box_fd;

    // Open the pub_pipe for reading
    if ((pub_pipe_fd = open(pub_pipe_path, O_RDONLY)) == -1) {
        fprintf(stderr, "[ERR] Open failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Open the box to write the messages
    if ((box_fd = tfs_open(BOX_NAME, TFS_O_APPEND)) == -1) {
        fprintf(stderr, "[ERR] tfs_open failed.\n");
        exit(EXIT_FAILURE);
    }

    ssize_t ret;
    char buffer[PIPENAME_SIZE]; // TODO: mudar valores para os do protocolo

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

        if (tfs_write(box_fd, buffer, (size_t)ret) < (size_t)ret) {
            fprintf(stderr, "[ERR]: tfs_write failed\n");
            exit(EXIT_FAILURE);
        }
    }

    if (tfs_close(box_fd) == -1) {
        fprintf(stderr, "[ERR] tfs_close failed.\n");
        exit(EXIT_FAILURE);
    }

    if (close(pub_pipe_fd) == -1) {
        fprintf(stderr, "[ERR]: Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

void sub_fn(char *sub_pipe_path) {
    int sub_pipe_fd, box_fd;

    // Open the sub_pipe for writing messages
    if ((sub_pipe_fd = open(sub_pipe_path, O_WRONLY)) == -1) {
        fprintf(stderr, "[ERR] Open failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Open the box to read the messages
    if ((box_fd = tfs_open(BOX_NAME, 0)) == -1) {
        fprintf(stderr, "[ERR] tfs_open failed.\n");
        exit(EXIT_FAILURE);
    }

    ssize_t ret;
    char buffer[MSG_MAX_SIZE]; // TODO: mudar valores para os do protocolo

    // Read the messages from the box
    // TODO: bloco apenas pode ter 1024, q é o max de uma msg
    if ((ret = tfs_read(box_fd, buffer, MSG_MAX_SIZE)) == -1) {
        fprintf(stderr, "[ERR]: tfs_read failed\n");
        exit(EXIT_FAILURE);
    }

    // Send the messages to sub
    // TODO: processar cada mensagem de cada vez
    if (write(sub_pipe_fd, buffer, (size_t)ret) < (size_t)ret) {
        fprintf(stderr, "[ERR] Write failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (tfs_close(box_fd) == -1) {
        fprintf(stderr, "[ERR] tfs_close failed.\n");
        exit(EXIT_FAILURE);
    }

    if (close(sub_pipe_fd) == -1) {
        fprintf(stderr, "[ERR]: Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

// argv[1] = register_pipe, argv[2] = max_sessions
int main(int argc, char **argv) {

    if (!strcmp(argv[1], "--help")) {
        printf("usage: ./mbroker <pipename> <max_sessions>\n");
        return 0;
    }

    if (argc != 3) {
        fprintf(stderr, "mbroker: Invalid arguments.\nTry './mbroker --help'"
                        " for more information.\n");
        exit(EXIT_FAILURE);
    }

    // Init the file system
    if (tfs_init(NULL) == -1) {
        fprintf(stderr, "[ERR] tfs_init failed.\n");
        exit(EXIT_FAILURE);
    }

    int box_fd;
    // Create the box
    if ((box_fd = tfs_open(BOX_NAME, TFS_O_CREAT)) == -1) {
        fprintf(stderr, "[ERR] tfs_open failed.\n");
        exit(EXIT_FAILURE);
    }

    if (tfs_close(box_fd) == -1) {
        fprintf(stderr, "[ERR] tfs_close failed.\n");
        exit(EXIT_FAILURE);
    }

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

    // Loop while dealing with clients
    while (1) {
        // Read the client pipe path, from where we will interact with him
        ssize_t ret = read(register_pipe_fd, buffer, PIPENAME_SIZE);

        if (ret == 0) {
            // ret == 0 indicates EOF
            fprintf(stderr, "[INFO]: pipe closed\n");

            // Close the register pipe
            if (close(register_pipe_fd) == -1) {
                fprintf(stderr, "[ERR]: Close failed: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }

            // Reopen it for reading
            if ((register_pipe_fd = open(argv[1], O_RDONLY)) == -1) {
                fprintf(stderr, "[ERR] Open failed: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }

            continue;
        } else if (ret == -1) {
            // ret == -1 indicates error
            fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        if (!strcmp(buffer, "/tmp/pub.pipe")) {
            // dealing with a pub
            pub_fn(buffer);
        } else if (!strcmp(buffer, "/tmp/sub.pipe")) {
            // dealing with a sub
            sub_fn(buffer);
        }
    }

    if (close(register_pipe_fd) == -1) {
        fprintf(stderr, "[ERR]: Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    return 0;
}
