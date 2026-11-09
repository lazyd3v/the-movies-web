/* DirectSound secondary buffers as "hardware" rings in guest memory.
 * https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418055(v=vs.85) (IDirectSoundBuffer)
 * The AudioWorklet reads each playing ring directly from shared Wasm memory at its
 * own play cursor and publishes that cursor in a control block, like a DMA engine.
 * The guest polls GetCurrentPosition and writes behind the cursor. While the
 * renderer is not running (no heartbeat), the cursor advances by wall clock so the
 * engine keeps streaming. Thread-safe: the game pumps from its engine threads.
 *
 * Also provides dsound.dll!DllGetClassObject with an IClassFactory that creates
 * an IKsPropertySet whose Get/Set/QuerySupport return E_NOTIMPL (the game's
 * hardware-acceleration probe then selects plain DirectSound output). */
#define _POSIX_C_SOURCE 200809L
#include "dsound-api.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#ifndef MOVIES_DSOUND_SPACE
#define MOVIES_DSOUND_SPACE GUEST_SPACE
#endif
#define ARG(n) R32(c->esp + 4 + 4 * (n))
#define TRAMP 0x0e030000u
#define VT_BUFFER (MOVIES_DSOUND_BASE)
#define VT_FACTORY (MOVIES_DSOUND_BASE + 0x80)
#define VT_PROPERTY (MOVIES_DSOUND_BASE + 0xc0)
#define OBJ_FACTORY (MOVIES_DSOUND_BASE + 0x100)
#define OBJ_PROPERTY (MOVIES_DSOUND_BASE + 0x110)
#define OBJ_BUFFERS (MOVIES_DSOUND_BASE + 0x200)
#define SLOTS 8
#define E_NOINTERFACE 0x80004002u
#define E_NOTIMPL 0x80004001u
#define E_POINTER 0x80004003u
#define INVALIDPARAM 0x80070057u
#define DSERR_BADFORMAT 0x88780064u
#define DSERR_CONTROLUNAVAIL 0x8878001eu
#define DSERR_UNSUPPORTED 0x80004001u
#define DSERR_OUTOFMEMORY 0x8007000eu
#define DSERR_ALREADYINITIALIZED 0x88780082u
enum { STATE, ADDRESS, BYTES, CHANNELS, CURSOR, HEARTBEAT, VOLUME, RESERVED };
static volatile int32_t control[SLOTS * 8] __attribute__((aligned(16)));
typedef struct Buffer {
    int used; uint32_t refs, ring, bytes, channels, align, flags, looping;
    int32_t volume, pan, frequency;
    int32_t seen_heartbeat; double seen_at;
    int host_stream; uint32_t last_cursor; uint64_t played, written; /* host streams only */
} Buffer;
static Buffer buffers[SLOTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t (*alloc_ring)(uint32_t), factory_refs = 1, property_refs = 1;
static int (*free_ring)(uint32_t);
static int ready;

static const uint8_t iid_unknown[16] = {0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static const uint8_t iid_factory[16] = {1,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static const uint8_t iid_buffer[16] = {0x85,0xfa,0x9a,0x27,0x81,0x49,0xce,0x11,0xa5,0x21,0x00,0x20,0xaf,0x0b,0xe5,0x60};
static const uint8_t iid_buffer8[16] = {0x58,0x03,0x01,0x6c,0x69,0x6b,0x1e,0x45,0xa0,0x00,0x0c,0x15,0xf2,0x6b,0x5d,0x7c};
static const uint8_t iid_property_set[16] = {0x30,0xac,0xef,0x31,0x5c,0x51,0xd0,0x11,0xa9,0xaa,0x00,0xaa,0x00,0x61,0xbe,0x93};

static void ret(CPU *c, unsigned n, uint32_t v) { c->eax = v; c->esp += 4 * (n + 1); }
static int range(uint32_t p, uint64_t n) { return p && (uint64_t)p + n <= MOVIES_DSOUND_SPACE; }
static int same(uint32_t p, const uint8_t *id) { return range(p, 16) && !memcmp(HP(p), id, 16); }
static double now_ms(void) {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
#endif
}
static int32_t load(int index) { return __atomic_load_n(&control[index], __ATOMIC_SEQ_CST); }
static void store(int index, int32_t v) { __atomic_store_n(&control[index], v, __ATOMIC_SEQ_CST); }
uint32_t movies_dsound_control_address(void) { return (uint32_t)(uintptr_t)control; }
volatile int32_t *movies_dsound_control_block(void) { return control; }
#ifdef __EMSCRIPTEN__
/* Runs on the game worker: hand the shared Wasm memory to the page's AudioWorklet. */
EM_JS(void, announce_renderer, (uint32_t control_address, uint32_t slots), {
    if (typeof postMessage === 'function' && typeof HEAPU8 !== 'undefined' && HEAPU8.buffer instanceof SharedArrayBuffer)
        postMessage({type: 'dsound', memory: HEAPU8.buffer, control: control_address, slots: slots});
});
#endif
static Buffer *buffer_of(uint32_t self, unsigned *slot) {
    if (self < OBJ_BUFFERS || self >= OBJ_BUFFERS + 32 * SLOTS || (self - OBJ_BUFFERS) % 32) return NULL;
    unsigned i = (self - OBJ_BUFFERS) / 32; if (!buffers[i].used || buffers[i].host_stream) return NULL;
    if (slot) *slot = i; return &buffers[i];
}
/* Locked: the renderer's cursor, or wall-clock progress while it is silent. */
static uint32_t cursor_frames(Buffer *b, unsigned slot) {
    uint32_t frames = b->bytes / b->align, cursor = (uint32_t)load(slot * 8 + CURSOR) % frames;
    int32_t beat = load(slot * 8 + HEARTBEAT); double t = now_ms();
    if (beat != b->seen_heartbeat) { b->seen_heartbeat = beat; b->seen_at = t; return cursor; }
    if (load(slot * 8 + STATE) == 1 && t - b->seen_at > 60) {
        uint32_t advance = (uint32_t)((t - b->seen_at) * 44.1);
        cursor = (uint32_t)(((uint64_t)cursor + advance) % frames);
        store(slot * 8 + CURSOR, (int32_t)cursor); b->seen_at = t;
    }
    return cursor;
}
static uint32_t object_query(CPU *c, uint32_t self, uint32_t iid, uint32_t out, int ok) {
    (void)c; (void)iid;
    if (!range(out, 4)) return E_POINTER;
    W32(out, ok ? self : 0); return ok ? 0 : E_NOINTERFACE;
}
/* ---- IDirectSoundBuffer ---- */
static void b_query(CPU *c) {
    uint32_t self = ARG(0), iid = ARG(1);
    pthread_mutex_lock(&lock); Buffer *b = buffer_of(self, NULL);
    int ok = b && (same(iid, iid_unknown) || same(iid, iid_buffer) || same(iid, iid_buffer8)); if (ok) b->refs++;
    pthread_mutex_unlock(&lock); ret(c, 3, b ? object_query(c, self, iid, ARG(2), ok) : E_POINTER);
}
static void b_addref(CPU *c) { pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), NULL); uint32_t r = b ? ++b->refs : 0; pthread_mutex_unlock(&lock); ret(c, 1, r); }
static void b_release(CPU *c) {
    unsigned slot; uint32_t ring = 0, r = 0;
    pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), &slot);
    if (b && (r = --b->refs) == 0) {
        store(slot * 8 + STATE, 0); store(slot * 8 + ADDRESS, 0); store(slot * 8 + BYTES, 0);
        ring = b->ring; memset(b, 0, sizeof *b);
    }
    pthread_mutex_unlock(&lock);
    if (ring && free_ring) free_ring(ring);
    ret(c, 1, r);
}
static void b_caps(CPU *c) {
    uint32_t p = ARG(1);
    pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), NULL); Buffer copy = b ? *b : (Buffer){0}; pthread_mutex_unlock(&lock);
    if (!b) { ret(c, 2, E_POINTER); return; }
    if (!range(p, 20) || R32(p) < 20) { ret(c, 2, INVALIDPARAM); return; }
    W32(p + 4, copy.flags | 0x200 /* LOCSOFTWARE */); W32(p + 8, copy.bytes); W32(p + 12, 0); W32(p + 16, 0); ret(c, 2, 0);
}
static void b_position(CPU *c) {
    uint32_t play = ARG(1), write = ARG(2); unsigned slot;
    pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), &slot);
    uint32_t p = 0, w = 0;
    if (b) { p = cursor_frames(b, slot) * b->align; w = (p + 64 * b->align) % b->bytes; }
    pthread_mutex_unlock(&lock);
    if (!b) { ret(c, 3, E_POINTER); return; }
    {   /* Diagnostic: statistics of the ring the guest mixed (low zero-crossing rate
         * = tonal audio, ~0.5 = noise). Logged a few times, every ~4 s. */
        static double last; static unsigned logs; double t = now_ms();
        if (logs < 3 && t - last > 4000) {
            last = t; logs++;
            uint32_t ring = b->ring, n = b->bytes / 2, crossings = 0, nonzero = 0; double energy = 0; int16_t lo = 0, hi = 0;
            for (uint32_t i = 0; i + 2 < n; i += 2) {
                int16_t a = (int16_t)R16(ring + 2 * i), d = (int16_t)R16(ring + 2 * i + 4);
                if ((a < 0) != (d < 0)) crossings++; if (a) nonzero++; energy += (double)a * a; if (a < lo) lo = a; if (a > hi) hi = a;
            }
            fprintf(stderr, "[dsound] ring stats: left rms=%.0f min=%d max=%d nonzero=%u/%u zcr=%.3f first=%d,%d,%d,%d,%d,%d,%d,%d\n",
                sqrt(energy / (n / 2)), lo, hi, nonzero, n / 2, (double)crossings / (n / 2),
                (int16_t)R16(ring), (int16_t)R16(ring + 2), (int16_t)R16(ring + 4), (int16_t)R16(ring + 6),
                (int16_t)R16(ring + 8), (int16_t)R16(ring + 10), (int16_t)R16(ring + 12), (int16_t)R16(ring + 14));
        }
    }
    if (play && range(play, 4)) W32(play, p);
    if (write && range(write, 4)) W32(write, w);
    ret(c, 3, 0);
}
static void b_format(CPU *c) {
    uint32_t out = ARG(1), size = ARG(2), written = ARG(3);
    pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), NULL); Buffer copy = b ? *b : (Buffer){0}; pthread_mutex_unlock(&lock);
    if (!b) { ret(c, 4, E_POINTER); return; }
    if (written && range(written, 4)) W32(written, 18);
    if (out) {
        if (size < 16 || !range(out, size < 18 ? size : 18)) { ret(c, 4, INVALIDPARAM); return; }
        W16(out, 1); W16(out + 2, (uint16_t)copy.channels); W32(out + 4, 44100); W32(out + 8, 44100 * copy.align);
        W16(out + 12, (uint16_t)copy.align); W16(out + 14, 16); if (size >= 18) W16(out + 16, 0);
    }
    ret(c, 4, 0);
}
static void b_get_volume(CPU *c) { pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), NULL); int32_t v = b ? b->volume : 0; pthread_mutex_unlock(&lock); if (b && range(ARG(1), 4)) W32(ARG(1), (uint32_t)v); ret(c, 2, b ? 0 : E_POINTER); }
static void b_get_pan(CPU *c) { pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), NULL); int32_t v = b ? b->pan : 0; pthread_mutex_unlock(&lock); if (b && range(ARG(1), 4)) W32(ARG(1), (uint32_t)v); ret(c, 2, b ? 0 : E_POINTER); }
static void b_get_frequency(CPU *c) { pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), NULL); int32_t v = b ? b->frequency : 0; pthread_mutex_unlock(&lock); if (b && range(ARG(1), 4)) W32(ARG(1), (uint32_t)v); ret(c, 2, b ? 0 : E_POINTER); }
static void b_status(CPU *c) {
    unsigned slot;
    pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), &slot);
    uint32_t s = b && load(slot * 8 + STATE) == 1 ? 1u | (b->looping ? 4u : 0u) : 0;
    pthread_mutex_unlock(&lock);
    if (b && range(ARG(1), 4)) W32(ARG(1), s); ret(c, 2, b ? 0 : E_POINTER);
}
static void b_initialize(CPU *c) { ret(c, 3, DSERR_ALREADYINITIALIZED); }
static void b_lock(CPU *c) {
    uint32_t offset = ARG(1), bytes = ARG(2), p1 = ARG(3), n1 = ARG(4), p2 = ARG(5), n2 = ARG(6), flags = ARG(7); unsigned slot;
    pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), &slot); Buffer copy = b ? *b : (Buffer){0};
    if (b && (flags & 1)) offset = (cursor_frames(b, slot) * b->align + 64 * b->align) % b->bytes; /* FROMWRITECURSOR */
    pthread_mutex_unlock(&lock);
    if (!b) { ret(c, 8, E_POINTER); return; }
    if (flags & 2) { offset = 0; bytes = copy.bytes; } /* ENTIREBUFFER */
    if (!range(p1, 4) || !range(n1, 4) || offset >= copy.bytes || !bytes || bytes > copy.bytes) { ret(c, 8, INVALIDPARAM); return; }
    uint32_t first = copy.bytes - offset < bytes ? copy.bytes - offset : bytes, second = bytes - first;
    W32(p1, copy.ring + offset); W32(n1, first);
    if (range(p2, 4)) W32(p2, second ? copy.ring : 0);
    if (range(n2, 4)) W32(n2, second);
    ret(c, 8, 0);
}
static void b_play(CPU *c) {
    unsigned slot;
    pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), &slot);
    if (b) { b->looping = ARG(3) & 1; b->seen_at = now_ms(); b->seen_heartbeat = load(slot * 8 + HEARTBEAT); store(slot * 8 + STATE, 1); }
    pthread_mutex_unlock(&lock);
    if (b) { static unsigned logs; if (logs++ < 4) fprintf(stderr, "[dsound] buffer %u playing: %u bytes, %u ch, looping=%u\n", slot, b->bytes, b->channels, ARG(3) & 1); }
    ret(c, 4, b ? 0 : E_POINTER);
}
static void b_set_position(CPU *c) {
    unsigned slot;
    pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), &slot);
    if (b) store(slot * 8 + CURSOR, (int32_t)((ARG(1) % b->bytes) / b->align));
    pthread_mutex_unlock(&lock); ret(c, 2, b ? 0 : E_POINTER);
}
static void b_set_format(CPU *c) { ret(c, 2, DSERR_UNSUPPORTED); }
static void b_set_volume(CPU *c) {
    unsigned slot; int32_t v = (int32_t)ARG(1);
    if (v > 0 || v < -10000) { ret(c, 2, INVALIDPARAM); return; }
    pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), &slot);
    if (b) { b->volume = v; store(slot * 8 + VOLUME, (int32_t)lrint(10000.0 * pow(10.0, v / 2000.0))); }
    pthread_mutex_unlock(&lock); ret(c, 2, b ? 0 : E_POINTER);
}
static void b_set_pan(CPU *c) { pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), NULL); if (b) b->pan = (int32_t)ARG(1); pthread_mutex_unlock(&lock); ret(c, 2, b ? 0 : E_POINTER); }
static void b_set_frequency(CPU *c) { pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), NULL); if (b) b->frequency = (int32_t)ARG(1); pthread_mutex_unlock(&lock); ret(c, 2, b ? 0 : E_POINTER); }
static void b_stop(CPU *c) {
    unsigned slot;
    pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), &slot); if (b) store(slot * 8 + STATE, 0);
    pthread_mutex_unlock(&lock); ret(c, 1, b ? 0 : E_POINTER);
}
static void b_unlock(CPU *c) { pthread_mutex_lock(&lock); Buffer *b = buffer_of(ARG(0), NULL); pthread_mutex_unlock(&lock); ret(c, 5, b ? 0 : E_POINTER); }
static void b_restore(CPU *c) { ret(c, 1, 0); }
static void b_unsupported3(CPU *c) { ret(c, 3, DSERR_UNSUPPORTED); }
static void b_unsupported4(CPU *c) { ret(c, 4, DSERR_UNSUPPORTED); }
static void b_unsupported5(CPU *c) { ret(c, 5, DSERR_UNSUPPORTED); }

