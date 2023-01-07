#include "mbroker.h"
#include "common.h"
#include "logging.h"
#include "operations.h"
#include "producer-consumer.h"

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

    set_log_level(LOG_VERBOSE);

    if (argc == 2 && !strcmp(argv[1], "--help")) {
        printf("usage: ./mbroker <pipename> <max_sessions>\n");
        return 0;
    }

    size_t max_sessions;

    if (argc != 3 || sscanf(argv[2], "%ld", &max_sessions) == 0) {
        fprintf(stderr, "mbroker: Invalid arguments.\nTry './mbroker --help'"
                        " for more information.\n");
        exit(EXIT_FAILURE);
    }

    // Init the file system
    if (tfs_init(NULL) == -1) {
        PANIC("tfs_init failed");
    }

    int register_pipe_fd, aux_reg_pipe_fd;

    // Remove pipe if it exists
    if (unlink(argv[1]) != 0 && errno != ENOENT) {
        PANIC("unlink(%s) failed: %s", argv[1], strerror(errno));
    }

    // Create the register_pipe
    if (mkfifo(argv[1], 0640) != 0) {
        PANIC("mkfifo failed: %s", strerror(errno));
    }

    // Open it for reading
    if ((register_pipe_fd = open(argv[1], O_RDONLY)) == -1) {
        PANIC("open failed: %s", strerror(errno));
    }

    // Open it for writing, so after dealing with all registrations pending,
    // reading from the pipe won't return 0 (which means there's no writers),
    // and instead will be waiting (passively) for someone to write something to
    // the pipe, since there's always at least one writer, the mbroker itself
    if ((aux_reg_pipe_fd = open(argv[1], O_WRONLY)) == -1) {
        PANIC("open failed: %s", strerror(errno));
    }

    // Init the producer-consumer queue
    pc_queue_t *queue = (pc_queue_t *)malloc(sizeof(pc_queue_t));
    if (queue == NULL) {
        PANIC("couldn't malloc queue")
    }

    if (pcq_create(queue, max_sessions / 2) == -1) { // TODO: * 2
        PANIC("couldn't create pc_queue")
    }

    char opcode;

    // Loop while dealing with clients
    while (1) {
        // Read the OP_CODE
        ssize_t ret = read(register_pipe_fd, &opcode, OPCODE_SIZE);

        if (ret == 0) {
            // ret == 0 indicates EOF
            PANIC("registration pipe is closed")
        } else if (ret == -1) {
            // ret == -1 indicates error
            PANIC("read failed: %s", strerror(errno))
        }

        switch (opcode) {
        case OPCODE_PUB_REG: // publisher registration
        case OPCODE_SUB_REG: // subscriber registration
        /* Manager requests */
        case OPCODE_BOX_CREAT:  // Box creation
        case OPCODE_BOX_REMOVE: // Box removal
        {
            void *registration = calloc(REGISTRATION_SIZE, sizeof(char));
            if (registration == NULL) {
                PANIC("couldn't malloc registration");
            }

            // copy OP_CODE
            memcpy(registration, &opcode, OPCODE_SIZE);

            // Read the rest of the registration
            if (read(register_pipe_fd, registration + OPCODE_SIZE,
                     REGISTRATION_SIZE - OPCODE_SIZE) <
                REGISTRATION_SIZE - OPCODE_SIZE) {
                PANIC("read failed: %s", strerror(errno));
            }

            pcq_enqueue(queue, registration);

            handle_registration(queue);

            break;
        }
        case OPCODE_BOX_LIST: { // Box listing
            void *registration = calloc(LIST_REQUEST_SIZE, sizeof(char));
            if (registration == NULL) {
                PANIC("couldn't malloc registration");
            }

            // copy OP_CODE
            memcpy(registration, &opcode, OPCODE_SIZE);

            // Read the rest of the registration
            if (read(register_pipe_fd, registration + OPCODE_SIZE,
                     LIST_REQUEST_SIZE - OPCODE_SIZE) <
                LIST_REQUEST_SIZE - OPCODE_SIZE) {
                PANIC("read failed: %s", strerror(errno));
            }

            pcq_enqueue(queue, registration);

            handle_registration(queue);

            break;
        }
        default:
            PANIC("Internal error: Invalid OP_CODE!");
            break;
        }
    }

    free(queue);

    if (close(register_pipe_fd) == -1) {
        PANIC("close failed: %s", strerror(errno));
    }

    if (close(aux_reg_pipe_fd) == -1) {
        PANIC("close failed: %s", strerror(errno));
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

void handle_registration(pc_queue_t *queue) {
    while (1) {
        char *registration = (char *)pcq_dequeue(queue);
        LOG("Received a registration");

        char opcode = registration[0];
        switch (opcode) {
        case OPCODE_PUB_REG: // publisher registration
        case OPCODE_SUB_REG: // subscriber registration
        /* Manager requests */
        case OPCODE_BOX_CREAT:  // Box creation
        case OPCODE_BOX_REMOVE: // Box removal
        {
            char pipe_path[PIPENAME_SIZE];
            // Copy the client pipe path, from where we will interact with him
            memcpy(pipe_path, registration + OPCODE_SIZE, PIPENAME_SIZE);

            char box_name[BOXNAME_SIZE];
            // Copy the box name
            memcpy(box_name, registration + OPCODE_SIZE + PIPENAME_SIZE,
                   BOXNAME_SIZE);

            // The registration buffer was dinamicaly alloced by mbroker, we
            // are responsible for freeing it
            free(registration);

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
            // Copy the client pipe path, from where we will interact with him
            memcpy(pipe_path, registration + OPCODE_SIZE, PIPENAME_SIZE);

            // The registration buffer was dinamicaly alloced by mbroker, we
            // are responsible for freeing it
            free(registration);

            box_listing(pipe_path);

            break;
        }
        default:
            PANIC("Internal error: Invalid OP_CODE!");
            break;
        }

        break; // TODO: remove in the future
    }
}

void pub_connect(char *pub_pipe_path, char *box_name) {
    int pub_pipe_fd, box_fd;

    // Open the pub_pipe for reading
    if ((pub_pipe_fd = open(pub_pipe_path, O_RDONLY)) == -1) {
        PANIC("open failed: %s", strerror(errno));
    }

    int i_box = box_lookup(box_name);

    // Check if box exists
    if (i_box == -1) {
        INFO("box %s doesn't exist", box_name);

        // Close the pub pipe
        if (close(pub_pipe_fd) == -1) {
            PANIC("close failed: %s", strerror(errno));
        }
        return;
    }

    box_t *box = &boxes[i_box];

    // Check if box is full
    if (box->box_size == BOX_SIZE) {
        // TODO: alertar subs que ja n vai ser escrito mais nada
        INFO("box %s is full", box_name);

        // Close the pub pipe
        if (close(pub_pipe_fd) == -1) {
            PANIC("close failed: %s", strerror(errno));
        }
        return;
    }

    // TODO: atencao mutex
    // Check if there's no pub already in the given box
    if (box->n_publishers == 1) {
        INFO("there's already a pub in box %s", box_name);

        // Close the pub pipe
        if (close(pub_pipe_fd) == -1) {
            PANIC("close failed: %s", strerror(errno));
        }
        return;
    }

    box->n_publishers = 1;

    // Open the box to write the messages
    if ((box_fd = tfs_open(box_name, TFS_O_APPEND)) == -1) {
        PANIC("tfs_open failed");
    }

    ssize_t ret;
    char buffer[PUB_MSG_SIZE];
    char msg[MSG_MAX_SIZE];

    // Read incoming messages until pub closes its pipe
    while (1) {
        ret = read(pub_pipe_fd, buffer, PUB_MSG_SIZE);

        if (ret == 0) {
            // ret == 0 indicates EOF, pub ended session
            break;
        } else if (ret == -1) {
            // ret == -1 indicates error
            PANIC("read failed: %s", strerror(errno));
        }

        // Verify code
        if (buffer[0] != OPCODE_PUB_MSG) {
            PANIC("Internal error: Invalid OP_CODE!");
        }

        // Copy only the msg itself
        strcpy(msg, buffer + OPCODE_SIZE);

        // Check if box has been deleted by a manager in the meantime
        if (free_boxes[i_box] == 1)
            break;

        if ((ret = tfs_write(box_fd, msg, strlen(msg) + 1)) < strlen(msg) + 1) {
            if (ret == -1) { // error
                PANIC("tfs_write failed");
            } else { // Couldn't write whole message
                // TODO: alertar subs que ja n vai ser escrito mais nada
                INFO("box %s is full", box_name);
                box->box_size += (uint64_t)ret;
                break;
            }
        }

        box->box_size += (uint64_t)ret;
    }

    if (tfs_close(box_fd) == -1) {
        PANIC("tfs_close failed");
    }

    box->n_publishers = 0;

    if (close(pub_pipe_fd) == -1) {
        PANIC("close failed: %s", strerror(errno));
    }
}

void sub_connect(char *sub_pipe_path, char *box_name) {
    int sub_pipe_fd, box_fd;

    // Open the sub_pipe for writing messages
    if ((sub_pipe_fd = open(sub_pipe_path, O_WRONLY)) == -1) {
        PANIC("open failed: %s", strerror(errno));
    }

    int i_box = box_lookup(box_name);

    // Check if box exists
    if (i_box == -1) {
        INFO("box %s doesn't exist", box_name);

        // Close the sub_pipe
        if (close(sub_pipe_fd) == -1) {
            PANIC("close failed: %s", strerror(errno));
        }

        return;
    }

    box_t *box = &boxes[i_box];

    // TODO: atencao mutex
    box->n_subscribers++;

    // Open the box to read the messages
    if ((box_fd = tfs_open(box_name, 0)) == -1) {
        PANIC("tfs_open failed");
    }

    ssize_t ret;
    char buffer[MSG_MAX_SIZE];
    char response[PUB_MSG_SIZE];
    response[0] = OPCODE_SUB_MSG;

    // Check if box has been deleted by a manager in the meantime
    // if (free_boxes[i_box] == 1) {
    // break;  // TODO: quando o sub tiver bem feito, e mandar a msg
    // recebida e dps terminar, ou terminar aqui antes de ler?
    // }

    // Read the messages from the box
    if ((ret = tfs_read(box_fd, buffer, BOX_SIZE)) == -1) {
        PANIC("tfs_read failed");
    }

    size_t len = strlen(buffer);
    char *ptr_buffer = buffer;
    size_t _ret = (size_t)ret;
    // Separate messages
    while (_ret > 0) {
        strncpy(response + OPCODE_SIZE, ptr_buffer,
                (len == _ret ? len : len + 1));

        _ret -= (len == _ret ? len : len + 1);
        // Send each message to sub
        if (write(sub_pipe_fd, response, PUB_MSG_SIZE) < PUB_MSG_SIZE) {
            // TODO: ignore SIGPIPE and end session
            PANIC("write failed: %s", strerror(errno))
        }
        ptr_buffer += len + 1;
        if (_ret > 0) {
            len = strnlen(ptr_buffer, _ret);
            if (len == _ret)
                response[OPCODE_SIZE + len] = '\0';
        }
    }

    if (tfs_close(box_fd) == -1) {
        PANIC("tfs_close failed");
    }

    box->n_subscribers--;

    if (close(sub_pipe_fd) == -1) {
        PANIC("close failed: %s", strerror(errno));
    }
}

void box_creation(char *man_pipe_path, char *box_name) {
    int man_pipe_fd, box_fd;

    // Open the man_pipe for writing messages
    if ((man_pipe_fd = open(man_pipe_path, O_WRONLY)) == -1) {
        PANIC("open failed: %s", strerror(errno));
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
                    new_box.box_size = 0;
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
                PANIC("Internal error: Box close failed!");
            }
        }
    }

    // Send the response

    // OP_CODE
    if (write(man_pipe_fd, &op_code, OPCODE_SIZE) < OPCODE_SIZE) {
        PANIC("write failed: %s", strerror(errno));
    }

    // return_code
    if (write(man_pipe_fd, &return_code, RETURN_CODE_SIZE) < RETURN_CODE_SIZE) {
        PANIC("write failed: %s", strerror(errno));
    }

    // error message
    if (return_code == -1) {
        if (write(man_pipe_fd, error_msg, ERROR_MSG_SIZE) < ERROR_MSG_SIZE) {
            PANIC("write failed: %s", strerror(errno));
        }
    }

    if (close(man_pipe_fd) == -1) {
        PANIC("close failed: %s", strerror(errno));
    }
}

