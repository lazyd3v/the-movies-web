#ifndef MOVIES_AUDIO_API_H
#define MOVIES_AUDIO_API_H
#include "runtime.h"
/* Single owner guest thread; provider must never reenter guest code. A real
 * prewarmed AudioWorklet is required, not wall-clock completion simulation. */
typedef struct MoviesAudioHost {
 int (*available)(void);
 uint32_t (*open)(uint32_t channels);
 /* Copy all PCM bytes before acknowledging. header is the completion ID. */
 int (*write)(uint32_t token,uint32_t header,const uint8_t *pcm,uint32_t bytes);
 /* Return only actually consumed buffers, at most capacity IDs. */
 int (*poll)(uint32_t token,uint32_t *headers,uint32_t capacity,uint32_t *count);
 /* Must await real render-thread discard acknowledgement before returning. */
 int (*reset)(uint32_t token);
 int (*close)(uint32_t token);
} MoviesAudioHost;
/* No provider replacement with open device or deferred callbacks. */
int movies_audio_set_host(const MoviesAudioHost *host);
/* Explicit safe guest boundary AFTER imports return. Never call from arbitrary
 * JS handlers/worklet/pthread. Bounded pump preserves caller CPU registers. */
void movies_audio_pump(CPU *cpu);
/* Bind synchronous Module.moviesAudio* provider in Wasm; absent on native. */
int movies_audio_init_browser(void);
ImportHandler *movies_audio_handlers(uint32_t *count);
#endif
