#ifndef MOVIES_DSOUND_API_H
#define MOVIES_DSOUND_API_H
#include "runtime.h"
#define MOVIES_DSOUND_BASE 0x04010000u
/* Rings come from alloc (guest memory, zeroed by this module); release frees them. */
int movies_dsound_init(uint32_t (*alloc)(uint32_t), int (*release)(uint32_t));
/* IDirectSound::CreateSoundBuffer body: DSBUFFERDESC at desc, result pointer out. */
uint32_t movies_dsound_create_buffer(uint32_t desc, uint32_t out);
void movies_dsound_enable_engine(void);
/* Host address of the renderer control block (8 slots x 8 int32). */
uint32_t movies_dsound_control_address(void);
volatile int32_t *movies_dsound_control_block(void);
/* Host-owned 44.1 kHz stereo PCM16 streaming voices (e.g. movie audio). */
int movies_dsound_stream_open(uint32_t bytes);
void movies_dsound_stream_close(int slot);
void movies_dsound_stream_play(int slot, int playing);
void movies_dsound_stream_reset(int slot);
void movies_dsound_stream_volume(int slot, int32_t hundredths_db);
uint32_t movies_dsound_stream_space(int slot);
uint32_t movies_dsound_stream_write(int slot, const uint8_t *pcm, uint32_t bytes);
uint64_t movies_dsound_stream_played(int slot);
ImportHandler *movies_dsound_handlers(uint32_t *count);
#endif
