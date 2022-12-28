#include "logging.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
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
#define LIST_REQUEST_SIZE OPCODE_SIZE + PIPENAME_SIZE
#define RETURN_CODE_SIZE sizeof(int32_t)
#define ERROR_MSG_SIZE 1024
#define BOX_RESPONSE OPCODE_SIZE + RETURN_CODE_SIZE + ERROR_MSG_SIZE

static void print_usage() {
    fprintf(stderr,
            "usage:\n"
            "   ./manager <register_pipe> <pipe_name> create <box_name>\n"
            "   ./manager <register_pipe> <pipe_name> remove <box_name>\n"
            "   ./manager <register_pipe> <pipe_name> list\n");
}

// argv[1] = register_pipe, argv[2] = pipe_name, argv[3] = *, argv[4] = box_name
int main(int argc, char **argv) {
    int valid_args = 1;

    if (!strcmp(argv[1], "--help")) {
        print_usage();
        return 0;
    }

    if (argc != 4 && argc != 5)
        valid_args = 0;
    else if (argc == 4 && strcmp(argv[3], "list"))
        valid_args = 0;
    else if (argc == 5 &&
             (strcmp(argv[3], "create") && strcmp(argv[3], "remove")))
        valid_args = 0;

    if (!valid_args) {
        fprintf(stderr,
                "manager: Invalid arguments.\nTry './manager --help' for"
                " more information.\n");
        exit(EXIT_FAILURE);
    }

    int register_pipe_fd, man_pipe_fd;

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

    // Remove man_pipe if it exists
    if (unlink(argv[2]) != 0 && errno != ENOENT) {
        fprintf(stderr, "[ERR]: unlink(%s) failed: %s\n", argv[2],
                strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Create man_pipe, through which we will read the response from mbroker
    if (mkfifo(argv[2], 0640) != 0) {
        fprintf(stderr, "[ERR] mkfifo failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Protocol */

    char registration[REGISTRATION_SIZE] = {0};

    // OP_CODE
    if (argc == 4) // List
        registration[0] = 7;
    else if (!strcmp(argv[3], "create")) // Create
        registration[0] = 3;
    else if (!strcmp(argv[3], "remove")) // Remove
        registration[0] = 5;

    // Pipe path
    strcpy(registration + OPCODE_SIZE, argv[2]);

    if (argc != 4) { // create or remove a box
        // Box name
        strcpy(registration + OPCODE_SIZE + PIPENAME_SIZE, argv[4]);

        // Send registration to mbroker
        if (write(register_pipe_fd, registration, REGISTRATION_SIZE) <
            REGISTRATION_SIZE) {
            fprintf(stderr, "[ERR] Write failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    } else { // list the boxes, so there's no box_name to send
        // Send registration to mbroker
        if (write(register_pipe_fd, registration, LIST_REQUEST_SIZE) <
            LIST_REQUEST_SIZE) {
            fprintf(stderr, "[ERR] Write failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    if (close(register_pipe_fd) == -1) {
        fprintf(stderr, "[ERR] Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Open man_pipe for reading messages
    if ((man_pipe_fd = open(argv[2], O_RDONLY)) == -1) {
        fprintf(stderr, "[ERR] Open failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    char opcode;
    // Read the response
    if (read(man_pipe_fd, &opcode, OPCODE_SIZE) < OPCODE_SIZE) {
        fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    switch (opcode) {
    case 4:   // response to creation
    case 6: { // response to removal
        int32_t return_code;

        // Read the return code
        if (read(man_pipe_fd, &return_code, RETURN_CODE_SIZE) <
            RETURN_CODE_SIZE) {
            fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        if (return_code == -1) { // error
            char error_msg[ERROR_MSG_SIZE];

            // Read the error message
            if (read(man_pipe_fd, error_msg, ERROR_MSG_SIZE) < ERROR_MSG_SIZE) {
                fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }

            printf("%d\n%s\n", return_code, error_msg);
        } else {
            printf("%d\n", return_code);
        }
        break;
    }
    case 8: // response to box listing
        break;
    default:
        fprintf(stderr, "Internal error: Invalid OP_CODE!\n");
        exit(EXIT_FAILURE);
        break;
    }

    // TODO: devemos fechar?
    if (close(man_pipe_fd) == -1) {
        fprintf(stderr, "[ERR] Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // TODO: unlink named pipe

    return 0;
}
