#ifndef MOVIES_THREAD_API_H
#define MOVIES_THREAD_API_H
#include "runtime.h"
/* Host pthread corresponds to a guest thread. No scheduler/CreateThread support. */
ImportHandler *movies_thread_handlers(uint32_t *count);
/* Persistent host worker reuse: binding a fresh guest identity clears its TLS
 * values/generations. Call once before each original guest start routine. */
uint32_t movies_thread_reserve_id(void);
void movies_thread_bind_id(uint32_t id);
#endif
