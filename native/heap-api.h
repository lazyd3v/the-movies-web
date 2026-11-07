#ifndef MOVIES_HEAP_API_H
#define MOVIES_HEAP_API_H
#include "runtime.h"
/* Reserves initial pages (one page when initialSize is zero); growable heaps
 * acquire additional shared extents, retained for reuse until HeapDestroy.
 * Exclusive owner of GUEST_HEAP_BASE..GUEST_HEAP_HI; do not mix guest_alloc. */
ImportHandler *movies_heap_handlers(uint32_t *count);
/* Private backend allocations: zero-filled,8-byte aligned,1..32MiB; zero on
 * OOM. Guest Heap* cannot access this owner. Free rejects foreign/double frees. */
/* Snapshot of the implemented guest allocation arena, not host RAM. Free bytes
 * include reusable heap extents; unreserved excludes heap-owned reservations.
 * Guest Heap* and backend allocations share this arena; image/process-data
 * reservations lie outside it and are not falsely counted as free heap. */
typedef struct MoviesHeapBudget { uint32_t total,free_bytes,unreserved; } MoviesHeapBudget;
void movies_heap_budget(MoviesHeapBudget *out);
uint32_t movies_backend_alloc(uint32_t bytes);
int movies_backend_free(uint32_t address);
void movies_set_last_error(uint32_t error);
#endif