uint32_t movies_dsound_create_buffer(uint32_t desc, uint32_t out) {
    if (!range(out, 4)) return E_POINTER;
    W32(out, 0);
    if (!ready) return DSERR_UNSUPPORTED;
    if (!range(desc, 20) || (R32(desc) != 20 && R32(desc) != 36)) return INVALIDPARAM;
    uint32_t flags = R32(desc + 4), bytes = R32(desc + 8), format = R32(desc + 16);
    if (flags & 1) return DSERR_UNSUPPORTED;                 /* primary buffer */
    if (flags & 0x10) return DSERR_CONTROLUNAVAIL;          /* CTRL3D */
    if (!range(format, 16)) return INVALIDPARAM;
    uint32_t tag = R16(format), ch = R16(format + 2), rate = R32(format + 4), align = R16(format + 12), bits = R16(format + 14);
    if (tag != 1 || bits != 16 || (ch != 1 && ch != 2) || align != ch * 2 || rate != 44100) {
        fprintf(stderr, "[dsound] CreateSoundBuffer format tag=%u ch=%u rate=%u bits=%u unsupported\n", tag, ch, rate, bits);
        return DSERR_BADFORMAT;
    }
    if (bytes < 4 * align || bytes > (8u << 20) || bytes % align) return INVALIDPARAM;
    uint32_t ring = alloc_ring ? alloc_ring(bytes) : 0;
    if (!ring) return DSERR_OUTOFMEMORY;
    memset(HP(ring), 0, bytes);
    pthread_mutex_lock(&lock);
    unsigned i = 0; while (i < SLOTS && buffers[i].used) i++;
    if (i == SLOTS) { pthread_mutex_unlock(&lock); if (free_ring) free_ring(ring); return DSERR_OUTOFMEMORY; }
    buffers[i] = (Buffer){1, 1, ring, bytes, ch, align, flags, 0, 0, 0, 44100, 0, 0};
    store(i * 8 + STATE, 0); store(i * 8 + CURSOR, 0); store(i * 8 + VOLUME, 10000);
    store(i * 8 + CHANNELS, (int32_t)ch); store(i * 8 + BYTES, (int32_t)bytes);
    store(i * 8 + ADDRESS, (int32_t)(uint32_t)(uintptr_t)HP(ring));
    pthread_mutex_unlock(&lock);
    W32(out, OBJ_BUFFERS + 32 * i);
    fprintf(stderr, "[dsound] secondary buffer %u: %u bytes, %u ch, flags=%#x\n", i, bytes, ch, flags);
    return 0;
}
/* ---- Host-owned streaming voices (movie audio): 44.1 kHz stereo PCM16 rings fed
 * by host code, played by the same renderer. Not reachable from guest COM. ---- */
