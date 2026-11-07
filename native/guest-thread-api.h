#ifndef MOVIES_GUEST_THREAD_API_H
#define MOVIES_GUEST_THREAD_API_H
#include "runtime.h"
/* Enable only after shared guest memory and a prewarmed host pthread pool exist.
 * A fixed set of host pthreads persists; each guest job receives fresh CPU/TIB/
 * stack/TLS/ID. There is no detached Worker recycling or fabricated completion.
 * Stack resources use the private backend allocator. No child JS bridges are
 * installed by this module. Creation flags other than0 are unsupported. */
/* Explicit capacity must not exceed the prewarmed pool; init refuses live threads.
 * A child trap sets a sticky process-failure latch. Bootstrap must check it at
 * safe boundaries and stop the process; thread exit status alone is insufficient. */
int movies_guest_threads_init(unsigned limit);
int movies_guest_threads_shutdown(void);
int movies_guest_thread_is_child(void);
int movies_guest_threads_failed(char *out,uint32_t bytes);
/* Immutable first UTF8 failure published before release-store(*ready,1).
 * The Wasm page may acquire-load this flag and stop a blocked runtime owner.
 * View remains valid until process teardown; reset only during explicit init. */
typedef struct MoviesGuestFailureView {const uint32_t *ready;const char *message;uint32_t message_capacity;} MoviesGuestFailureView;
void movies_guest_threads_failure_view(MoviesGuestFailureView *out);
ImportHandler *movies_guest_thread_handlers(uint32_t *count);
#endif
