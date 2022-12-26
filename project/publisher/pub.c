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

// argv[1] = register_pipe, argv[2] = pipe_name, argv[3] = box_name
int main(int argc, char **argv) {
    (void)argc;
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

    // Fill the remaining space of the path with the null character
    char pipe_path[PIPENAME_SIZE] = {0};
    strcpy(pipe_path, argv[2]);

    // Send the path to pub_pipe to mbroker
    if (write(register_pipe_fd, pipe_path, PIPENAME_SIZE) < 0) {
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

    char buffer[MSG_MAX_SIZE];
    char c = '\0';
    int i = 0;
    while (c != EOF) {
        c = (char)getchar();

        if (i < 5) {
            // the msg hasn't exceeded the max size (1024)
            if (c == '\n' || (c == EOF && i > 0)) {
                buffer[i] = '\0';
                i = 0;
                // TODO:send msg to broker
                if (write(pub_pipe_fd, buffer, strlen(buffer) + 1) < 0) {
                    fprintf(stderr, "[ERR] Write failed: %s\n",
                            strerror(errno));
                    exit(EXIT_FAILURE);
                }
            } else {
                buffer[i++] = c;
            }
        } else if (i == 5) {
            // the msg size is >= 1024, so we need to truncate
            buffer[i++] = '\0';
            // TODO:send msg to broker
            if (write(pub_pipe_fd, buffer, strlen(buffer) + 1) < 0) {
                fprintf(stderr, "[ERR] Write failed: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }
        } else {
            // consume the leftover characters
            while ((c = (char)getchar()) != '\n' && c != EOF)
                ;
            i = 0;
        }
    }

    if (close(pub_pipe_fd) == -1) {
        fprintf(stderr, "[ERR] Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    return 0;
}