void box_removal(char *man_pipe_path, char *box_name) {
    int man_pipe_fd;

    // Open the man_pipe for writing messages
    if ((man_pipe_fd = open(man_pipe_path, O_WRONLY)) == -1) {
        PANIC("open failed: %s", strerror(errno));
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
        PANIC("write failed: %s", strerror(errno));
    }

    // return_code
    if (write(man_pipe_fd, &return_code, RETURN_CODE_SIZE) < RETURN_CODE_SIZE) {
        PANIC("write failed: %s", strerror(errno));
    }

    // error message
    if (return_code == -1) {
        if (write(man_pipe_fd, error_msg, ERROR_MSG_SIZE) < ERROR_MSG_SIZE) {
            PANIC("write failed: %s", strerror(errno));
        }
    }

    if (close(man_pipe_fd) == -1) {
        PANIC("close failed: %s", strerror(errno));
    }
}

void box_listing(char *man_pipe_path) {
    int man_pipe_fd;

    // Open the man_pipe for writing messages
    if ((man_pipe_fd = open(man_pipe_path, O_WRONLY)) == -1) {
        PANIC("open failed: %s", strerror(errno));
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
                    PANIC("write failed: %s", strerror(errno));
                }

                // Send the "last" byte
                if (write(man_pipe_fd, &last, LAST_SIZE) < LAST_SIZE) {
                    PANIC("write failed: %s", strerror(errno));
                }

                // Send the box
                if (write(man_pipe_fd, last_box, sizeof(box_t)) <
                    sizeof(box_t)) {
                    PANIC("write failed: %s", strerror(errno));
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
            PANIC("write failed: %s", strerror(errno));
        }
        // Send the "last" byte
        if (write(man_pipe_fd, &last, LAST_SIZE) < LAST_SIZE) {
            PANIC("write failed: %s", strerror(errno));
        }
        // Send the box
        if (write(man_pipe_fd, last_box, sizeof(box_t)) < sizeof(box_t)) {
            PANIC("write failed: %s", strerror(errno));
        }
    }

    if (close(man_pipe_fd) == -1) {
        PANIC("close failed: %s", strerror(errno));
    }
}
