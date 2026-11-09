#ifndef MOVIES_MP2_DECODE_H
#define MOVIES_MP2_DECODE_H
#include <stddef.h>
#include <stdint.h>
/* MPEG-1 / MPEG-2 LSF / MPEG-2.5 Audio Layer II decoder (ISO/IEC 11172-3 and
 * 13818-3 low sampling frequencies). No CRC verification, no ancillary data. */
typedef struct Mp2Decoder { float v[2][1024]; unsigned offset[2]; } Mp2Decoder;
typedef struct Mp2Frame { unsigned bytes, rate, channels, samples; } Mp2Frame;
void mp2_reset(Mp2Decoder *d);
/* Parse a frame header at data; 0 when not a valid Layer II header. */
int mp2_header(const uint8_t *data, size_t size, Mp2Frame *frame);
/* Decode one whole frame into interleaved 16-bit PCM (frame->samples per channel,
 * at most 1152 * 2 values). Returns bytes consumed (0 on invalid/incomplete). */
size_t mp2_decode_frame(Mp2Decoder *d, const uint8_t *data, size_t size, int16_t *pcm, Mp2Frame *frame);
#endif
