#ifndef MOVIES_ATOMIC_API_H
#define MOVIES_ATOMIC_API_H
#include "runtime.h"
/* 32-bit Win32 interlocked operations on aligned guest memory. */
ImportHandler *movies_atomic_handlers(uint32_t *count);
#endif
