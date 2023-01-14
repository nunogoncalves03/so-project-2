#include "mbroker.h"
#include "common.h"
#include "locks.h"
#include "logging.h"
#include "operations.h"
#include "producer-consumer.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Boxes and respective locks */
static box_t boxes[MAX_N_BOXES];
static pthread_mutex_t boxes_locks[MAX_N_BOXES];
static int free_boxes[MAX_N_BOXES];
static pthread_mutex_t free_boxes_lock;
static pthread_cond_t boxes_cond_vars[MAX_N_BOXES];

/* Variable to know whether mbroker should be shutdown */
static int shutdown_mbroker = 0;

void sigint_handler() { shutdown_mbroker = 1; }

// argv[1] = register_pipe, argv[2] = max_sessions
int main(int argc, char **argv) {

    if (signal(SIGINT, sigint_handler) == SIG_ERR) {
        PANIC("couldn't set signal handler")
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        PANIC("couldn't set signal handler")
    }

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
        PANIC("tfs_init failed")
    }

    // Initialize free_boxes
    for (int i = 0; i < MAX_N_BOXES; i++)
        free_boxes[i] = 1;

    // Initialize locks
    mutex_init(&free_boxes_lock);

    for (int i = 0; i < MAX_N_BOXES; i++)
        mutex_init(&boxes_locks[i]);

    for (int i = 0; i < MAX_N_BOXES; i++)
        cond_init(&boxes_cond_vars[i]);

    // Set log level
    set_log_level(LOG_VERBOSE);

    // Init the producer-consumer queue
    pc_queue_t queue;

    if (pcq_create(&queue, max_sessions * 2) == -1) {
        PANIC("couldn't create pc_queue")
    }

    // Create the worker threads
    pthread_t tid[max_sessions];
    for (int i = 0; i < max_sessions; i++)
        pthread_create(&tid[i], NULL, handle_registration, &queue);

    int register_pipe_fd, aux_reg_pipe_fd = 0;

    // Remove pipe if it exists
    if (unlink(argv[1]) != 0 && errno != ENOENT) {
        PANIC("unlink(%s) failed: %s", argv[1], strerror(errno))
    }

    // Create the register_pipe
    if (mkfifo(argv[1], 0640) != 0) {
        PANIC("mkfifo failed: %s", strerror(errno))
    }

    // Open it for reading
    if ((register_pipe_fd = open(argv[1], O_RDONLY)) == -1 &&
        !shutdown_mbroker) {
        PANIC("open failed: %s", strerror(errno))
    }

    // Open it for writing, so after dealing with all registrations pending,
    // reading from the pipe won't return 0 (which means there are no writers),
    // and instead will be waiting (passively) for someone to write something to
    // the pipe, since there's always at least one writer, the mbroker itself
    if (!shutdown_mbroker &&
        (aux_reg_pipe_fd = open(argv[1], O_WRONLY)) == -1) {
        PANIC("open failed: %s", strerror(errno))
    }

    char opcode;
    // Loop forever reading registrations
    while (!shutdown_mbroker) {
        // Read the OP_CODE
        ssize_t ret = read(register_pipe_fd, &opcode, OPCODE_SIZE);

        if (shutdown_mbroker)
            break;

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
                PANIC("couldn't malloc registration")
            }

            // copy OP_CODE
            memcpy(registration, &opcode, OPCODE_SIZE);

            // Read the rest of the registration
            if (read(register_pipe_fd, registration + OPCODE_SIZE,
                     REGISTRATION_SIZE - OPCODE_SIZE) <
                REGISTRATION_SIZE - OPCODE_SIZE) {
                PANIC("read failed: %s", strerror(errno))
            }

            LOG("received a registration from: %s code: %d",
                ((char *)(registration + OPCODE_SIZE)), opcode)
            pcq_enqueue(&queue, registration);

            break;
        }
        case OPCODE_BOX_LIST: { // Box listing
            void *registration = calloc(LIST_REQUEST_SIZE, sizeof(char));
            if (registration == NULL) {
                PANIC("couldn't malloc registration")
            }

            // Copy OP_CODE
            memcpy(registration, &opcode, OPCODE_SIZE);

            // Read the rest of the registration
            if (read(register_pipe_fd, registration + OPCODE_SIZE,
                     LIST_REQUEST_SIZE - OPCODE_SIZE) <
                LIST_REQUEST_SIZE - OPCODE_SIZE) {
                PANIC("read failed: %s", strerror(errno))
            }

            LOG("received a registration from: %s code: %d",
                ((char *)(registration + OPCODE_SIZE)), opcode)
            pcq_enqueue(&queue, registration);

            break;
        }
        default:
            PANIC("Internal error: Invalid OP_CODE!")
            break;
        }
    }

    /* In this section we should destroy TFS, pcq, locks, cond vars and exit
    threads, but due to problems with destroying locks, which are caused
    by threads that are holding the lock at the time of its destruction, we
    chose not to do it, since it's not a requirement. */

    // Close register pipe
    if (register_pipe_fd != -1 && close(register_pipe_fd) == -1) {
        PANIC("close failed: %s", strerror(errno))
    }

    // Close auxiliar register pipe
    if (aux_reg_pipe_fd != -1 && close(aux_reg_pipe_fd) == -1) {
        PANIC("close failed: %s", strerror(errno))
    }

    // Remove register pipe
    if (unlink(argv[1]) != 0 && errno != ENOENT) {
        PANIC("unlink(%s) failed: %s", argv[1], strerror(errno))
    }
    printf("\n"); // Print a newline after ^C

    return 0;
}

