#ifndef _MBROKER_H__
#define _MBROKER_H__

#include "producer-consumer.h"

/* Searches for the given box
 * Input:
 *   - box_name: the box's name
 *
 * Returns the index of the box if it exists, -1 otherwise.
 */
int box_lookup(const char *box_name);

/* Pops a registration from the given queue and processes it
 * Input:
 *   - queue: a pointer to the queue
 *
 */
void handle_registration(pc_queue_t *queue);

/* Receives messages from a publsiher and stores them into the given box
 *
 * Input:
 *   - pub_pipe_path: The path of the pipe through which the messages will be
 *     sent;
 *   - box_name: The name of the box where the messages will be stored
 *
 */
void pub_connect(char *pub_pipe_path, char *box_name);

/* Continuously sends the messages stored in the given box to a subscriber
 *
 * Input:
 *   - sub_pipe_path: The path of the pipe through which the messages will be
 *     sent;
 *   - box_name: The name of the box where the messages are being stored.
 *
 */
void sub_connect(char *sub_pipe_path, char *box_name);

/* Creates a box in the tfs with the given name and stores it in "boxes"
 *
 * Input:
 *   - man_pipe_path: The path of the pipe through which the response to the
 *     request will be sent;
 *   - box_name: The name of the box to be created.
 *
 */
void box_creation(char *man_pipe_path, char *box_name);

/* Removes the box with the given name from the tfs and "boxes"
 *
 * Input:
 *   - man_pipe_path: The path of the pipe through which the response to the
 *     request will be sent;
 *   - box_name: The name of the box to be removed.
 *
 */
void box_removal(char *man_pipe_path, char *box_name);

/* Lists all the existing boxes (both in tfs and "boxes")
 *
 * Input:
 *   - man_pipe_path: The path of the pipe through which the response to the
 *     request will be sent.
 *
 */
void box_listing(char *man_pipe_path);

#endif
