/* DirectShow movie playback for the game's own "Texture Renderer" filter.
 * Map: the game builds FilterGraph + its CBaseVideoRenderer + CLSID_WMAsfReader,
 * Render()s each reader output pin, then drives IMediaControl/IMediaPosition and
 * polls get_CurrentPosition every frame (no events, no clock). See
 * docs/research/static-startup-status.md "Movie playback".
 *
 * Here the reader is virtual: Load() hands the original .wmv to the browser FFmpeg
 * reader service, which decodes WMV3 to bottom-up BGR24 frames and WMAv2 to PCM.
 * Render(videoPin) connects to the guest renderer's input pin with an RGB24
 * VIDEOINFOHEADER type and our allocator; frames due by the graph clock are pushed
 * from get_CurrentPosition through the guest IMemInputPin::Receive. Movie audio
 * plays on a host-owned DirectSound voice with the graph's IBasicAudio volume.
 * https://learn.microsoft.com/en-us/windows/win32/directshow/ */
#define _POSIX_C_SOURCE 200809L
#include "movie-api.h"
#include "dsound-api.h"
#include "file-api.h"
#include "heap-api.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#ifndef MOVIES_MOVIE_SPACE
#define MOVIES_MOVIE_SPACE GUEST_SPACE
#endif
#define ARG(n) R32(c->esp + 4 + 4 * (n))
#define TRAMP 0x0e040000u
#define MOVIES 4
#define BLOCK 0x400u
#define VT (MOVIES_MOVIE_BASE + MOVIES * BLOCK)          /* vtables follow the blocks */
#define S_FALSE 1u
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_FAIL 0x80004005u
#define E_UNEXPECTED 0x8000ffffu
#define E_OUTOFMEMORY 0x8007000eu
#define VFW_E_NOT_CONNECTED 0x80040209u
#define VFW_E_SAMPLE_TIME_NOT_SET 0x80040249u
#define VFW_E_MEDIA_TIME_NOT_SET 0x80040251u
#define VFW_E_NOT_FOUND 0x80040216u
/* Object offsets inside a movie block; each object is {vtable, 0}. */
enum { O_CONTROL = 0x00, O_POSITION = 0x08, O_SEEKING = 0x10, O_AUDIO = 0x18, O_FILTER = 0x20, O_SOURCE = 0x28,
       O_ENUM = 0x30, O_VPIN = 0x38, O_APIN = 0x40, O_ALLOC = 0x48, O_SAMPLE = 0x50,
       O_MEDIATYPE = 0x60, O_VIH = 0xb0, O_SCRATCH = 0x200 };
enum { K_CONTROL, K_POSITION, K_SEEKING, K_AUDIO, K_FILTER, K_SOURCE, K_ENUM, K_PIN, K_ALLOC, K_SAMPLE, K_KINDS };
static const unsigned kind_offset[K_KINDS] = {O_CONTROL, O_POSITION, O_SEEKING, O_AUDIO, O_FILTER, O_SOURCE, O_ENUM, O_VPIN, O_ALLOC, O_SAMPLE};
#define VT_SIZE 0x60u

typedef struct Movie {
    int used, loaded, connected, has_audio, video_eos, audio_eos, state;  /* state: 0 stopped, 1 paused, 2 running */
    uint32_t graph, filter, in_pin, mem_pin, video_token, audio_token, sample_buf, frame_bytes, enum_index;
    unsigned width, height; double fps, duration, base_pos, run_start; long next_frame;
    int audio_slot; int32_t volume; uint32_t refs[K_KINDS];
    char path[260];
} Movie;
static Movie movies[MOVIES];
/* Weak fallbacks keep harnesses that link this file without file/heap modules linkable. */
__attribute__((weak)) int movies_file_read_all(const char *p, uint8_t **d, size_t *n, size_t limit) { (void)p; (void)limit; *d = NULL; *n = 0; return 0; }
__attribute__((weak)) uint32_t movies_local_alloc(uint32_t bytes) { (void)bytes; return 0; }
static int ready;

