/* common.h
 *
 * Description: This file contains all the constants and structures that are
 * used by clients and the mbroker.
 */

#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdint.h>

#define MSG_MAX_SIZE 1024
#define PIPENAME_SIZE 256
#define BOXNAME_SIZE 32
#define OPCODE_SIZE sizeof(uint8_t)
#define REGISTRATION_SIZE OPCODE_SIZE + PIPENAME_SIZE + BOXNAME_SIZE
#define PUB_MSG_SIZE OPCODE_SIZE + MSG_MAX_SIZE
#define RETURN_CODE_SIZE sizeof(int32_t)
#define ERROR_MSG_SIZE 1024
#define BOX_RESPONSE OPCODE_SIZE + RETURN_CODE_SIZE + ERROR_MSG_SIZE
#define MAX_N_BOXES 23 // TODO: Can we even do this?
#define BOX_SIZE 1024
#define LAST_SIZE sizeof(uint8_t)
#define LIST_REQUEST_SIZE OPCODE_SIZE + PIPENAME_SIZE
#define INT64_SIZE sizeof(uint64_t)

// OP_CODES
#define OPCODE_PUB_REG 1
#define OPCODE_SUB_REG 2
#define OPCODE_BOX_CREAT 3
#define OPCODE_RES_BOX_CREAT 4
#define OPCODE_BOX_REMOVE 5
#define OPCODE_RES_BOX_REMOVE 6
#define OPCODE_BOX_LIST 7
#define OPCODE_RES_BOX_LIST 8
#define OPCODE_PUB_MSG 9
#define OPCODE_SUB_MSG 10

typedef struct {
    char box_name[BOXNAME_SIZE];
    uint64_t box_size;
    uint64_t n_publishers;
    uint64_t n_subscribers;
} box_t;

#endif
