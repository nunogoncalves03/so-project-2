#include "logging.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PIPENAME_SIZE 256
#define MSG_MAX_SIZE 1024
#define BOXNAME_SIZE 32
#define OPCODE_SIZE 1
#define REGISTRATION_SIZE OPCODE_SIZE + PIPENAME_SIZE + BOXNAME_SIZE
#define PUB_MSG_SIZE OPCODE_SIZE + MSG_MAX_SIZE

// argv[1] = register_pipe, argv[2] = pipe_name, argv[3] = box_name
int main(int argc, char **argv) {

    if (argc == 2 && !strcmp(argv[1], "--help")) {
        printf("usage: ./pub <register_pipe> <pipe_name> <box_name>\n");
        return 0;
    }

    if (argc != 4) {
        fprintf(stderr, "pub: Invalid arguments.\nTry './pub --help' for"
                        " more information.\n");
        exit(EXIT_FAILURE);
    }

    int register_pipe_fd, pub_pipe_fd;

    // Open the register pipe for writing
    if ((register_pipe_fd = open(argv[1], O_WRONLY)) == -1) {
        fprintf(stderr, "[ERR] Open failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Check if the pipe name is valid
    if (strlen(argv[2]) > PIPENAME_SIZE - 1) {
        fprintf(stderr, "[ERR] named_pipe name bigger than supported\n");
        exit(EXIT_FAILURE);
    }

    // Remove pub_pipe if it exists
    if (unlink(argv[2]) != 0 && errno != ENOENT) {
        fprintf(stderr, "[ERR]: unlink(%s) failed: %s\n", argv[2],
                strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Create pub_pipe, through which we will send messages to the mbroker
    if (mkfifo(argv[2], 0640) != 0) {
        fprintf(stderr, "[ERR] mkfifo failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Protocol */

    char registration[REGISTRATION_SIZE] = {0};

    // OP_CODE
    registration[0] = 1;

    // Pipe path
    strcpy(registration + OPCODE_SIZE, argv[2]);

    // Box name
    strcpy(registration + OPCODE_SIZE + PIPENAME_SIZE, argv[3]);

    // Send registration to mbroker
    if (write(register_pipe_fd, registration, REGISTRATION_SIZE) <
        REGISTRATION_SIZE) {
        fprintf(stderr, "[ERR] Write failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (close(register_pipe_fd) == -1) {
        fprintf(stderr, "[ERR] Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Open pub_pipe for writing messages
    if ((pub_pipe_fd = open(argv[2], O_WRONLY)) == -1) {
        fprintf(stderr, "[ERR] Open failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    char buffer[PUB_MSG_SIZE];
    buffer[0] = 9;
    char c = '\0';
    int i = 1;
    while (c != EOF) {
        c = (char)getchar();

        if (i < MSG_MAX_SIZE) {
            // the msg hasn't exceeded the max size (1024)
            if (c == '\n' || (c == EOF && i > 1)) {
                // we've reached the end of the msg, let's send it
                buffer[i] = '\0';
                i = 1;
                if (write(pub_pipe_fd, buffer, PUB_MSG_SIZE) < PUB_MSG_SIZE) {
                    fprintf(stderr, "[ERR] Write failed: %s\n",
                            strerror(errno));
                    exit(EXIT_FAILURE);
                }
            } else {
                buffer[i++] = c;
            }
        } else if (i == MSG_MAX_SIZE) {
            // the msg size is >= 1023, so we need to truncate
            buffer[i++] = '\0';
            if (write(pub_pipe_fd, buffer, PUB_MSG_SIZE) < PUB_MSG_SIZE) {
                fprintf(stderr, "[ERR] Write failed: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }

            // if the msg is exactly 1023 char long, we don't want to enter
            // the "else" condition and consume the next characters
            if (c == '\n' || c == EOF)
                i = 1;

        } else {
            // if the msg is exactly 1024 char long, we don't want to consume
            // the '\n' before checking the condition
            if (c != '\n' && c != EOF) {
                // consume the leftover characters
                while ((c = (char)getchar()) != '\n' && c != EOF)
                    ;
            }

            i = 1;
        }
    }

    if (close(pub_pipe_fd) == -1) {
        fprintf(stderr, "[ERR] Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // TODO: unlink named pipe

    return 0;
}