static uint64_t stream_progress(Buffer *b, unsigned slot) {
    uint32_t frames = b->bytes / b->align, cursor = cursor_frames(b, slot);
    b->played += (cursor + frames - b->last_cursor) % frames; b->last_cursor = cursor;
    if (b->written < b->played) {          /* underrun: silence stale data, resync */
        memset(HP(b->ring), 0, b->bytes); b->written = b->played;
    }
    return b->played;
}
int movies_dsound_stream_open(uint32_t bytes) {
    if (!ready || bytes < 16 || bytes % 4 || bytes > (4u << 20)) return -1;
    uint32_t ring = alloc_ring ? alloc_ring(bytes) : 0; if (!ring) return -1;
    memset(HP(ring), 0, bytes);
    pthread_mutex_lock(&lock);
    unsigned i = 0; while (i < SLOTS && buffers[i].used) i++;
    if (i == SLOTS) { pthread_mutex_unlock(&lock); if (free_ring) free_ring(ring); return -1; }
    buffers[i] = (Buffer){.used = 1, .refs = 1, .ring = ring, .bytes = bytes, .channels = 2, .align = 4, .frequency = 44100, .host_stream = 1};
    store(i * 8 + STATE, 0); store(i * 8 + CURSOR, 0); store(i * 8 + VOLUME, 10000);
    store(i * 8 + CHANNELS, 2); store(i * 8 + BYTES, (int32_t)bytes); store(i * 8 + ADDRESS, (int32_t)(uint32_t)(uintptr_t)HP(ring));
    pthread_mutex_unlock(&lock); return (int)i;
}
static Buffer *stream(int slot) { return slot >= 0 && slot < SLOTS && buffers[slot].used && buffers[slot].host_stream ? &buffers[slot] : NULL; }
void movies_dsound_stream_close(int slot) {
    uint32_t ring = 0;
    pthread_mutex_lock(&lock); Buffer *b = stream(slot);
    if (b) { store(slot * 8 + STATE, 0); store(slot * 8 + ADDRESS, 0); store(slot * 8 + BYTES, 0); ring = b->ring; memset(b, 0, sizeof *b); }
    pthread_mutex_unlock(&lock); if (ring && free_ring) free_ring(ring);
}
void movies_dsound_stream_play(int slot, int playing) {
    pthread_mutex_lock(&lock); Buffer *b = stream(slot);
    if (b) {
        if (playing && load(slot * 8 + STATE) != 1) { b->seen_at = now_ms(); b->seen_heartbeat = load(slot * 8 + HEARTBEAT); }
        if (!playing) stream_progress(b, (unsigned)slot);
        store(slot * 8 + STATE, playing ? 1 : 0);
    }
    pthread_mutex_unlock(&lock);
}
void movies_dsound_stream_reset(int slot) {
    pthread_mutex_lock(&lock); Buffer *b = stream(slot);
    if (b) { store(slot * 8 + STATE, 0); memset(HP(b->ring), 0, b->bytes); store(slot * 8 + CURSOR, 0); b->last_cursor = 0; b->played = b->written = 0; }
    pthread_mutex_unlock(&lock);
}
void movies_dsound_stream_volume(int slot, int32_t hundredths_db) {
    if (hundredths_db > 0) hundredths_db = 0; if (hundredths_db < -10000) hundredths_db = -10000;
    pthread_mutex_lock(&lock); if (stream(slot)) store(slot * 8 + VOLUME, hundredths_db <= -10000 ? 0 : (int32_t)lrint(10000.0 * pow(10.0, hundredths_db / 2000.0))); pthread_mutex_unlock(&lock);
}
/* Bytes of free space ahead of the play cursor (writes beyond it would overwrite unplayed audio). */
uint32_t movies_dsound_stream_space(int slot) {
    pthread_mutex_lock(&lock); Buffer *b = stream(slot); uint32_t space = 0;
    if (b) { uint64_t played = stream_progress(b, (unsigned)slot), queued = (b->written - played) * 4; space = queued >= b->bytes ? 0 : b->bytes - (uint32_t)queued; }
    pthread_mutex_unlock(&lock); return space;
}
uint32_t movies_dsound_stream_write(int slot, const uint8_t *pcm, uint32_t bytes) {
    pthread_mutex_lock(&lock); Buffer *b = stream(slot); uint32_t done = 0;
    if (b) {
        uint64_t played = stream_progress(b, (unsigned)slot), queued = (b->written - played) * 4;
        uint32_t space = queued >= b->bytes ? 0 : b->bytes - (uint32_t)queued, n = (bytes < space ? bytes : space) & ~3u;
        while (done < n) {
            uint32_t at = (uint32_t)((b->written * 4) % b->bytes), chunk = b->bytes - at < n - done ? b->bytes - at : n - done;
            memcpy(HP(b->ring + at), pcm + done, chunk); done += chunk; b->written += chunk / 4;
        }
    }
    pthread_mutex_unlock(&lock); return done;
}
/* Frames the renderer has played since the last reset. */
uint64_t movies_dsound_stream_played(int slot) {
    pthread_mutex_lock(&lock); Buffer *b = stream(slot); uint64_t played = b ? stream_progress(b, (unsigned)slot) : 0; pthread_mutex_unlock(&lock); return played;
}

