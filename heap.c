/*
 * heap.c
 * ------
 * Binary min-heap ordered by Task.effective_priority ascending.
 *
 * Why from scratch:
 *   xv6 has no stdlib, no priority_queue, nothing.
 *   This is a standard textbook binary heap over a fixed array.
 *
 * Heap property:
 *   data[0] always has the LOWEST effective_priority value.
 *   Since the queues are min-heaps, lowest value = highest urgency =
 *   served first. Aging reduces effective_priority over time so
 *   long-waiting tasks bubble upward naturally.
 *
 * Thread safety:
 *   NOT thread-safe on their own.
 *   Caller must hold the matching lock before every call.
 *
 * Complexity:
 *   heap_push     O(log n)
 *   heap_pop      O(log n)
 *   heap_top      O(1)
 *   heap_rebuild  O(n)   — used after aging pass updates all priorities
 */

#include "scheduler.h"

/* swap two entries in the array */
static void swap(Heap *h, int i, int j) {
    Task tmp   = h->data[i];
    h->data[i] = h->data[j];
    h->data[j] = tmp;
}

/*
 * sift_up
 * After inserting at position i, move it up until the parent is smaller.
 * Stops at the root or when heap property is satisfied.
 */
static void sift_up(Heap *h, int i) {
    int p;
    while (i > 0) {
        p = (i - 1) / 2;
        if (h->data[i].effective_priority < h->data[p].effective_priority) {
            swap(h, i, p);
            i = p;
        } else {
            break;
        }
    }
}

/*
 * sift_down
 * Move element at position i downward until both children are larger.
 * Used after removing the root and after heap_rebuild.
 */
static void sift_down(Heap *h, int i) {
    int l, r, s;
    while (1) {
        l = 2 * i + 1;   /* left child index  */
        r = 2 * i + 2;   /* right child index */
        s = i;            /* index of smallest so far */

        if (l < h->size &&
            h->data[l].effective_priority < h->data[s].effective_priority)
            s = l;

        if (r < h->size &&
            h->data[r].effective_priority < h->data[s].effective_priority)
            s = r;

        if (s == i) break;   /* already smallest — done */

        swap(h, i, s);
        i = s;
    }
}

/*
 * heap_push
 * Add a task to the heap. Appends at the end then sifts up.
 * Silently drops the task if the heap is full.
 */
void heap_push(Heap *h, Task t) {
    if (h->size >= HEAP_CAP) {
#ifndef XV6
        fprintf(stderr, "[HEAP] overflow — raise HEAP_CAP\n");
#endif
        return;
    }
    h->data[h->size] = t;
    h->size++;
    sift_up(h, h->size - 1);
}

/*
 * heap_pop
 * Remove and return the root (lowest effective_priority).
 * Moves the last element to the root then sifts down.
 * Returns a sentinel task with id=-1 when the heap is empty.
 */
Task heap_pop(Heap *h) {
    Task empty;
    memset(&empty, 0, sizeof(Task));
    empty.id = -1;   /* caller checks id == -1 to detect empty */

    if (h->size == 0) return empty;

    Task result  = h->data[0];   /* save root */
    h->size--;
    if (h->size > 0) {
        h->data[0] = h->data[h->size];
        sift_down(h, 0);
    }
    return result;
}

/*
 * heap_top
 * Return a pointer to the root without removing it.
 * Returns NULL when the heap is empty.
 */
Task *heap_top(Heap *h) {
    if (h->size == 0) return (Task *)0;
    return &h->data[0];
}

/*
 * heap_rebuild
 * Restore heap order after a bulk update of effective_priority values.
 * Called by do_aging() after modifying all tasks in a queue.
 * Uses Floyd's bottom-up algorithm — O(n), faster than n insertions.
 */
void heap_rebuild(Heap *h) {
    int i = h->size / 2 - 1;
    while (i >= 0) {
        sift_down(h, i);
        i--;
    }
}

/*
 * heap_empty
 * Returns 1 if no tasks in the heap, 0 otherwise.
 */
int heap_empty(Heap *h) {
    return h->size == 0;
}