int box_lookup(const char *box_name) {
    for (int i = 0; i < MAX_N_BOXES; i++) {
        mutex_lock(&boxes_locks[i]);
        if (free_boxes[i] == 0 && !strcmp(boxes[i].box_name, box_name)) {
            mutex_unlock(&boxes_locks[i]);
            return i;
        }
        mutex_unlock(&boxes_locks[i]);
    }
    return -1;
}

void *handle_registration(void *q) {
    pc_queue_t *queue = (pc_queue_t *)q;
    while (1) {
        char *registration = (char *)pcq_dequeue(queue);

        char opcode = registration[0];
        LOG("starting client session: %s",
            ((char *)(registration + OPCODE_SIZE)))

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
            PANIC("Internal error: Invalid OP_CODE!")
            break;
        }
    }
    return NULL;
}

void pub_connect(char *pub_pipe_path, char *box_name) {
    int pub_pipe_fd, box_fd;

    // Open the pub_pipe for reading
    if ((pub_pipe_fd = open(pub_pipe_path, O_RDONLY)) == -1) {
        if (errno == ENOENT) {
            WARN("publisher pipe %s no longer exists", pub_pipe_path)
            return;
        }
        PANIC("open failed: %s", strerror(errno))
    }

    mutex_lock(&free_boxes_lock);
    int i_box = box_lookup(box_name);

    // Check if box exists
    if (i_box == -1) {
        INFO("box %s doesn't exist", box_name)
        mutex_unlock(&free_boxes_lock);

        // Close the pub pipe
        if (close(pub_pipe_fd) == -1) {
            PANIC("close failed: %s", strerror(errno))
        }
        return;
    }
    mutex_lock(&boxes_locks[i_box]);
    mutex_unlock(&free_boxes_lock);

    box_t *box = &boxes[i_box];

    // Check if box is full
    if (box->box_size == BOX_SIZE) {
        INFO("box %s is full", box_name)
        mutex_unlock(&boxes_locks[i_box]);

        // Close the pub pipe
        if (close(pub_pipe_fd) == -1) {
            PANIC("close failed: %s", strerror(errno))
        }
        return;
    }

    // Check if there's no pub already in the given box
    if (box->n_publishers == 1) {
        INFO("there's already a pub in box %s", box_name)
        mutex_unlock(&boxes_locks[i_box]);

        // Close the pub pipe
        if (close(pub_pipe_fd) == -1) {
            PANIC("close failed: %s", strerror(errno))
        }
        return;
    }

    box->n_publishers = 1;
    mutex_unlock(&boxes_locks[i_box]);

    // Open the box to write the messages
    if ((box_fd = tfs_open(box_name, TFS_O_APPEND)) == -1) {
        WARN("tfs_open failed")

        mutex_lock(&boxes_locks[i_box]);
        box->n_publishers = 0;
        mutex_unlock(&boxes_locks[i_box]);

        if (close(pub_pipe_fd) == -1) {
            PANIC("close failed: %s", strerror(errno))
        }

        return;
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
            PANIC("read failed: %s", strerror(errno))
        }

        // Verify code
        if (buffer[0] != OPCODE_PUB_MSG) {
            PANIC("Internal error: Invalid OP_CODE!")
        }

        // Copy only the msg itself
        strcpy(msg, buffer + OPCODE_SIZE);

        mutex_lock(&free_boxes_lock);
        // Check if box has been deleted by a manager in the meantime
        if (free_boxes[i_box] == 1) {
            mutex_unlock(&free_boxes_lock);
            break;
        }

        mutex_lock(&boxes_locks[i_box]);
        mutex_unlock(&free_boxes_lock);
        if ((ret = tfs_write(box_fd, msg, strlen(msg) + 1)) < strlen(msg) + 1) {
            if (ret == -1) { // error
                mutex_unlock(&boxes_locks[i_box]);
                PANIC("tfs_write failed")
            } else { // Couldn't write whole message
                INFO("box %s is full", box_name)
                // Signal subs that a new message was written
                cond_broadcast(&boxes_cond_vars[i_box]);
                box->box_size += (uint64_t)ret;
                mutex_unlock(&boxes_locks[i_box]);
                break;
            }
        }

        // Signal subs that a new message was written
        cond_broadcast(&boxes_cond_vars[i_box]);
        box->box_size += (uint64_t)ret;
        mutex_unlock(&boxes_locks[i_box]);
    }

    if (tfs_close(box_fd) == -1) {
        PANIC("tfs_close failed")
    }

    mutex_lock(&boxes_locks[i_box]);
    box->n_publishers = 0;
    mutex_unlock(&boxes_locks[i_box]);

    if (close(pub_pipe_fd) == -1) {
        PANIC("close failed: %s", strerror(errno))
    }
}

