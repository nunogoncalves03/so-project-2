#include "common.h"
#include "logging.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Variable to know whether subscriber should be shutdown */
static int shutdown_subscriber = 0;

void sigint_handler() { shutdown_subscriber = 1; }

// argv[1] = register_pipe, argv[2] = pipe_name, argv[3] = box_name
int main(int argc, char **argv) {

    if (signal(SIGINT, sigint_handler) == SIG_ERR) {
        PANIC("couldn't set signal handler")
    }

    if (argc == 2 && !strcmp(argv[1], "--help")) {
        printf("usage: ./sub <register_pipe> <pipe_name> <box_name>\n");
        return 0;
    }

    if (argc != 4) {
        fprintf(stderr, "sub: Invalid arguments.\nTry './sub --help' for"
                        " more information.\n");
        exit(EXIT_FAILURE);
    }

    // Check if the pipe name is valid
    if (strlen(argv[2]) > PIPENAME_SIZE - 1) {
        fprintf(stderr, "named_pipe name bigger than supported (%d chars)\n",
                PIPENAME_SIZE - 1);
        exit(EXIT_FAILURE);
    }

    // Check if the box name is valid
    if (strlen(argv[3]) > BOXNAME_SIZE - 1) {
        fprintf(stderr, "box_name bigger than supported (%d chars)\n",
                BOXNAME_SIZE - 1);
        exit(EXIT_FAILURE);
    }

    int register_pipe_fd, sub_pipe_fd;

    // Open the register pipe for writing
    if ((register_pipe_fd = open(argv[1], O_WRONLY)) == -1) {
        PANIC("open failed: %s", strerror(errno))
    }

    // Remove sub_pipe if it exists
    if (unlink(argv[2]) != 0 && errno != ENOENT) {
        PANIC("unlink(%s) failed: %s", argv[2], strerror(errno))
    }

    // Create sub_pipe, through which we will read messages from the mbroker
    if (mkfifo(argv[2], 0640) != 0) {
        PANIC("mkfifo failed: %s", strerror(errno))
    }

    /* Protocol */

    char registration[REGISTRATION_SIZE] = {0};

    // OP_CODE
    registration[0] = OPCODE_SUB_REG;

    // Pipe path
    strcpy(registration + OPCODE_SIZE, argv[2]);

    // Box name
    strcpy(registration + OPCODE_SIZE + PIPENAME_SIZE, argv[3]);

    // Send registration to mbroker
    if (write(register_pipe_fd, registration, REGISTRATION_SIZE) <
        REGISTRATION_SIZE) {
        PANIC("write failed: %s", strerror(errno))
    }

    if (close(register_pipe_fd) == -1) {
        PANIC("close failed: %s", strerror(errno))
    }

    // Open sub_pipe for reading messages
    if ((sub_pipe_fd = open(argv[2], O_RDONLY)) == -1) {
        // If open is interrupted by a signal, check if it's a SIGINT
        if (shutdown_subscriber) {
            // Remove sub_pipe
            if (unlink(argv[2]) != 0 && errno != ENOENT) {
                PANIC("unlink(%s) failed: %s", argv[2], strerror(errno))
            }
            return 0;
        }
        // otherwise, PANIC
        PANIC("open failed: %s", strerror(errno))
    }

    char buffer[PUB_MSG_SIZE];
    ssize_t ret;
    uint16_t msg_counter = 0;
    // Read incoming messages until mbroker closes the pipe
    while (1) {
        ret = read(sub_pipe_fd, buffer, PUB_MSG_SIZE);

        if (ret == 0 || shutdown_subscriber) {
            // ret == 0 indicates EOF, mbroker closed the pipe
            // shutdown_subscriber indicates SIGINT
            // print the number of messages received
            printf(shutdown_subscriber ? "\n%d\n" : "%d\n", msg_counter);
            if (!shutdown_subscriber)
                INFO("mbroker forced the end of the session")
            break;
        } else if (ret == -1) {
            // ret == -1 indicates error
            PANIC("read failed: %s", strerror(errno))
        }

        // Verify code
        if (buffer[0] != OPCODE_SUB_MSG) {
            PANIC("Internal error: Invalid OP_CODE!")
        }

        msg_counter++;
        fprintf(stdout, "%s\n", buffer + OPCODE_SIZE);
    }

    if (close(sub_pipe_fd) == -1) {
        PANIC("close failed: %s", strerror(errno))
    }

    // Remove the sub_pipe
    if (unlink(argv[2]) != 0 && errno != ENOENT) {
        PANIC("unlink(%s) failed: %s", argv[2], strerror(errno))
    }

    return 0;
}
