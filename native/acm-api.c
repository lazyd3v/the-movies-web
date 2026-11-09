/* MSACM32 conversion streams for MPEG audio Layer II (WAVE_FORMAT_MPEG, 0x50)
 * to 16-bit PCM, plus PCM passthrough of identical formats.
 * https://learn.microsoft.com/en-us/windows/win32/api/msacm/nf-msacm-acmstreamopen
 * https://learn.microsoft.com/en-us/windows/win32/api/msacm/ns-msacm-acmstreamheader
 * Conversion is synchronous. Every source byte is accepted: partial frames carry
 * over to the next call, and decoded PCM that does not fit in the destination
 * stays pending, so callers that size buffers via acmStreamSize lose nothing. */
#include "acm-api.h"
#include "mp2-decode.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef MOVIES_ACM_SPACE
#define MOVIES_ACM_SPACE GUEST_SPACE
#endif
#define ARG(n) R32(c->esp + 4 + 4 * (n))
#define STREAMS 64
#define BASE 0x0c100001u
#define PREPARED 0x20000u
#define DONE 0x10000u
#define NOTPOSSIBLE 512u
#define INVALHANDLE 5u
#define INVALPARAM 11u
#define NOMEM 7u
#define UNPREPARED 514u
#define CARRY 8192
typedef struct Stream {
    int used, mpeg;
    unsigned channels, rate, avg_bytes;
    Mp2Decoder decoder;
    uint8_t carry[CARRY]; size_t carried;
    int16_t *pending; size_t pending_bytes, pending_at, pending_capacity;
} Stream;
static Stream streams[STREAMS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void ret(CPU *c, unsigned n, uint32_t v) { c->eax = v; c->esp += 4 * (n + 1); }
static int range(uint32_t p, uint64_t n) { return p && (uint64_t)p + n <= MOVIES_ACM_SPACE; }
static Stream *find(uint32_t h) { return h >= BASE && h < BASE + STREAMS && streams[h - BASE].used ? &streams[h - BASE] : NULL; }
static void log_once(const char *what, uint32_t tag, uint32_t caller) {
    static unsigned logs; if (logs++ < 16) fprintf(stderr, "[acm] %s tag=%#x caller=%08x\n", what, tag, caller);
}
static void stream_open(CPU *c) {
    uint32_t out = ARG(0), src = ARG(2), dst = ARG(3), filter = ARG(4), flags = ARG(7);
    if (!range(src, 18) || !range(dst, 16) || filter) { ret(c, 8, INVALPARAM); return; }
    unsigned tag = R16(src), ch = R16(src + 2), rate = R32(src + 4), avg = R32(src + 8);
    unsigned dtag = R16(dst), dch = R16(dst + 2), drate = R32(dst + 4), dbits = R16(dst + 14);
    int mpeg = tag == 0x50, pcm = tag == 1 && R16(src + 14) == 16;
    if ((!mpeg && !pcm) || dtag != 1 || dbits != 16 || dch != ch || drate != rate || !ch || ch > 2 || !rate) {
        log_once("acmStreamOpen conversion not possible", tag, R32(c->esp)); ret(c, 8, NOTPOSSIBLE); return;
    }
    if (flags & 1) { ret(c, 8, 0); return; } /* ACM_STREAMOPENF_QUERY */
    if (!range(out, 4)) { ret(c, 8, INVALPARAM); return; }
    pthread_mutex_lock(&lock);
    unsigned i = 0; while (i < STREAMS && streams[i].used) i++;
    if (i == STREAMS) { pthread_mutex_unlock(&lock); ret(c, 8, NOMEM); return; }
    Stream *s = &streams[i]; memset(s, 0, sizeof *s);
    s->used = 1; s->mpeg = mpeg; s->channels = ch; s->rate = rate; s->avg_bytes = avg ? avg : rate * ch * 2;
    mp2_reset(&s->decoder);
    pthread_mutex_unlock(&lock);
    W32(out, BASE + i); ret(c, 8, 0);
}
static void stream_close(CPU *c) {
    pthread_mutex_lock(&lock); Stream *s = find(ARG(0));
    if (s) { free(s->pending); memset(s, 0, sizeof *s); }
    pthread_mutex_unlock(&lock); ret(c, 2, s ? 0 : INVALHANDLE);
}
/* Upper bounds: a Layer II frame carries 1152 samples per channel. */
static void stream_size(CPU *c) {
    uint32_t n = ARG(1), out = ARG(2), flags = ARG(3);
    pthread_mutex_lock(&lock); Stream *s = find(ARG(0)); Stream copy = s ? *s : (Stream){0}; pthread_mutex_unlock(&lock);
    if (!s) { ret(c, 4, INVALHANDLE); return; }
    if (!range(out, 4) || (flags & ~1u)) { ret(c, 4, INVALPARAM); return; }
    uint64_t result;
    if (!copy.mpeg) result = n;
    else {
        uint64_t frame_pcm = 1152ull * copy.channels * 2, frame_bytes = 1152ull * copy.avg_bytes / copy.rate;
        if (frame_bytes < 1) frame_bytes = 1;
        if (!(flags & 1)) result = (n / frame_bytes + 2) * frame_pcm;      /* SOURCE -> destination bytes */
        else result = (n / frame_pcm) * (frame_bytes + 1);                 /* DESTINATION -> source bytes */
    }
    if (result > 0x7fffffffu) result = 0x7fffffffu;
    W32(out, (uint32_t)result); ret(c, 4, 0);
}
static void header_flags(CPU *c, int prepare) {
    uint32_t h = ARG(1);
    pthread_mutex_lock(&lock); Stream *s = find(ARG(0)); pthread_mutex_unlock(&lock);
    if (!s) { ret(c, 3, INVALHANDLE); return; }
    if (!range(h, 0x54) || R32(h) < 0x54) { ret(c, 3, INVALPARAM); return; }
    uint32_t status = R32(h + 4);
    if (!prepare && !(status & PREPARED)) { ret(c, 3, UNPREPARED); return; }
    W32(h + 4, prepare ? status | PREPARED : status & ~PREPARED); ret(c, 3, 0);
}
static void prepare_header(CPU *c) { header_flags(c, 1); }
static void unprepare_header(CPU *c) { header_flags(c, 0); }
static int append_pending(Stream *s, const int16_t *pcm, size_t bytes) {
    if (s->pending_at && s->pending_at == s->pending_bytes) s->pending_at = s->pending_bytes = 0;
    if (s->pending_bytes + bytes > s->pending_capacity) {
        size_t want = (s->pending_bytes + bytes) * 2;
        int16_t *grown = realloc(s->pending, want); if (!grown) return 0;
        s->pending = grown; s->pending_capacity = want;
    }
    memcpy((uint8_t *)s->pending + s->pending_bytes, pcm, bytes); s->pending_bytes += bytes; return 1;
}
static uint32_t drain(Stream *s, uint32_t dst, uint32_t capacity) {
    size_t available = s->pending_bytes - s->pending_at, n = available < capacity ? available : capacity;
    memcpy(HP(dst), (uint8_t *)s->pending + s->pending_at, n); s->pending_at += n;
    if (s->pending_at == s->pending_bytes) s->pending_at = s->pending_bytes = 0;
    return (uint32_t)n;
}
static void stream_convert(CPU *c) {
    uint32_t h = ARG(1), flags = ARG(2);
    pthread_mutex_lock(&lock); Stream *s = find(ARG(0));
    if (!s) { pthread_mutex_unlock(&lock); ret(c, 3, INVALHANDLE); return; }
    if (!range(h, 0x54) || !(R32(h + 4) & PREPARED)) { pthread_mutex_unlock(&lock); ret(c, 3, !range(h, 0x54) ? INVALPARAM : UNPREPARED); return; }
    uint32_t src = R32(h + 12), n = R32(h + 16), dst = R32(h + 28), capacity = R32(h + 32);
    if ((n && !range(src, n)) || (capacity && !range(dst, capacity))) { pthread_mutex_unlock(&lock); ret(c, 3, INVALPARAM); return; }
    if (flags & 0x10) { mp2_reset(&s->decoder); s->carried = 0; s->pending_at = s->pending_bytes = 0; } /* START */
    uint32_t written = drain(s, dst, capacity), used = 0, error = 0;
    if (!s->mpeg) {
        uint32_t room = capacity - written, take = n < room ? n : room;
        memcpy(HP(dst + written), HP(src), take); written += take; used = take;
    } else {
        static int16_t pcm[1152 * 2];
        while (used < n || s->carried >= 4) {
            /* Fill the carry buffer, then decode whole frames from it. */
            size_t room = CARRY - s->carried, take = n - used < room ? n - used : room;
            memcpy(s->carry + s->carried, HP(src + used), take); s->carried += take; used += (uint32_t)take;
            size_t at = 0, progressed = 0;
            for (;;) {
                while (at + 4 <= s->carried && !(s->carry[at] == 0xff && (s->carry[at + 1] & 0xe0) == 0xe0)) at++;
                Mp2Frame frame;
                if (at + 4 > s->carried || !mp2_header(s->carry + at, s->carried - at, &frame)) { if (at + 4 <= s->carried) { at++; continue; } break; }
                if (frame.bytes > s->carried - at) break;
                size_t got = mp2_decode_frame(&s->decoder, s->carry + at, s->carried - at, pcm, &frame);
                if (!got) { at++; continue; }
                at += got; progressed = 1;
                size_t bytes = (size_t)frame.samples * frame.channels * 2;
                if (frame.channels != s->channels) { error = NOTPOSSIBLE; break; }
                if (!append_pending(s, pcm, bytes)) { error = NOMEM; break; }
            }
            memmove(s->carry, s->carry + at, s->carried - at); s->carried -= at;
            written += drain(s, dst + written, capacity - written);
            if (error || (!progressed && used >= n)) break;
            if (!progressed && s->carried == CARRY) s->carried = 0; /* undecodable junk */
        }
        if (flags & 0x20) s->carried = 0; /* END: drop an incomplete trailing frame */
    }
    W32(h + 20, used); W32(h + 36, written); W32(h + 4, R32(h + 4) | DONE);
    pthread_mutex_unlock(&lock);
    ret(c, 3, error);
}
static void stream_reset(CPU *c) {
    pthread_mutex_lock(&lock); Stream *s = find(ARG(0));
    if (s) { mp2_reset(&s->decoder); s->carried = 0; s->pending_at = s->pending_bytes = 0; }
    pthread_mutex_unlock(&lock); ret(c, 2, s ? 0 : INVALHANDLE);
}
ImportHandler *movies_acm_handlers(uint32_t *count) {
    static ImportHandler handlers[] = {
        {"MSACM32.DLL!acmStreamOpen", stream_open}, {"MSACM32.DLL!acmStreamClose", stream_close},
        {"MSACM32.DLL!acmStreamSize", stream_size}, {"MSACM32.DLL!acmStreamPrepareHeader", prepare_header},
        {"MSACM32.DLL!acmStreamUnprepareHeader", unprepare_header}, {"MSACM32.DLL!acmStreamConvert", stream_convert},
        {"MSACM32.DLL!acmStreamReset", stream_reset}};
    *count = sizeof handlers / sizeof handlers[0]; return handlers;
}