static const uint8_t iid_unknown[16] = {0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static const uint8_t iid_persist[16] = {0x0c,0x01,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
#define DS_IID(x) {x,0x68,0xa8,0x56,0xd4,0x0a,0xce,0x11,0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}
static const uint8_t iid_control[16] = DS_IID(0xb1), iid_position[16] = DS_IID(0xb2), iid_basic_audio[16] = DS_IID(0xb3),
    iid_filter[16] = DS_IID(0x95), iid_media_filter[16] = DS_IID(0x99), iid_source[16] = DS_IID(0xa6),
    iid_pin[16] = DS_IID(0x91), iid_enum_pins[16] = DS_IID(0x92), iid_mem_input[16] = DS_IID(0x9d),
    iid_allocator[16] = DS_IID(0x9c), iid_sample[16] = DS_IID(0x9a), iid_graph[16] = DS_IID(0xa9), iid_filter_graph[16] = DS_IID(0x9f);
static const uint8_t iid_seeking[16] = {0x80,0x38,0xb7,0x36,0xc8,0xc2,0xcf,0x11,0x8b,0x46,0x00,0x80,0x5f,0x6c,0xef,0x60};
static const uint8_t clsid_asf_reader[16] = {0xa0,0x63,0x74,0x18,0xb7,0x5b,0xd3,0x11,0xac,0xbe,0x00,0x80,0xc7,0x5e,0x24,0x6e};
static const uint8_t mediatype_video[16] = {0x76,0x69,0x64,0x73,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
static const uint8_t subtype_rgb24[16] = {0x7d,0xeb,0x36,0xe4,0x4f,0x52,0xce,0x11,0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70};
static const uint8_t format_video_info[16] = {0x80,0x9f,0x58,0x05,0x56,0xc3,0xce,0x11,0xbf,0x01,0x00,0xaa,0x00,0x55,0x59,0x5a};
static const uint8_t time_format_media[16] = {0x74,0x55,0x78,0x7b,0x82,0x8c,0xcf,0x11,0xbc,0x0c,0x00,0xaa,0x00,0xac,0x74,0xf6};

/* COM stdcall: n counts the arguments after `this`; pops return address, this and n. */
static void ret(CPU *c, unsigned n, uint32_t v) { c->eax = v; c->esp += 4 * (n + 2); }
static int range(uint32_t p, uint64_t n) { return p && (uint64_t)p + n <= MOVIES_MOVIE_SPACE; }
static int same(uint32_t p, const uint8_t *id) { return range(p, 16) && !memcmp(HP(p), id, 16); }
static uint32_t base(unsigned i) { return MOVIES_MOVIE_BASE + i * BLOCK; }
static double now_ms(void) {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
#endif
}
/* Identify (movie, object offset) from a guest `this`. */
static Movie *movie_of(uint32_t self, unsigned *offset) {
    if (self < MOVIES_MOVIE_BASE || self >= MOVIES_MOVIE_BASE + MOVIES * BLOCK) return NULL;
    unsigned i = (self - MOVIES_MOVIE_BASE) / BLOCK; if (!movies[i].used) return NULL;
    if (offset) *offset = (self - MOVIES_MOVIE_BASE) % BLOCK;
    return &movies[i];
}
static Movie *movie_for_graph(uint32_t graph) {
    for (unsigned i = 0; i < MOVIES; i++) if (movies[i].used && movies[i].graph == graph) return &movies[i];
    return NULL;
}
static unsigned index_of(Movie *m) { return (unsigned)(m - movies); }
static Movie *allocate_movie(void) {
    for (unsigned i = 0; i < MOVIES; i++) if (!movies[i].used) {
        Movie *m = &movies[i]; memset(m, 0, sizeof *m); m->used = 1; m->audio_slot = -1;
        uint32_t b = base(i); memset(HP(b), 0, BLOCK);
        for (unsigned k = 0; k < K_KINDS; k++) W32(b + kind_offset[k], VT + k * VT_SIZE);
        W32(b + O_APIN, VT + K_PIN * VT_SIZE);
        return m;
    }
    return NULL;
}
static void log_once(const char *what) { static unsigned logs; if (logs++ < 32) fprintf(stderr, "[movie] %s\n", what); }

/* ---- nested guest COM calls (main thread, inside a host import) ---- */
static uint32_t gcall(CPU *c, uint32_t object, unsigned slot, unsigned n, const uint32_t *extra) {
    if (!range(object, 4) || !range(R32(object), 4 * (slot + 1))) { guest_trap(c, R32(c->esp), "invalid guest COM object for movie graph"); return E_POINTER; }
    uint32_t address = R32(R32(object) + 4 * slot); GuestFn fn = lookup_function(address);
    if (!fn) { guest_trap(c, address, "untranslated guest DirectShow method"); return E_NOTIMPL; }
    if (n > 7 || c->esp < 4096 || !range(c->esp - 4096, 4096)) { guest_trap(c, address, "movie callback guest stack invalid"); return E_POINTER; }
    CPU nested = *c; nested.esp -= 512;
    uint32_t before = nested.esp, args[8] = {object};
    for (unsigned i = 0; i < n; i++) args[i + 1] = extra[i];
    guest_call(&nested, fn, (int)n + 1, args);
    if (nested.esp != before) { guest_trap(c, address, "guest DirectShow method violated stdcall"); return E_FAIL; }
    return nested.eax;
}

/* ---- browser decode bridge (Module.moviesMedia* provider, synchronous) ---- */
#ifdef __EMSCRIPTEN__
EM_JS(uint32_t, movie_video_open_js, (const uint8_t *data, uint32_t size, uint32_t *out), {
    if (typeof Module.moviesMediaOpenVideo !== 'function') return 0;
    var token = Module.moviesMediaCreateReader(size); if (!token) return 0;
    for (var at = 0; at < size; at += 2097152) {
        var n = Math.min(2097152, size - at);
        if (!Module.moviesMediaAppendReaderInput(token, at, HEAPU8.slice(data + at, data + at + n))) { Module.moviesMediaReleaseReader(token); return 0; }
    }
    var meta = Module.moviesMediaOpenVideo(token);
    if (!meta) { Module.moviesMediaReleaseReader(token); return 0; }
    HEAPU32[(out >> 2)] = meta.width; HEAPU32[(out >> 2) + 1] = meta.height; HEAPU32[(out >> 2) + 2] = Math.round(meta.fps * 1000);
    HEAPU32[(out >> 2) + 3] = Math.round(meta.duration100ns / 10000); HEAPU32[(out >> 2) + 4] = meta.hasAudio ? 1 : 0;
    return token;
});
EM_JS(uint32_t, movie_audio_open_js, (const uint8_t *data, uint32_t size), {
    var token = Module.moviesMediaCreateReader(size); if (!token) return 0;
    for (var at = 0; at < size; at += 2097152) {
        var n = Math.min(2097152, size - at);
        if (!Module.moviesMediaAppendReaderInput(token, at, HEAPU8.slice(data + at, data + at + n))) { Module.moviesMediaReleaseReader(token); return 0; }
    }
    if (!Module.moviesMediaOpenReader(token)) { Module.moviesMediaReleaseReader(token); return 0; }
    return token;
});
EM_JS(int, movie_video_read_js, (uint32_t token, uint8_t *dst, uint32_t capacity), {
    var r = Module.moviesMediaReadVideoFrame(token); if (!r) return -1; if (r.eos) return 0;
    if (r.bytes.length > capacity) return -1; HEAPU8.set(r.bytes, dst); return 1;
});
EM_JS(int, movie_video_seek_js, (uint32_t token, double seconds), { return Module.moviesMediaSeekVideo(token, seconds) ? 1 : 0; });
EM_JS(int, movie_audio_read_js, (uint32_t token, uint8_t *dst, uint32_t capacity), {
    var s = Module.moviesMediaReadReaderSample(token, capacity); if (!s) return -1; if (s.eos === true) return 0;
    HEAPU8.set(s.bytes, dst); return s.bytes.length;
});
EM_JS(int, movie_audio_seek_js, (uint32_t token, double start100ns), { return Module.moviesMediaSetReaderRange(token, start100ns, null) ? 1 : 0; });
EM_JS(void, movie_release_js, (uint32_t token), { Module.moviesMediaReleaseReader(token); });
#else
static uint32_t movie_video_open_js(const uint8_t *d, uint32_t n, uint32_t *o) { (void)d; (void)n; (void)o; return 0; }
static uint32_t movie_audio_open_js(const uint8_t *d, uint32_t n) { (void)d; (void)n; return 0; }
static int movie_video_read_js(uint32_t t, uint8_t *d, uint32_t n) { (void)t; (void)d; (void)n; return -1; }
static int movie_video_seek_js(uint32_t t, double s) { (void)t; (void)s; return 0; }
static int movie_audio_read_js(uint32_t t, uint8_t *d, uint32_t n) { (void)t; (void)d; (void)n; return -1; }
static int movie_audio_seek_js(uint32_t t, double s) { (void)t; (void)s; return 0; }
static void movie_release_js(uint32_t t) { (void)t; }
#endif

/* ---- clock and frame pump ---- */
static double position(Movie *m) {
    double pos = m->base_pos;
    if (m->state == 2) pos += (now_ms() - m->run_start) / 1000.0;
    return pos > m->duration ? m->duration : pos < 0 ? 0 : pos;
}
static void seek(Movie *m, double t) {
    if (t < 0) t = 0; if (t > m->duration) t = m->duration;
    m->base_pos = t; m->run_start = now_ms();
    if (m->video_token) { movie_video_seek_js(m->video_token, t); m->next_frame = lround(t * m->fps); m->video_eos = 0; }
    if (m->audio_token) { movie_audio_seek_js(m->audio_token, t * 1e7); m->audio_eos = 0; if (m->audio_slot >= 0) { movies_dsound_stream_reset(m->audio_slot); if (m->state == 2) movies_dsound_stream_play(m->audio_slot, 1); } }
}
static void feed_audio(Movie *m) {
    if (m->audio_slot < 0 || !m->audio_token || m->audio_eos || m->state != 2) return;
    static uint8_t chunk[8192];
    for (unsigned i = 0; i < 12 && movies_dsound_stream_space(m->audio_slot) >= sizeof chunk; i++) {
        int n = movie_audio_read_js(m->audio_token, chunk, sizeof chunk);
        if (n <= 0) { m->audio_eos = 1; break; }
        movies_dsound_stream_write(m->audio_slot, chunk, (uint32_t)n);
    }
}
static void pump(CPU *c, Movie *m) {
    if (m->state != 2) return;
    feed_audio(m);
    if (!m->connected || m->video_eos || !m->sample_buf) return;
    long due = (long)floor(position(m) * m->fps);
    if (due < m->next_frame) return;
    if (due - m->next_frame > (long)(m->fps * 2)) {            /* far behind: jump instead of decoding every frame */
        movie_video_seek_js(m->video_token, due / m->fps); m->next_frame = due;
    }
    int got = 0;
    for (unsigned i = 0; i < 4 && m->next_frame <= due; i++) {
        int r = movie_video_read_js(m->video_token, HP(m->sample_buf), m->frame_bytes);
        if (r <= 0) { m->video_eos = 1; if (r < 0) log_once("video frame read failed"); break; }
        m->next_frame++; got = 1;
    }
    if (got) { uint32_t sample = base(index_of(m)) + O_SAMPLE; gcall(c, m->mem_pin, 6, 1, &sample); }
}

/* ---- generic IUnknown on our objects ---- */
static uint32_t interface_for(Movie *m, unsigned offset, uint32_t iid) {
    uint32_t b = base(index_of(m));
    if (offset <= O_AUDIO) {                                  /* graph facets */
        if (same(iid, iid_control)) return b + O_CONTROL;
        if (same(iid, iid_position)) return b + O_POSITION;
        if (same(iid, iid_seeking)) return b + O_SEEKING;
        if (same(iid, iid_basic_audio)) return b + O_AUDIO;
        if (same(iid, iid_unknown)) return b + offset;
        return 0;
    }
    if (offset == O_FILTER || offset == O_SOURCE) {
        if (same(iid, iid_unknown) || same(iid, iid_filter) || same(iid, iid_media_filter) || same(iid, iid_persist)) return b + O_FILTER;
        if (same(iid, iid_source)) return b + O_SOURCE;
        return 0;
    }
    if (offset == O_ENUM) return same(iid, iid_unknown) || same(iid, iid_enum_pins) ? b + O_ENUM : 0;
    if (offset == O_VPIN || offset == O_APIN) return same(iid, iid_unknown) || same(iid, iid_pin) ? b + offset : 0;
    if (offset == O_ALLOC) return same(iid, iid_unknown) || same(iid, iid_allocator) ? b + O_ALLOC : 0;
    if (offset == O_SAMPLE) return same(iid, iid_unknown) || same(iid, iid_sample) ? b + O_SAMPLE : 0;
    return 0;
}
static void m_query(CPU *c) {
    unsigned off; Movie *m = movie_of(ARG(0), &off); uint32_t out = ARG(2);
    if (!m || !range(out, 4)) { ret(c, 2, E_POINTER); return; }
    uint32_t p = interface_for(m, off, ARG(1));
    if (!p && off <= O_AUDIO && (same(ARG(1), iid_graph) || same(ARG(1), iid_filter_graph))) p = m->graph; /* back to the graph (no extra reference) */
    W32(out, p); ret(c, 2, p ? 0 : E_NOINTERFACE);
}
static void m_addref(CPU *c) { ret(c, 0, 2); }
static void m_release(CPU *c) { ret(c, 0, 1); }      /* lifetimes follow the graph */
static void dispatch_count(CPU *c) { if (range(ARG(1), 4)) W32(ARG(1), 0); ret(c, 1, 0); }
static void dispatch_info(CPU *c) { if (range(ARG(3), 4)) W32(ARG(3), 0); ret(c, 3, E_NOTIMPL); }
static void dispatch_names(CPU *c) { ret(c, 5, E_NOTIMPL); }
static void dispatch_invoke(CPU *c) { ret(c, 8, E_NOTIMPL); }
static void notimpl0(CPU *c) { ret(c, 0, E_NOTIMPL); }
static void notimpl1(CPU *c) { ret(c, 1, E_NOTIMPL); }
static void notimpl2(CPU *c) { ret(c, 2, E_NOTIMPL); }
static void notimpl3(CPU *c) { ret(c, 3, E_NOTIMPL); }
static void notimpl4(CPU *c) { ret(c, 4, E_NOTIMPL); }
static void notimpl5(CPU *c) { ret(c, 5, E_NOTIMPL); }
static void ok1(CPU *c) { ret(c, 1, 0); }
static void ok2(CPU *c) { ret(c, 2, 0); }
static void ok3(CPU *c) { ret(c, 3, 0); }
static void ok6(CPU *c) { ret(c, 6, 0); }
static void false1(CPU *c) { ret(c, 1, S_FALSE); }

/* ---- IMediaControl ---- */
static void filter_state(CPU *c, Movie *m, int target) {
    if (!m->filter) return;
    if (target == 0) gcall(c, m->filter, 4, 0, NULL);                           /* Stop */
    else if (target == 1) gcall(c, m->filter, 5, 0, NULL);                      /* Pause */
    else { if (m->state == 0) gcall(c, m->filter, 5, 0, NULL); uint32_t t[2] = {0, 0}; gcall(c, m->filter, 6, 2, t); } /* Run(0) */
}
static void trace(Movie *m, const char *what) {
    static unsigned logs; if (logs++ < 16) fprintf(stderr, "[movie] %s %s state=%d pos=%.2f/%.2f frame=%ld eos=%d\n", what, m->path, m->state, position(m), m->duration, m->next_frame, m->video_eos);
}
static void mc_run(CPU *c) {
    Movie *m = movie_of(ARG(0), NULL); if (!m) { ret(c, 0, E_POINTER); return; }
    trace(m, "Run");
    if (m->state != 2) {
        filter_state(c, m, 2); m->run_start = now_ms(); m->state = 2;
        if (m->audio_slot >= 0) { movies_dsound_stream_play(m->audio_slot, 1); feed_audio(m); }
        pump(c, m);
    }
    ret(c, 0, 0);
}
static void mc_pause(CPU *c) {
    Movie *m = movie_of(ARG(0), NULL); if (!m) { ret(c, 0, E_POINTER); return; }
    if (m->state == 2) m->base_pos = position(m);
    if (m->state != 1) { filter_state(c, m, 1); m->state = 1; if (m->audio_slot >= 0) movies_dsound_stream_play(m->audio_slot, 0); }
    ret(c, 0, 0);
}
static void mc_stop(CPU *c) {
    Movie *m = movie_of(ARG(0), NULL); if (!m) { ret(c, 0, E_POINTER); return; }
    trace(m, "Stop");
    if (m->state == 2) m->base_pos = position(m);
    if (m->state != 0) { filter_state(c, m, 0); m->state = 0; if (m->audio_slot >= 0) movies_dsound_stream_play(m->audio_slot, 0); }
    ret(c, 0, 0);
}
static void mc_state(CPU *c) {
    Movie *m = movie_of(ARG(0), NULL); uint32_t out = ARG(2);
    if (!m || !range(out, 4)) { ret(c, 2, E_POINTER); return; }
    W32(out, (uint32_t)m->state); ret(c, 2, 0);
}
/* ---- IMediaPosition (REFTIME seconds as double) ---- */
static void write_double(uint32_t p, double v) { if (range(p, 8)) WF64(p, v); }
static double arg_double(CPU *c, unsigned i) { uint64_t bits = (uint64_t)ARG(i) | ((uint64_t)ARG(i + 1) << 32); double d; memcpy(&d, &bits, 8); return d; }
static void mp_duration(CPU *c) { Movie *m = movie_of(ARG(0), NULL); if (!m) { ret(c, 1, E_POINTER); return; } write_double(ARG(1), m->duration); ret(c, 1, 0); }
static void mp_put_position(CPU *c) { Movie *m = movie_of(ARG(0), NULL); if (!m) { ret(c, 2, E_POINTER); return; } seek(m, arg_double(c, 1)); ret(c, 2, 0); }
static void mp_get_position(CPU *c) {
    Movie *m = movie_of(ARG(0), NULL); if (!m) { ret(c, 1, E_POINTER); return; }
    pump(c, m); write_double(ARG(1), position(m)); ret(c, 1, 0);
}
static void mp_stop_time(CPU *c) { Movie *m = movie_of(ARG(0), NULL); if (!m) { ret(c, 1, E_POINTER); return; } write_double(ARG(1), m->duration); ret(c, 1, 0); }
static void mp_zero(CPU *c) { write_double(ARG(1), 0); ret(c, 1, 0); }
static void mp_rate(CPU *c) { write_double(ARG(1), 1.0); ret(c, 1, 0); }
static void mp_can_seek(CPU *c) { if (range(ARG(1), 4)) W32(ARG(1), 0xffffffffu); ret(c, 1, 0); }
/* ---- IMediaSeeking (100 ns units) ---- */
static void write64(uint32_t p, int64_t v) { if (range(p, 8)) W64(p, (uint64_t)v); }
static void ms_caps(CPU *c) { if (range(ARG(1), 4)) W32(ARG(1), 0x1f); ret(c, 1, 0); }
static void ms_format(CPU *c) { if (range(ARG(1), 16)) memcpy(HP(ARG(1)), time_format_media, 16); ret(c, 1, 0); }
static void ms_is_format(CPU *c) { ret(c, 1, same(ARG(1), time_format_media) ? 0 : S_FALSE); }
static void ms_duration(CPU *c) { Movie *m = movie_of(ARG(0), NULL); if (!m) { ret(c, 1, E_POINTER); return; } write64(ARG(1), (int64_t)llround(m->duration * 1e7)); ret(c, 1, 0); }
static void ms_current(CPU *c) { Movie *m = movie_of(ARG(0), NULL); if (!m) { ret(c, 1, E_POINTER); return; } pump(c, m); write64(ARG(1), (int64_t)llround(position(m) * 1e7)); ret(c, 1, 0); }
static void ms_set_positions(CPU *c) {
    Movie *m = movie_of(ARG(0), NULL); uint32_t cur = ARG(1), flags = ARG(2);
    if (!m) { ret(c, 4, E_POINTER); return; }
    if (range(cur, 8) && (flags & 3)) {
        double t = (double)(int64_t)R64(cur) / 1e7;
        seek(m, (flags & 3) == 2 ? position(m) + t : t);
    }
    ret(c, 4, 0);
}
static void ms_get_positions(CPU *c) {
    Movie *m = movie_of(ARG(0), NULL); if (!m) { ret(c, 2, E_POINTER); return; }
    write64(ARG(1), (int64_t)llround(position(m) * 1e7)); write64(ARG(2), (int64_t)llround(m->duration * 1e7)); ret(c, 2, 0);
}
static void ms_available(CPU *c) { Movie *m = movie_of(ARG(0), NULL); if (!m) { ret(c, 2, E_POINTER); return; } write64(ARG(1), 0); write64(ARG(2), (int64_t)llround(m->duration * 1e7)); ret(c, 2, 0); }
static void ms_get_rate(CPU *c) { write_double(ARG(1), 1.0); ret(c, 1, 0); }
static void ms_preroll(CPU *c) { write64(ARG(1), 0); ret(c, 1, 0); }
/* ---- IBasicAudio (hundredths of dB) ---- */
static void ba_put_volume(CPU *c) {
    Movie *m = movie_of(ARG(0), NULL); if (!m) { ret(c, 1, E_POINTER); return; }
    m->volume = (int32_t)ARG(1); if (m->audio_slot >= 0) movies_dsound_stream_volume(m->audio_slot, m->volume); ret(c, 1, 0);
}
static void ba_get_volume(CPU *c) { Movie *m = movie_of(ARG(0), NULL); if (!m) { ret(c, 1, E_POINTER); return; } if (range(ARG(1), 4)) W32(ARG(1), (uint32_t)m->volume); ret(c, 1, 0); }
static void ba_get_balance(CPU *c) { if (range(ARG(1), 4)) W32(ARG(1), 0); ret(c, 1, 0); }

/* ---- virtual WM ASF Reader: IBaseFilter + IFileSourceFilter ---- */
static void f_class(CPU *c) { if (range(ARG(1), 16)) memcpy(HP(ARG(1)), clsid_asf_reader, 16); ret(c, 1, 0); }
static void f_state(CPU *c) { Movie *m = movie_of(ARG(0), NULL); if (range(ARG(2), 4)) W32(ARG(2), m ? (uint32_t)m->state : 0); ret(c, 2, 0); }
static void f_get_sync(CPU *c) { if (range(ARG(1), 4)) W32(ARG(1), 0); ret(c, 1, 0); }
static void f_enum_pins(CPU *c) {
    Movie *m = movie_of(ARG(0), NULL); if (!m || !range(ARG(1), 4)) { ret(c, 1, E_POINTER); return; }
    m->enum_index = 0; uint32_t e = base(index_of(m)) + O_ENUM; W32(ARG(1), e);
    ret(c, 1, 0);
}
static void f_find_pin(CPU *c) { if (range(ARG(2), 4)) W32(ARG(2), 0); ret(c, 2, VFW_E_NOT_FOUND); }
static void f_info(CPU *c) {
    uint32_t out = ARG(1); if (!range(out, 260)) { ret(c, 1, E_POINTER); return; }
    memset(HP(out), 0, 260); const char *name = "Asf Reader"; for (unsigned i = 0; name[i]; i++) W16(out + 2 * i, (uint8_t)name[i]);
    ret(c, 1, 0);
}
static uint32_t load_movie(Movie *m, const char *path) {
    uint8_t *data; size_t size;
    if (!movies_file_read_all(path, &data, &size, 64u << 20)) { fprintf(stderr, "[movie] cannot read %s\n", path); return 0x80070002u; }
    uint32_t meta[5] = {0};
    uint32_t token = movie_video_open_js(data, (uint32_t)size, meta);
    if (!token || !meta[0] || !meta[1] || !meta[2]) { free(data); fprintf(stderr, "[movie] video decoder unavailable for %s\n", path); return E_FAIL; }
    m->video_token = token; m->width = meta[0]; m->height = meta[1]; m->fps = meta[2] / 1000.0; m->duration = meta[3] / 1000.0;
    m->frame_bytes = m->width * m->height * 3; m->has_audio = meta[4] != 0;
    if (m->has_audio) {
        m->audio_token = movie_audio_open_js(data, (uint32_t)size);
        if (m->audio_token) m->audio_slot = movies_dsound_stream_open(176400); /* 1 s ring */
        if (m->audio_slot >= 0) movies_dsound_stream_volume(m->audio_slot, m->volume);
    }
    free(data);
    fprintf(stderr, "[movie] loaded %s: %ux%u %.2f fps %.2f s audio=%s\n", path, m->width, m->height, m->fps, m->duration,
            m->audio_slot >= 0 ? "yes" : m->has_audio ? "unavailable" : "none");
    m->loaded = 1; return 0;
}
static void s_load(CPU *c) {
    Movie *m = movie_of(ARG(0), NULL); uint32_t name = ARG(1);
    if (!m || !name) { ret(c, 2, E_POINTER); return; }
    if (m->loaded) { ret(c, 2, E_UNEXPECTED); return; }
    unsigned i = 0;
    for (; i < sizeof m->path - 1 && range(name + 2 * i, 2); i++) { uint16_t ch = R16(name + 2 * i); if (!ch) break; m->path[i] = ch < 128 ? (char)ch : '?'; }
    m->path[i] = 0;
    ret(c, 2, load_movie(m, m->path));
}
static void s_cur_file(CPU *c) { if (range(ARG(1), 4)) W32(ARG(1), 0); ret(c, 2, E_NOTIMPL); }
/* ---- IEnumPins ---- */
static void e_next(CPU *c) {
    Movie *m = movie_of(ARG(0), NULL); uint32_t count = ARG(1), out = ARG(2), fetched = ARG(3);
    if (!m || !range(out, 4)) { ret(c, 3, E_POINTER); return; }
    uint32_t pins[2] = {base(index_of(m)) + O_VPIN, base(index_of(m)) + O_APIN}, total = m->has_audio && m->audio_slot >= 0 ? 2 : 1, n = 0;
    while (n < count && m->enum_index < total) { W32(out + 4 * n, pins[m->enum_index++]); n++; }
    if (range(fetched, 4)) W32(fetched, n);
    ret(c, 3, n == count ? 0 : S_FALSE);
}
static void e_skip(CPU *c) { Movie *m = movie_of(ARG(0), NULL); if (m) m->enum_index += ARG(1); ret(c, 1, 0); }
static void e_reset(CPU *c) { Movie *m = movie_of(ARG(0), NULL); if (m) m->enum_index = 0; ret(c, 0, 0); }
/* ---- output pins ---- */
static void fill_media_type(Movie *m) {
    uint32_t b = base(index_of(m)), mt = b + O_MEDIATYPE, vih = b + O_VIH;
    memset(HP(mt), 0, 72); memset(HP(vih), 0, 88);
    memcpy(HP(mt), mediatype_video, 16); memcpy(HP(mt + 16), subtype_rgb24, 16);
    W32(mt + 32, 1); W32(mt + 36, 0); W32(mt + 40, m->frame_bytes); memcpy(HP(mt + 44), format_video_info, 16);
    W32(mt + 64, 88); W32(mt + 68, vih);
    W32(vih + 8, m->width); W32(vih + 12, m->height); W32(vih + 24, m->width); W32(vih + 28, m->height);
    W32(vih + 32, (uint32_t)(m->frame_bytes * 8 * m->fps)); W64(vih + 40, (uint64_t)llround(1e7 / m->fps));
    W32(vih + 48, 40); W32(vih + 52, m->width); W32(vih + 56, m->height); W16(vih + 60, 1); W16(vih + 62, 24);
    W32(vih + 64, 0); W32(vih + 68, m->frame_bytes);
}
static void p_connected_to(CPU *c) {
    unsigned off; Movie *m = movie_of(ARG(0), &off); uint32_t out = ARG(1);
    if (!m || !range(out, 4)) { ret(c, 1, E_POINTER); return; }
    if (off == O_VPIN && m->connected && m->in_pin) { gcall(c, m->in_pin, 1, 0, NULL); W32(out, m->in_pin); ret(c, 1, 0); return; }
    W32(out, 0); ret(c, 1, VFW_E_NOT_CONNECTED);
}
static void p_media_type(CPU *c) {
    unsigned off; Movie *m = movie_of(ARG(0), &off); uint32_t out = ARG(1);
    if (!m || !range(out, 72)) { ret(c, 1, E_POINTER); return; }
    if (off != O_VPIN || !m->connected) { memset(HP(out), 0, 72); ret(c, 1, VFW_E_NOT_CONNECTED); return; }
    uint32_t b = base(index_of(m)); memcpy(HP(out), HP(b + O_MEDIATYPE), 72);
    uint32_t format = movies_local_alloc(88); if (!format) { W32(out + 64, 0); W32(out + 68, 0); ret(c, 1, E_OUTOFMEMORY); return; }
    memcpy(HP(format), HP(b + O_VIH), 88); W32(out + 68, format); ret(c, 1, 0);
}
static void p_info(CPU *c) {
    unsigned off; Movie *m = movie_of(ARG(0), &off); uint32_t out = ARG(1);
    if (!m || !range(out, 264)) { ret(c, 1, E_POINTER); return; }
    memset(HP(out), 0, 264); W32(out, base(index_of(m)) + O_FILTER); W32(out + 4, 1);
    const char *name = off == O_VPIN ? "Raw Video 1" : "Raw Audio 0"; for (unsigned i = 0; name[i]; i++) W16(out + 8 + 2 * i, (uint8_t)name[i]);
    ret(c, 1, 0);
}
static void p_direction(CPU *c) { if (range(ARG(1), 4)) W32(ARG(1), 1); ret(c, 1, 0); } /* PINDIR_OUTPUT */
static void p_query_id(CPU *c) { if (range(ARG(1), 4)) W32(ARG(1), 0); ret(c, 1, E_NOTIMPL); }
static void p_accept(CPU *c) { ret(c, 1, same(ARG(1), mediatype_video) && same(ARG(1) + 16, subtype_rgb24) ? 0 : S_FALSE); }
static void p_receive_connection(CPU *c) { ret(c, 2, E_UNEXPECTED); }
static void p_disconnect(CPU *c) { ret(c, 0, S_FALSE); }
/* ---- IMemAllocator ---- */
static void a_set_properties(CPU *c) {
    Movie *m = movie_of(ARG(0), NULL); uint32_t req = ARG(1), actual = ARG(2);
    if (!m || !range(req, 16) || !range(actual, 16)) { ret(c, 2, E_POINTER); return; }
    W32(actual, 1); W32(actual + 4, m->frame_bytes); W32(actual + 8, 1); W32(actual + 12, 0); ret(c, 2, 0);
}
static void a_get_properties(CPU *c) {
    Movie *m = movie_of(ARG(0), NULL); uint32_t out = ARG(1);
    if (!m || !range(out, 16)) { ret(c, 1, E_POINTER); return; }
    W32(out, 1); W32(out + 4, m->frame_bytes); W32(out + 8, 1); W32(out + 12, 0); ret(c, 1, 0);
}
static void a_get_buffer(CPU *c) { if (range(ARG(1), 4)) W32(ARG(1), 0); ret(c, 4, E_NOTIMPL); }
static void a_release_buffer(CPU *c) { ret(c, 1, 0); }
static void ok0(CPU *c) { ret(c, 0, 0); }
/* ---- IMediaSample (one reused sample per movie) ---- */
static void sm_pointer(CPU *c) { Movie *m = movie_of(ARG(0), NULL); if (!m || !range(ARG(1), 4)) { ret(c, 1, E_POINTER); return; } W32(ARG(1), m->sample_buf); ret(c, 1, 0); }
static void sm_size(CPU *c) { Movie *m = movie_of(ARG(0), NULL); ret(c, 0, m ? m->frame_bytes : 0); }
static void sm_time(CPU *c) { ret(c, 2, VFW_E_SAMPLE_TIME_NOT_SET); }
static void sm_media_time(CPU *c) { ret(c, 2, VFW_E_MEDIA_TIME_NOT_SET); }
static void sm_media_type(CPU *c) { if (range(ARG(1), 4)) W32(ARG(1), 0); ret(c, 1, S_FALSE); }
static void sm_ok0(CPU *c) { ret(c, 0, 0); }
static void sm_false0(CPU *c) { ret(c, 0, S_FALSE); }

/* ---- hooks called by media-com for playback graphs ---- */
uint32_t movies_movie_create_reader(uint32_t clsid, uint32_t *out) {
    if (!ready || !same(clsid, clsid_asf_reader)) return 0;
    Movie *m = allocate_movie(); if (!m) { log_once("no free movie block for an ASF reader"); return E_OUTOFMEMORY; }
    *out = base(index_of(m)) + O_FILTER; return 1;
}
int movies_movie_is_reader_clsid(uint32_t clsid) { return ready && same(clsid, clsid_asf_reader); }
uint32_t movies_movie_add_filter(CPU *c, uint32_t graph, uint32_t filter, uint32_t name) {
    if (!ready) return E_NOINTERFACE;
    unsigned off; Movie *reader = movie_of(filter, &off);
    Movie *m = movie_for_graph(graph);
    if (reader) {                                            /* our ASF reader joins the graph */
        if (off != O_FILTER) return E_NOINTERFACE;
        if (m && m != reader) { reader->filter = m->filter; reader->state = m->state; m->used = 0; }
        reader->graph = graph; return 0;
    }
    if (!m) { m = allocate_movie(); if (!m) { log_once("no free movie block for a renderer"); return E_OUTOFMEMORY; } m->graph = graph; }
    if (m->filter) { log_once("second renderer on one playback graph"); return E_FAIL; }
    gcall(c, filter, 1, 0, NULL);                            /* AddRef: the graph holds the filter */
    uint32_t args[2] = {graph, name};
    uint32_t hr = gcall(c, filter, 13, 2, args);             /* JoinFilterGraph */
    if (hr & 0x80000000u) { gcall(c, filter, 2, 0, NULL); return hr; }
    m->filter = filter; return 0;
}
int movies_movie_owns_graph(uint32_t graph) { return ready && movie_for_graph(graph) != NULL; }
uint32_t movies_movie_query(uint32_t graph, uint32_t iid, uint32_t out) {
    Movie *m = ready ? movie_for_graph(graph) : NULL; if (!m) return E_NOINTERFACE;
    uint32_t p = interface_for(m, O_CONTROL, iid);
    if (!p || same(iid, iid_unknown)) return E_NOINTERFACE;
    W32(out, p); return 0;
}
uint32_t movies_movie_render(CPU *c, uint32_t graph, uint32_t pin) {
    unsigned off; Movie *m = movie_of(pin, &off);
    if (!m || m->graph != graph || (off != O_VPIN && off != O_APIN)) { log_once("Render on a pin that is not ours"); return E_NOTIMPL; }
    if (off == O_APIN) return 0;                             /* movie audio plays on a host voice */
    if (!m->filter || !m->loaded) return E_UNEXPECTED;
    if (m->connected) return 0x80040204u;                    /* VFW_E_ALREADY_CONNECTED */
    uint32_t b = base(index_of(m)), scratch = b + O_SCRATCH;
    const char *in = "In"; for (unsigned i = 0; i <= 2; i++) W16(scratch + 2 * i, i < 2 ? (uint8_t)in[i] : 0);
    uint32_t find[2] = {scratch, scratch + 0x10}; W32(scratch + 0x10, 0);
    uint32_t hr = gcall(c, m->filter, 11, 2, find);           /* FindPin(L"In") */
    if ((hr & 0x80000000u) || !R32(scratch + 0x10)) return hr ? hr : E_FAIL;
    m->in_pin = R32(scratch + 0x10);
    fill_media_type(m);
    uint32_t rc[2] = {b + O_VPIN, b + O_MEDIATYPE};
    hr = gcall(c, m->in_pin, 4, 2, rc);                       /* ReceiveConnection */
    if (hr & 0x80000000u) { fprintf(stderr, "[movie] renderer refused RGB24 %ux%u: %08x\n", m->width, m->height, hr); gcall(c, m->in_pin, 2, 0, NULL); m->in_pin = 0; return hr; }
    memcpy(HP(scratch + 0x20), iid_mem_input, 16); W32(scratch + 0x30, 0);
    uint32_t qi[2] = {scratch + 0x20, scratch + 0x30};
    hr = gcall(c, m->in_pin, 0, 2, qi);
    if ((hr & 0x80000000u) || !R32(scratch + 0x30)) return hr ? hr : E_NOINTERFACE;
    m->mem_pin = R32(scratch + 0x30);
    uint32_t na[2] = {b + O_ALLOC, 0};
    gcall(c, m->mem_pin, 4, 2, na);                           /* NotifyAllocator(ours, not read-only) */
    m->sample_buf = movies_backend_alloc(m->frame_bytes);
    if (!m->sample_buf) return E_OUTOFMEMORY;
    m->connected = 1; m->next_frame = 0;
    return 0;
}
void movies_movie_graph_release(CPU *c, uint32_t graph) {
    Movie *m = ready ? movie_for_graph(graph) : NULL; if (!m) return;
    trace(m, "Release");
    if (m->filter && m->state != 0) gcall(c, m->filter, 4, 0, NULL);
    if (m->in_pin) { if (m->connected) gcall(c, m->in_pin, 5, 0, NULL); gcall(c, m->in_pin, 2, 0, NULL); }
    if (m->mem_pin) gcall(c, m->mem_pin, 2, 0, NULL);
    if (m->filter) { uint32_t none[2] = {0, 0}; gcall(c, m->filter, 13, 2, none); gcall(c, m->filter, 2, 0, NULL); }
    if (m->video_token) movie_release_js(m->video_token);
    if (m->audio_token) movie_release_js(m->audio_token);
    if (m->audio_slot >= 0) movies_dsound_stream_close(m->audio_slot);
    if (m->sample_buf) movies_backend_free(m->sample_buf);
    memset(HP(base(index_of(m))), 0, BLOCK); memset(m, 0, sizeof *m);
}

int movies_movie_init(void) {

    if ((uint64_t)VT + K_KINDS * VT_SIZE > MOVIES_MOVIE_SPACE || !g_mem) return 0;
    memset(movies, 0, sizeof movies);
    /* Per kind: up to 24 slots; slots 0-2 are IUnknown. */
    GuestFn table[K_KINDS][24] = {
        [K_CONTROL] = {m_query, m_addref, m_release, dispatch_count, dispatch_info, dispatch_names, dispatch_invoke,
                       mc_run, mc_pause, mc_stop, mc_state, notimpl1, notimpl2, notimpl1, notimpl1, notimpl0},
        [K_POSITION] = {m_query, m_addref, m_release, dispatch_count, dispatch_info, dispatch_names, dispatch_invoke,
                        mp_duration, mp_put_position, mp_get_position, mp_stop_time, ok2, mp_zero, ok2, ok2, mp_rate, mp_can_seek, mp_can_seek},
        [K_SEEKING] = {m_query, m_addref, m_release, ms_caps, ok1, ms_is_format, ms_format, ms_format, ms_is_format, ok1,
                       ms_duration, ms_duration, ms_current, notimpl5, ms_set_positions, ms_get_positions, ms_available, ok2, ms_get_rate, ms_preroll},
        [K_AUDIO] = {m_query, m_addref, m_release, dispatch_count, dispatch_info, dispatch_names, dispatch_invoke,
                     ba_put_volume, ba_get_volume, ok1, ba_get_balance},
        [K_FILTER] = {m_query, m_addref, m_release, f_class, ok0, ok0, ok2, f_state, ok1, f_get_sync, f_enum_pins, f_find_pin, f_info, ok2, notimpl1},
        [K_SOURCE] = {m_query, m_addref, m_release, s_load, s_cur_file},
        [K_ENUM] = {m_query, m_addref, m_release, e_next, e_skip, e_reset, notimpl1},
        [K_PIN] = {m_query, m_addref, m_release, notimpl2, p_receive_connection, p_disconnect, p_connected_to, p_media_type,
                   p_info, p_direction, p_query_id, p_accept, notimpl1, notimpl2, ok0, ok0, ok0, ok6},
        [K_ALLOC] = {m_query, m_addref, m_release, a_set_properties, a_get_properties, ok0, ok0, a_get_buffer, a_release_buffer},
        [K_SAMPLE] = {m_query, m_addref, m_release, sm_pointer, sm_size, sm_time, ok2, sm_ok0, ok1, sm_false0, ok1, sm_size, ok1,
                      sm_media_type, ok1, sm_false0, ok1, sm_media_time, ok2},
    };
    unsigned slot = 0;
    for (unsigned k = 0; k < K_KINDS; k++)
        for (unsigned i = 0; i < 24; i++) {
            GuestFn fn = table[k][i] ? table[k][i] : (i < 3 ? (i == 0 ? m_query : i == 1 ? m_addref : m_release) : NULL);
            if (!fn) { W32(VT + k * VT_SIZE + 4 * i, 0); continue; }
            register_function(TRAMP + 16 * slot, fn); W32(VT + k * VT_SIZE + 4 * i, TRAMP + 16 * slot); slot++;
        }
    (void)ok3; (void)false1; (void)notimpl4; (void)notimpl3;
    ready = 1; return 1;
}