void sub_connect(char *sub_pipe_path, char *box_name) {
    int sub_pipe_fd, box_fd;

    // Open the sub_pipe for writing messages
    if ((sub_pipe_fd = open(sub_pipe_path, O_WRONLY)) == -1) {
        if (errno == ENOENT) {
            WARN("subscriber pipe %s no longer exists", sub_pipe_path)
            return;
        }
        PANIC("open failed: %s", strerror(errno))
    }

    mutex_lock(&free_boxes_lock);
    int i_box = box_lookup(box_name);

    // Check if box exists
    if (i_box == -1) {
        INFO("box %s doesn't exist", box_name)
        mutex_unlock(&free_boxes_lock);

        // Close the sub_pipe
        if (close(sub_pipe_fd) == -1) {
            PANIC("close failed: %s", strerror(errno))
        }

        return;
    }
    mutex_lock(&boxes_locks[i_box]);
    mutex_unlock(&free_boxes_lock);

    box_t *box = &boxes[i_box];

    box->n_subscribers++;
    mutex_unlock(&boxes_locks[i_box]);

    // Open the box to read the messages
    if ((box_fd = tfs_open(box_name, 0)) == -1) {
        WARN("tfs_open failed")

        mutex_lock(&boxes_locks[i_box]);
        box->n_subscribers--;
        mutex_unlock(&boxes_locks[i_box]);

        if (close(sub_pipe_fd) == -1) {
            PANIC("close failed: %s", strerror(errno))
        }

        return;
    }

    ssize_t ret;
    char buffer[MSG_MAX_SIZE + 1];
    char response[PUB_MSG_SIZE];
    response[0] = OPCODE_SUB_MSG;
    int end_session = 0;

    mutex_lock(&free_boxes_lock);
    do {
        // Check if box has been deleted by a manager in the meantime
        if (free_boxes[i_box] == 1) {
            mutex_unlock(&free_boxes_lock);
            break;
        }

        mutex_lock(&boxes_locks[i_box]);
        mutex_unlock(&free_boxes_lock);
        // Read the messages from the box
        if ((ret = tfs_read(box_fd, buffer, BOX_SIZE)) == -1) {
            mutex_unlock(&boxes_locks[i_box]);
            PANIC("tfs_read failed")
        }
        mutex_unlock(&boxes_locks[i_box]);

        buffer[ret] = '\0';
        size_t len;
        char *ptr_buffer = buffer;
        size_t _ret = (size_t)ret;
        // Separate messages
        while (_ret > 0) {
            len = strlen(ptr_buffer);
            strcpy(response + OPCODE_SIZE, ptr_buffer);
            // if there was no '\0' at the end of the last message
            if (_ret == len && ret != 0)
                _ret++;
            _ret -= len + 1;
            // Send each message to sub
            if (write(sub_pipe_fd, response, PUB_MSG_SIZE) < PUB_MSG_SIZE) {
                if (errno == EPIPE) {
                    end_session = 1;
                    break;
                } else {
                    PANIC("write failed: %s", strerror(errno))
                }
            }
            ptr_buffer += len + 1;
        }

        if (end_session)
            break;

        // Wait for a signal from a pub
        mutex_lock(&free_boxes_lock);
        cond_wait(&boxes_cond_vars[i_box], &free_boxes_lock);
    } while (1);

    if (tfs_close(box_fd) == -1) {
        PANIC("tfs_close failed")
    }

    mutex_lock(&boxes_locks[i_box]);
    box->n_subscribers--;
    mutex_unlock(&boxes_locks[i_box]);

    if (close(sub_pipe_fd) == -1) {
        PANIC("close failed: %s", strerror(errno))
    }
}

