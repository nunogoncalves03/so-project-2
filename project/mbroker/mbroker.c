#include "mbroker.h"
#include "common.h"
#include "logging.h"
#include "operations.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Boxes */
static box_t boxes[MAX_N_BOXES];
static int free_boxes[MAX_N_BOXES];

// argv[1] = register_pipe, argv[2] = max_sessions
int main(int argc, char **argv) {
    for (int i = 0; i < MAX_N_BOXES; i++)
        free_boxes[i] = 1;

    if (argc == 2 && !strcmp(argv[1], "--help")) {
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

    char opcode;

    // Loop while dealing with clients
    while (1) {
        // Read the OP_CODE
        ssize_t ret = read(register_pipe_fd, &opcode, OPCODE_SIZE);

        if (ret == 0) {
            // ret == 0 indicates EOF
            fprintf(stderr, "[INFO]: pipe closed\n");

            // TODO: reopen?
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

        switch (opcode) {
        case OPCODE_PUB_REG: // publisher registration
        case OPCODE_SUB_REG: // subscriber registration
        /* Manager requests */
        case OPCODE_BOX_CREAT:  // Box creation
        case OPCODE_BOX_REMOVE: // Box removal
        {
            char pipe_path[PIPENAME_SIZE];
            // Read the client pipe path, from where we will interact with him
            if (read(register_pipe_fd, pipe_path, PIPENAME_SIZE) <
                PIPENAME_SIZE) {
                fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }

            char box_name[BOXNAME_SIZE];
            // Read the box name
            if (read(register_pipe_fd, box_name, BOXNAME_SIZE) < BOXNAME_SIZE) {
                fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }

            if (opcode == OPCODE_PUB_REG) // publisher
                pub_connect(pipe_path, box_name);
            else if (opcode == OPCODE_SUB_REG) // subscriber
                sub_connect(pipe_path, box_name);
            else if (opcode == OPCODE_BOX_CREAT) // box creation
                box_creation(pipe_path, box_name);
            else if (opcode == OPCODE_BOX_REMOVE) // box removal
                box_removal(pipe_path, box_name);

            break;
        }

        case OPCODE_BOX_LIST: { // Box listing
            char pipe_path[PIPENAME_SIZE];
            // Read the client pipe path, from where we will interact with him
            if (read(register_pipe_fd, pipe_path, PIPENAME_SIZE) <
                PIPENAME_SIZE) {
                fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }

            box_listing(pipe_path);

            break;
        }
        default:
            fprintf(stderr, "Internal error: Invalid OP_CODE!\n");
            exit(EXIT_FAILURE);
            break;
        }
    }

    if (close(register_pipe_fd) == -1) {
        fprintf(stderr, "[ERR]: Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    return 0;
}

int box_lookup(const char *box_name) {
    for (int i = 0; i < MAX_N_BOXES; i++) {
        if (free_boxes[i] == 0 && !strcmp(boxes[i].box_name, box_name))
            return i;
    }
    return -1;
}

void pub_connect(char *pub_pipe_path, char *box_name) {
    int pub_pipe_fd, box_fd;

    // Open the pub_pipe for reading
    if ((pub_pipe_fd = open(pub_pipe_path, O_RDONLY)) == -1) {
        fprintf(stderr, "[ERR] Open failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    int i_box = box_lookup(box_name);

    // Check if box exists
    if (i_box == -1) {
        fprintf(stderr, "[ERR] Box %s doesn't exist.\n", box_name);
        exit(EXIT_FAILURE);
    }

    box_t *box = &boxes[i_box];

    // TODO: atencao mutex
    // Check if there's no pub already in the given box
    if (box->n_publishers == 1) {
        fprintf(stderr, "[ERR] There's already a pub in box %s.\n", box_name);
        exit(EXIT_FAILURE);
    }

    box->n_publishers = 1;

    // Open the box to write the messages
    if ((box_fd = tfs_open(box_name, TFS_O_APPEND)) == -1) {
        fprintf(stderr, "[ERR] tfs_open failed.\n");
        exit(EXIT_FAILURE);
    }

    ssize_t ret;
    char buffer[PUB_MSG_SIZE];
    char msg[MSG_MAX_SIZE];

    // Read incoming messages until pub closes its pipe
    while (1) {
        ret = read(pub_pipe_fd, buffer, PUB_MSG_SIZE);

        if (ret == 0) {
            // ret == 0 indicates EOF
            break;
        } else if (ret == -1) {
            // ret == -1 indicates error
            fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        // Verify code
        if (buffer[0] != OPCODE_PUB_MSG) {
            fprintf(stderr, "Internal error: Invalid OP_CODE!\n");
            exit(EXIT_FAILURE);
        }

        // Copy only the msg itself
        strcpy(msg, buffer + OPCODE_SIZE);

        if (tfs_write(box_fd, msg, strlen(msg) + 1) < strlen(msg) + 1) {
            fprintf(stderr, "[ERR]: tfs_write failed\n");
            exit(EXIT_FAILURE);
        }
    }

    if (tfs_close(box_fd) == -1) {
        fprintf(stderr, "[ERR] tfs_close failed.\n");
        exit(EXIT_FAILURE);
    }

    box->n_publishers = 0;

    if (close(pub_pipe_fd) == -1) {
        fprintf(stderr, "[ERR]: Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

void sub_connect(char *sub_pipe_path, char *box_name) {
    int sub_pipe_fd, box_fd;

    // Open the sub_pipe for writing messages
    if ((sub_pipe_fd = open(sub_pipe_path, O_WRONLY)) == -1) {
        fprintf(stderr, "[ERR] Open failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    int i_box = box_lookup(box_name);

    // Check if box exists
    if (i_box == -1) {
        fprintf(stderr, "[ERR] Box %s doesn't exist.\n", box_name);

        // Close the sub_pipe
        if (close(sub_pipe_fd) == -1) {
            fprintf(stderr, "[ERR]: Close failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        return;
    }

    box_t *box = &boxes[i_box];

    // TODO: atencao mutex
    box->n_subscribers++;

    // Open the box to read the messages
    if ((box_fd = tfs_open(box_name, 0)) == -1) {
        fprintf(stderr, "[ERR] tfs_open failed.\n");
        exit(EXIT_FAILURE);
    }

    ssize_t ret;
    char buffer[MSG_MAX_SIZE];
    char response[PUB_MSG_SIZE];
    response[0] = OPCODE_SUB_MSG;

    // Read the messages from the box
    // TODO: bloco apenas pode ter 1024, q é o max de uma msg
    if ((ret = tfs_read(box_fd, buffer, BOX_SIZE)) == -1) {
        fprintf(stderr, "[ERR]: tfs_read failed\n");
        exit(EXIT_FAILURE);
    }

    size_t len = strlen(buffer);
    char *ptr_buffer = buffer;
    size_t _ret = (size_t)ret;
    // Separate messages
    while (_ret > 0) {
        _ret -= len + 1;
        strcpy(response + OPCODE_SIZE, ptr_buffer);
        // Send each message to sub
        if (write(sub_pipe_fd, response, PUB_MSG_SIZE) < PUB_MSG_SIZE) {
            fprintf(stderr, "[ERR] Write failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        ptr_buffer += len + 1;
        len = strlen(ptr_buffer);
    }

    if (tfs_close(box_fd) == -1) {
        fprintf(stderr, "[ERR] tfs_close failed.\n");
        exit(EXIT_FAILURE);
    }

    box->n_subscribers--;

    if (close(sub_pipe_fd) == -1) {
        fprintf(stderr, "[ERR]: Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

void box_creation(char *man_pipe_path, char *box_name) {
    int man_pipe_fd, box_fd;

    // Open the man_pipe for writing messages
    if ((man_pipe_fd = open(man_pipe_path, O_WRONLY)) == -1) {
        fprintf(stderr, "[ERR] Open failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Protocol */

    // OP_CODE
    char op_code = OPCODE_RES_BOX_CREAT;

    // return_code
    int32_t return_code = 0;

    // error message
    char error_msg[ERROR_MSG_SIZE] = {0};

    // Check if box already exists
    if (box_lookup(box_name) != -1) {
        return_code = -1;
        strcpy(error_msg, "Box already exists.");
    } else {
        // Create the box
        if ((box_fd = tfs_open(box_name, TFS_O_CREAT)) == -1) {
            return_code = -1;
            strcpy(error_msg, "Couldn't create box.");
        } else {
            for (int i = 0; i < MAX_N_BOXES; i++) {
                if (free_boxes[i] == 1) {
                    free_boxes[i] = 0;

                    box_t new_box;
                    strcpy(new_box.box_name, box_name);
                    new_box.box_size = BOX_SIZE;
                    new_box.n_publishers = 0;
                    new_box.n_subscribers = 0;

                    boxes[i] = new_box;
                    break;
                }
            }
        }

        if (box_fd != -1) {
            if (tfs_close(box_fd) == -1) {
                // Shouldn't happen
                fprintf(stderr, "Internal error: Box close failed!\n");
                exit(EXIT_FAILURE);
            }
        }
    }

    // Send the response

    // OP_CODE
    if (write(man_pipe_fd, &op_code, OPCODE_SIZE) < OPCODE_SIZE) {
        fprintf(stderr, "[ERR] Write failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // return_code
    if (write(man_pipe_fd, &return_code, RETURN_CODE_SIZE) < RETURN_CODE_SIZE) {
        fprintf(stderr, "[ERR] Write failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // error message
    if (return_code == -1) {
        if (write(man_pipe_fd, error_msg, ERROR_MSG_SIZE) < ERROR_MSG_SIZE) {
            fprintf(stderr, "[ERR] Write failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    if (close(man_pipe_fd) == -1) {
        fprintf(stderr, "[ERR]: Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

void box_removal(char *man_pipe_path, char *box_name) {
    int man_pipe_fd;

    // Open the man_pipe for writing messages
    if ((man_pipe_fd = open(man_pipe_path, O_WRONLY)) == -1) {
        fprintf(stderr, "[ERR] Open failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Protocol */

    // OP_CODE
    char op_code = OPCODE_RES_BOX_REMOVE;

    // return_code
    int32_t return_code = 0;

    // error message
    char error_msg[ERROR_MSG_SIZE] = {0};

    int i_box;
    // Check if box doesn't exist
    if ((i_box = box_lookup(box_name)) == -1) {
        return_code = -1;
        strcpy(error_msg, "Box doesn't exist.");
    } else {
        // Remove the box
        if (tfs_unlink(box_name) == -1) {
            return_code = -1;
            strcpy(error_msg, "Couldn't remove box.");
        }

        free_boxes[i_box] = 1;
    }

    // Send the response

    // OP_CODE
    if (write(man_pipe_fd, &op_code, OPCODE_SIZE) < OPCODE_SIZE) {
        fprintf(stderr, "[ERR] Write failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // return_code
    if (write(man_pipe_fd, &return_code, RETURN_CODE_SIZE) < RETURN_CODE_SIZE) {
        fprintf(stderr, "[ERR] Write failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // error message
    if (return_code == -1) {
        if (write(man_pipe_fd, error_msg, ERROR_MSG_SIZE) < ERROR_MSG_SIZE) {
            fprintf(stderr, "[ERR] Write failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    if (close(man_pipe_fd) == -1) {
        fprintf(stderr, "[ERR]: Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

void box_listing(char *man_pipe_path) {
    int man_pipe_fd;

    // Open the man_pipe for writing messages
    if ((man_pipe_fd = open(man_pipe_path, O_WRONLY)) == -1) {
        fprintf(stderr, "[ERR] Open failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Protocol */

    // OP_CODE
    char op_code = OPCODE_RES_BOX_LIST;

    // "last" byte
    uint8_t last = 0;
    box_t *last_box = NULL;

    for (int i = 0; i < MAX_N_BOXES; i++) {
        if (last_box == NULL) {
            if (free_boxes[i] == 0)
                last_box = &boxes[i];
        } else {
            if (free_boxes[i] == 0) {
                // Send the OP_CODE
                if (write(man_pipe_fd, &op_code, OPCODE_SIZE) < OPCODE_SIZE) {
                    fprintf(stderr, "[ERR]: write failed: %s\n",
                            strerror(errno));
                    exit(EXIT_FAILURE);
                }

                // Send the "last" byte
                if (write(man_pipe_fd, &last, LAST_SIZE) < LAST_SIZE) {
                    fprintf(stderr, "[ERR]: write failed: %s\n",
                            strerror(errno));
                    exit(EXIT_FAILURE);
                }

                // Send the box
                if (write(man_pipe_fd, last_box, sizeof(box_t)) <
                    sizeof(box_t)) {
                    fprintf(stderr, "[ERR]: write failed: %s\n",
                            strerror(errno));
                    exit(EXIT_FAILURE);
                }

                last_box = &boxes[i];
            }
        }
    }

    if (last_box != NULL) {
        // it's the last box to list
        last = 1;

        // Send the OP_CODE
        if (write(man_pipe_fd, &op_code, OPCODE_SIZE) < OPCODE_SIZE) {
            fprintf(stderr, "[ERR]: write failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        // Send the "last" byte
        if (write(man_pipe_fd, &last, LAST_SIZE) < LAST_SIZE) {
            fprintf(stderr, "[ERR]: write failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        // Send the box
        if (write(man_pipe_fd, last_box, sizeof(box_t)) < sizeof(box_t)) {
            fprintf(stderr, "[ERR]: write failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    if (close(man_pipe_fd) == -1) {
        fprintf(stderr, "[ERR]: Close failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}
