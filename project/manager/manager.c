#include "manager.h"
#include "common.h"
#include "logging.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

    if (argc == 2 && !strcmp(argv[1], "--help")) {
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
        registration[0] = OPCODE_BOX_LIST;
    else if (!strcmp(argv[3], "create")) // Create
        registration[0] = OPCODE_BOX_CREAT;
    else if (!strcmp(argv[3], "remove")) // Remove
        registration[0] = OPCODE_BOX_REMOVE;

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

    ssize_t ret;
    char opcode;
    // Read the response
    ret = read(man_pipe_fd, &opcode, OPCODE_SIZE);

    if (ret == 0) {
        // ret == 0 indicates EOF
        // Both box creation and removal require a response from mbroker, so the
        // only way mbroker doesn't send anything back is if we are listing
        // boxes and there are none
        INFO("NO BOXES FOUND");
        return 0;
    } else if (ret == -1) {
        // ret == -1 indicates error
        fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    switch (opcode) {
    case OPCODE_RES_BOX_CREAT:    // response to creation
    case OPCODE_RES_BOX_REMOVE: { // response to removal
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

            printf("ERROR: %s\n", error_msg);
        } else {
            printf("OK\n");
        }

        break;
    }
    case OPCODE_RES_BOX_LIST: { // response to box listing
        box_t boxes[MAX_N_BOXES];
        uint8_t last = 0;
        size_t i = 0;

        while (!last) {

            if (i != 0) {
                // Read the OP_CODE
                if (read(man_pipe_fd, &opcode, OPCODE_SIZE) < OPCODE_SIZE) {
                    fprintf(stderr, "[ERR]: read failed: %s\n",
                            strerror(errno));
                    exit(EXIT_FAILURE);
                }

                // Verify code
                if (opcode != OPCODE_RES_BOX_LIST) {
                    fprintf(stderr, "Internal error: Invalid OP_CODE!\n");
                    exit(EXIT_FAILURE);
                }
            }

            // Read the "last" byte
            if (read(man_pipe_fd, &last, LAST_SIZE) < LAST_SIZE) {
                fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }

            // Read a box_t
            if (read(man_pipe_fd, &boxes[i++], sizeof(box_t)) < sizeof(box_t)) {
                fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }
        }

        // Sort the boxes in lexicographical order
        qsort(boxes, i, sizeof(box_t), box_comparator_fn);

        // Print the boxes
        for (size_t j = 0; j < i; j++) {
            fprintf(stdout, "%s %zu %zu %zu\n", boxes[j].box_name,
                    boxes[j].box_size, boxes[j].n_publishers,
                    boxes[j].n_subscribers);
        }

        break;
    }
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

int box_comparator_fn(const void *a, const void *b) {
    return strcmp(((box_t *)(a))->box_name, ((box_t *)(b))->box_name);
}
