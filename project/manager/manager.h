#ifndef _MANAGER_H__
#define _MANAGER_H__

/* Compares two boxes (box_t) by their name
 *
 * Input:
 *   - box_t* a, box_t* b
 *
 * Returns the lexicographical difference between the two names
 */
int box_comparator_fn(const void *a, const void *b);

#endif
