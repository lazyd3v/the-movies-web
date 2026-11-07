#ifndef MOVIES_DIRECTINPUT_API_H
#define MOVIES_DIRECTINPUT_API_H
#include "runtime.h"
typedef struct MoviesDirectInputHost {
 int (*available)(void *);
 int (*focused)(void *,uint32_t hwnd);
 int (*valid_window)(void *,uint32_t hwnd);
 void (*pump)(void *);
} MoviesDirectInputHost;
/* UI-thread confined. Guest 03000000..03010000 and trampolines 0e000000
 * are exclusively reserved; init after the runtime function registry exists. */
int movies_directinput_init(void);
void movies_directinput_set_host(const MoviesDirectInputHost *,void *);
/* kind 1 keyboard, 2 mouse. Timestamp is process GetTickCount epoch.
 * Ignored unless acquired; returns 0 for invalid events, 1 otherwise. */
int movies_directinput_push_event(uint32_t kind,uint32_t offset,uint32_t data,uint32_t time);
ImportHandler *movies_directinput_handlers(uint32_t *count);
#endif