/* ---- IClassFactory / IKsPropertySet ---- */
static void f_query(CPU *c) { uint32_t iid = ARG(1); int ok = same(iid, iid_unknown) || same(iid, iid_factory); ret(c, 3, object_query(c, OBJ_FACTORY, iid, ARG(2), ok)); }
static void f_addref(CPU *c) { ret(c, 1, ++factory_refs); }
static void f_release(CPU *c) { ret(c, 1, factory_refs > 1 ? --factory_refs : 1); }
static void f_create(CPU *c) {
    uint32_t iid = ARG(2), out = ARG(3);
    if (!range(out, 4)) { ret(c, 4, E_POINTER); return; }
    if (ARG(1)) { W32(out, 0); ret(c, 4, 0x80040110u); return; } /* CLASS_E_NOAGGREGATION */
    int ok = same(iid, iid_property_set) || same(iid, iid_unknown);
    if (ok) property_refs++;
    ret(c, 4, object_query(c, OBJ_PROPERTY, iid, out, ok));
}
static void f_lock_server(CPU *c) { ret(c, 2, 0); }
static void p_query(CPU *c) { uint32_t iid = ARG(1); int ok = same(iid, iid_unknown) || same(iid, iid_property_set); ret(c, 3, object_query(c, OBJ_PROPERTY, iid, ARG(2), ok)); }
static void p_addref(CPU *c) { ret(c, 1, ++property_refs); }
static void p_release(CPU *c) { ret(c, 1, property_refs > 1 ? --property_refs : 1); }
static void p_get(CPU *c) { uint32_t got = ARG(7); if (range(got, 4)) W32(got, 0); ret(c, 8, E_NOTIMPL); }
static void p_set(CPU *c) { ret(c, 7, E_NOTIMPL); }
static void p_support(CPU *c) { uint32_t s = ARG(3); if (range(s, 4)) W32(s, 0); ret(c, 4, E_NOTIMPL); }
static void dll_get_class_object(CPU *c) {
    uint32_t iid = ARG(1), out = ARG(2);
    if (!ready || !range(out, 4)) { ret(c, 3, E_POINTER); return; }
    int ok = same(iid, iid_factory) || same(iid, iid_unknown);
    if (ok) factory_refs++;
    ret(c, 3, object_query(c, OBJ_FACTORY, iid, out, ok));
}

