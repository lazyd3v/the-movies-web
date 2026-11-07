#ifndef MOVIES_SYNC_API_H
#define MOVIES_SYNC_API_H
#include "runtime.h"
/* One private virtual-process namespace. Host pthreads represent guest threads.
 * Initialize before guest execution, never concurrently with live guest threads.
 * No APC delivery, security descriptors, inheritance or guest thread creation. Mutexes, manual/auto reset events and thread
 * completion objects are supported. Wait-any/all is bounded to64 handles.
 * Nonpthread Wasm cannot block awaiting another guest producer: unsignaled
 * nonzero waits explicitly fail ERROR_NOT_SUPPORTED; polling still works. */
void movies_sync_init(void);
ImportHandler *movies_sync_handlers(uint32_t *count);
/* CloseHandle fallback: 1 closed, 0 not an owned live mutex/event handle. */
int movies_sync_close_handle(uint32_t handle);
void movies_sync_abandon_current(void);
uint32_t movies_sync_create_thread_object(void **token);
void movies_sync_complete_thread(void *token,uint32_t exit_code);
#endif