void box_creation(char *man_pipe_path, char *box_name) {
    int man_pipe_fd, box_fd;

    // Open the man_pipe for writing messages
    if ((man_pipe_fd = open(man_pipe_path, O_WRONLY)) == -1) {
        if (errno == ENOENT) {
            WARN("manager pipe %s no longer exists", man_pipe_path)
            return;
        }
        PANIC("open failed: %s", strerror(errno))
    }

    /* Protocol */

    // OP_CODE
    char op_code = OPCODE_RES_BOX_CREAT;

    // return_code
    int32_t return_code = 0;

    // error message
    char error_msg[ERROR_MSG_SIZE] = {0};

    mutex_lock(&free_boxes_lock);
    // Check if box already exists
    if (box_lookup(box_name) != -1) {
        mutex_unlock(&free_boxes_lock);
        return_code = -1;
        strcpy(error_msg, "Box already exists.");
    } else {
        // Create the box
        if ((box_fd = tfs_open(box_name, TFS_O_CREAT)) == -1) {
            mutex_unlock(&free_boxes_lock);
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

                    mutex_lock(&boxes_locks[i]);
                    boxes[i] = new_box;
                    mutex_unlock(&boxes_locks[i]);
                    break;
                }
            }
            mutex_unlock(&free_boxes_lock);
        }

        if (box_fd != -1) {
            if (tfs_close(box_fd) == -1) {
                // Shouldn't happen
                PANIC("Internal error: Box close failed!")
            }
        }
    }

    // Send the response

    // OP_CODE
    if (write(man_pipe_fd, &op_code, OPCODE_SIZE) < OPCODE_SIZE &&
        errno != EPIPE) {
        PANIC("write failed: %s", strerror(errno))
    }

    // return_code
    if (write(man_pipe_fd, &return_code, RETURN_CODE_SIZE) < RETURN_CODE_SIZE &&
        errno != EPIPE) {
        PANIC("write failed: %s", strerror(errno))
    }

    // error message
    if (return_code == -1) {
        if (write(man_pipe_fd, error_msg, ERROR_MSG_SIZE) < ERROR_MSG_SIZE &&
            errno != EPIPE) {
            PANIC("write failed: %s", strerror(errno))
        }
    }

    if (close(man_pipe_fd) == -1) {
        PANIC("close failed: %s", strerror(errno))
    }
}

void box_removal(char *man_pipe_path, char *box_name) {
    int man_pipe_fd;

    // Open the man_pipe for writing messages
    if ((man_pipe_fd = open(man_pipe_path, O_WRONLY)) == -1) {
        if (errno == ENOENT) {
            WARN("manager pipe %s no longer exists", man_pipe_path)
            return;
        }
        PANIC("open failed: %s", strerror(errno))
    }

    /* Protocol */

    // OP_CODE
    char op_code = OPCODE_RES_BOX_REMOVE;

    // return_code
    int32_t return_code = 0;

    // error message
    char error_msg[ERROR_MSG_SIZE] = {0};

    int i_box;
    mutex_lock(&free_boxes_lock);
    // Check if box doesn't exist
    if ((i_box = box_lookup(box_name)) == -1) {
        mutex_unlock(&free_boxes_lock);
        return_code = -1;
        strcpy(error_msg, "Box doesn't exist.");
    } else {
        // Remove the box
        mutex_lock(&boxes_locks[i_box]);
        if (tfs_unlink(box_name) == -1) {
            return_code = -1;
            strcpy(error_msg, "Couldn't remove box.");
        } else {
            free_boxes[i_box] = 1;
            // Alert sub threads that the box has been removed
            cond_broadcast(&boxes_cond_vars[i_box]);
        }

        mutex_unlock(&boxes_locks[i_box]);
        mutex_unlock(&free_boxes_lock);
    }

    // Send the response

    // OP_CODE
    if (write(man_pipe_fd, &op_code, OPCODE_SIZE) < OPCODE_SIZE &&
        errno != EPIPE) {
        PANIC("write failed: %s", strerror(errno))
    }

    // return_code
    if (write(man_pipe_fd, &return_code, RETURN_CODE_SIZE) < RETURN_CODE_SIZE &&
        errno != EPIPE) {
        PANIC("write failed: %s", strerror(errno))
    }

    // error message
    if (return_code == -1) {
        if (write(man_pipe_fd, error_msg, ERROR_MSG_SIZE) < ERROR_MSG_SIZE &&
            errno != EPIPE) {
            PANIC("write failed: %s", strerror(errno))
        }
    }

    if (close(man_pipe_fd) == -1) {
        PANIC("close failed: %s", strerror(errno))
    }
}

