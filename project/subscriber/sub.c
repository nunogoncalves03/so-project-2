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

    // TODO: Processar CTRL + C
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        printf("usage: ./sub <register_pipe> <pipe_name> <box_name>\n");
        return 0;
    }

    if (argc != 4) {
        fprintf(stderr, "sub: Invalid arguments.\nTry './sub --help' for"
                        " more information.\n");
        exit(EXIT_FAILURE);
    }

    int register_pipe_fd, sub_pipe_fd;

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

    // Remove sub_pipe if it exists
    if (unlink(argv[2]) != 0 && errno != ENOENT) {
        fprintf(stderr, "[ERR]: unlink(%s) failed: %s\n", argv[2],
                strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Create sub_pipe, through which we will read messages from the mbroker
    if (mkfifo(argv[2], 0640) != 0) {
        fprintf(stderr, "[ERR] mkfifo failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Protocol */

    char registration[REGISTRATION_SIZE] = {0};

    // OP_CODE
    registration[0] = 2;

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

    // Open sub_pipe for reading messages
    if ((sub_pipe_fd = open(argv[2], O_RDONLY)) == -1) {
        fprintf(stderr, "[ERR] Open failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    char buffer[PUB_MSG_SIZE];
    ssize_t ret;
    int msg_counter = 0;
    // Read incoming messages until mbroker closes the pipe
    while (1) {
        ret = read(sub_pipe_fd, buffer, PUB_MSG_SIZE);

        if (ret == 0) {
            // ret == 0 indicates EOF
            // print the number of messages received
            printf("%d\n", msg_counter);
            break;
        } else if (ret == -1) {
            // ret == -1 indicates error
            fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        // Verify code
        if (buffer[0] != 10) {
            fprintf(stderr, "Internal error: Invalid OP_CODE!\n");
            exit(EXIT_FAILURE);
        }

        msg_counter++;
        fprintf(stdout, "%s\n", buffer + OPCODE_SIZE);
    }

    // TODO: devemos fechar?
    if (close(sub_pipe_fd) == -1) {
        fprintf(stderr, "[ERR] Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // TODO: unlink named pipe

    return 0;
}
