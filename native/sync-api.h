#ifndef MOVIES_SYNC_API_H
#define MOVIES_SYNC_API_H
#include "runtime.h"
/* One private virtual-process namespace. Host pthreads represent guest threads.
 * Initialize before guest execution, never concurrently with live guest threads.
 * No APC delivery, security descriptors, inheritance or guest thread creation. Mutexes, manual/auto reset events and thread
 * completion objects are supported. Wait-any/all is bounded to64 handles.
 * Nonpthread Wasm cannot block awaiting another guest producer: unsignaled
 * nonzero waits explicitly fail ERROR_NOT_SUPPORTED; polling still works.
 * Also owns process/thread pseudo-handle duplication, file-mapping sections
 * and WINMM event timers (fired lazily by waits), so CloseHandle reaches them. */
void movies_sync_init(void);
ImportHandler *movies_sync_handlers(uint32_t *count);
/* CloseHandle fallback: 1 closed, 0 not an owned live mutex/event handle. */
int movies_sync_close_handle(uint32_t handle);
void movies_sync_abandon_current(void);
uint32_t movies_sync_create_thread_object(void **token);
void movies_sync_complete_thread(void *token,uint32_t exit_code);
/* Binds the calling host thread's GetCurrentThread duplication target to a
 * completion token from movies_sync_create_thread_object (guest children). */
void movies_sync_bind_current_thread(void *token);
/* Shared wait core (WaitFor*, MsgWaitForMultipleObjects). Returns WAIT_OBJECT_0+i,
 * WAIT_ABANDONED_0+i, WAIT_TIMEOUT (258) or WAIT_FAILED with *error; LastError is
 * untouched. count 0 sleeps for timeout. May trap on a sticky guest-thread failure. */
uint32_t movies_sync_wait(CPU *c,uint32_t count,const uint32_t *handles,int all,uint32_t timeout,uint32_t *error);
/* 1 for the current-process pseudo handle or a duplicated process handle. */
int movies_sync_is_process_handle(uint32_t handle);
/* Closable inert objects owned by other modules: tag identifies the owner. */
uint32_t movies_sync_create_tagged(uint32_t tag,uint32_t value);
int movies_sync_tagged_get(uint32_t handle,uint32_t tag,uint32_t *value);
int movies_sync_tagged_set(uint32_t handle,uint32_t tag,uint32_t value);
#endif