void box_listing(char *man_pipe_path) {
    int man_pipe_fd;

    // Open the man_pipe for writing messages
    if ((man_pipe_fd = open(man_pipe_path, O_WRONLY)) == -1) {
        if (errno == ENOENT) {
            WARN("manager pipe %s no longer exists", man_pipe_path)
            return;
        }
        PANIC("open failed: %s", strerror(errno))
    }

    /* Protocol */

    // OP_CODE
    char op_code = OPCODE_RES_BOX_LIST;

    // "last" byte
    uint8_t last = 0;
    box_t *last_box = NULL;
    int i_last_box;

    mutex_lock(&free_boxes_lock);
    for (int i = 0; i < MAX_N_BOXES; i++) {
        if (last_box == NULL) {
            if (free_boxes[i] == 0) {
                last_box = &boxes[i];
                i_last_box = i;
            }
        } else {
            if (free_boxes[i] == 0) {
                // Send the OP_CODE
                if (write(man_pipe_fd, &op_code, OPCODE_SIZE) < OPCODE_SIZE &&
                    errno != EPIPE) {
                    mutex_unlock(&free_boxes_lock);
                    PANIC("write failed: %s", strerror(errno))
                }

                // Send the "last" byte
                if (write(man_pipe_fd, &last, LAST_SIZE) < LAST_SIZE &&
                    errno != EPIPE) {
                    mutex_unlock(&free_boxes_lock);
                    PANIC("write failed: %s", strerror(errno))
                }

                mutex_lock(&boxes_locks[i_last_box]);
                // Send the box
                if (write(man_pipe_fd, last_box, sizeof(box_t)) <
                        sizeof(box_t) &&
                    errno != EPIPE) {
                    mutex_unlock(&free_boxes_lock);
                    mutex_unlock(&boxes_locks[i_last_box]);
                    PANIC("write failed: %s", strerror(errno))
                }
                mutex_unlock(&boxes_locks[i_last_box]);

                last_box = &boxes[i];
                i_last_box = i;
            }
        }
    }

    if (last_box != NULL) {
        // it's the last box to list
        last = 1;

        // Send the OP_CODE
        if (write(man_pipe_fd, &op_code, OPCODE_SIZE) < OPCODE_SIZE &&
            errno != EPIPE) {
            mutex_unlock(&free_boxes_lock);
            PANIC("write failed: %s", strerror(errno))
        }
        // Send the "last" byte
        if (write(man_pipe_fd, &last, LAST_SIZE) < LAST_SIZE &&
            errno != EPIPE) {
            mutex_unlock(&free_boxes_lock);
            PANIC("write failed: %s", strerror(errno))
        }

        mutex_lock(&boxes_locks[i_last_box]);
        // Send the box
        if (write(man_pipe_fd, last_box, sizeof(box_t)) < sizeof(box_t) &&
            errno != EPIPE) {
            mutex_unlock(&free_boxes_lock);
            mutex_unlock(&boxes_locks[i_last_box]);
            PANIC("write failed: %s", strerror(errno))
        }
        mutex_unlock(&boxes_locks[i_last_box]);
    }
    mutex_unlock(&free_boxes_lock);

    if (close(man_pipe_fd) == -1) {
        PANIC("close failed: %s", strerror(errno))
    }
}