int movies_dsound_init(uint32_t (*alloc)(uint32_t), int (*release)(uint32_t)) {
    if ((uint64_t)MOVIES_DSOUND_BASE + 0x200 + 32 * SLOTS > MOVIES_DSOUND_SPACE || !g_mem) return 0;
    alloc_ring = alloc; free_ring = release;
    memset((void *)control, 0, sizeof control); memset(buffers, 0, sizeof buffers);
    memset(HP(MOVIES_DSOUND_BASE), 0, 0x200 + 32 * SLOTS);
    GuestFn methods[] = {
        b_query, b_addref, b_release, b_caps, b_position, b_format, b_get_volume, b_get_pan, b_get_frequency, b_status,
        b_initialize, b_lock, b_play, b_set_position, b_set_format, b_set_volume, b_set_pan, b_set_frequency, b_stop,
        b_unlock, b_restore, b_unsupported4, b_unsupported3, b_unsupported5,
        f_query, f_addref, f_release, f_create, f_lock_server,
        p_query, p_addref, p_release, p_get, p_set, p_support};
    for (unsigned i = 0; i < sizeof methods / sizeof *methods; i++) register_function(TRAMP + 16 * i, methods[i]);
    for (unsigned i = 0; i < 24; i++) W32(VT_BUFFER + 4 * i, TRAMP + 16 * i);
    for (unsigned i = 0; i < 5; i++) W32(VT_FACTORY + 4 * i, TRAMP + 16 * (24 + i));
    for (unsigned i = 0; i < 6; i++) W32(VT_PROPERTY + 4 * i, TRAMP + 16 * (29 + i));
    W32(OBJ_FACTORY, VT_FACTORY); W32(OBJ_PROPERTY, VT_PROPERTY);
    for (unsigned i = 0; i < SLOTS; i++) W32(OBJ_BUFFERS + 32 * i, VT_BUFFER);
    ready = 1;
#ifdef __EMSCRIPTEN__
    announce_renderer(movies_dsound_control_address(), SLOTS);
#endif
    return 1;
}
/* The Lionhead audio engine creates its mixer only when its CPU-detection byte
 * reports MMX (0x10d5e12, written by 0xc0fd60 from CPUID.1:EDX[23]). The
 * translated mixer's MMX code is supported; SSE/SSE2/3DNow stay reported absent
 * so the non-SSE2 mixer is selected. Called when the engine creates DirectSound,
 * which happens after detection and before the mixer check. */
void movies_dsound_enable_engine(void) {
    if (MOVIES_DSOUND_SPACE > 0x10d5e20u && R8(0x10d5e10u) && !R8(0x10d5e12u)) {
        W8(0x10d5e12u, 1); fprintf(stderr, "[dsound] audio engine MMX mixer enabled\n");
    }
}
ImportHandler *movies_dsound_handlers(uint32_t *count) {
    static ImportHandler handlers[] = {{"DSOUND.DLL!DllGetClassObject", dll_get_class_object}};
    *count = 1; return handlers;
}
