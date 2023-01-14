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

/* Variable to know whether publisher should be shutdown */
static int shutdown_publisher = 0;

void sigint_handler() { shutdown_publisher = 1; }

// argv[1] = register_pipe, argv[2] = pipe_name, argv[3] = box_name
int main(int argc, char **argv) {

    if (signal(SIGINT, sigint_handler) == SIG_ERR) {
        PANIC("couldn't set signal handler")
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        PANIC("couldn't set signal handler")
    }

    if (argc == 2 && !strcmp(argv[1], "--help")) {
        printf("usage: ./pub <register_pipe> <pipe_name> <box_name>\n");
        return 0;
    }

    if (argc != 4) {
        fprintf(stderr, "pub: Invalid arguments.\nTry './pub --help' for"
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

    int register_pipe_fd, pub_pipe_fd;

    // Open the register pipe for writing
    if ((register_pipe_fd = open(argv[1], O_WRONLY)) == -1) {
        PANIC("open failed: %s", strerror(errno))
    }

    // Remove pub_pipe if it exists
    if (unlink(argv[2]) != 0 && errno != ENOENT) {
        PANIC("unlink(%s) failed: %s", argv[2], strerror(errno))
    }

    // Create pub_pipe, through which we will send messages to the mbroker
    if (mkfifo(argv[2], 0640) != 0) {
        PANIC("mkfifo failed: %s", strerror(errno))
    }

    /* Protocol */

    char registration[REGISTRATION_SIZE] = {0};

    // OP_CODE
    registration[0] = OPCODE_PUB_REG;

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

    // Open pub_pipe for writing messages
    if ((pub_pipe_fd = open(argv[2], O_WRONLY)) == -1) {
        // If open is interrupted by a signal, check if it's a SIGINT
        if (shutdown_publisher) {
            // Remove pub_pipe
            if (unlink(argv[2]) != 0 && errno != ENOENT) {
                PANIC("unlink(%s) failed: %s", argv[2], strerror(errno))
            }
            return 0;
        }
        // otherwise, PANIC
        PANIC("open failed: %s", strerror(errno))
    }

    char buffer[PUB_MSG_SIZE];
    buffer[0] = OPCODE_PUB_MSG; // OP_CODE
    char c = '\0';
    int i = 1;
    while (c != EOF) {
        c = (char)getchar();

        if (shutdown_publisher) {
            printf("\n"); // Print a newline after ^C
            break;
        }

        if (i < MSG_MAX_SIZE) {
            // the msg hasn't exceeded the max size (1024)
            if ((c == '\n' || c == EOF) && i > 1) {
                // we've reached the end of the msg, let's send it
                buffer[i] = '\0';
                i = 1;
                if (write(pub_pipe_fd, buffer, PUB_MSG_SIZE) < PUB_MSG_SIZE) {
                    if (errno == EPIPE) {
                        INFO("mbroker forced the end of the session")
                        break;
                    } else {
                        PANIC("write failed: %s", strerror(errno))
                    }
                }
            } else if (c != '\n' && c != EOF) {
                buffer[i++] = c;
            }
        } else if (i == MSG_MAX_SIZE) {
            // the msg size is >= 1023, so we need to truncate
            buffer[i++] = '\0';
            if (write(pub_pipe_fd, buffer, PUB_MSG_SIZE) < PUB_MSG_SIZE) {
                if (errno == EPIPE) {
                    INFO("mbroker forced the end of the session")
                    break;
                } else {
                    PANIC("write failed: %s", strerror(errno))
                }
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
        PANIC("close failed: %s", strerror(errno))
    }

    // Remove pub_pipe
    if (unlink(argv[2]) != 0 && errno != ENOENT) {
        PANIC("unlink(%s) failed: %s", argv[2], strerror(errno))
    }

    return 0;
}
