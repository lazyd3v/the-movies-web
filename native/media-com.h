#ifndef MOVIES_MEDIA_COM_H
#define MOVIES_MEDIA_COM_H
#include "runtime.h"
/* Bounded single-owner STA subset. Init before guest execution; no reinit while
 * apartments/objects live. COM identities live in this reservation; reader
 * scratch/sample buffers use the dedicated backend allocator. */
#define MOVIES_MEDIA_BASE 0x04000000u
#define MOVIES_MEDIA_END 0x04010000u
#define MOVIES_MEDIA_TRAMP 0x0e010000u
typedef struct MoviesMediaProfile {
    uint32_t width,height,fps_numerator,fps_denominator,video_bitrate;
    uint32_t sample_rate,channels,audio_bitrate;
} MoviesMediaProfile;
typedef struct MoviesMediaHost {
    int (*create_writer)(uint32_t *token);
    int (*release_writer)(uint32_t token);
    /* Optional: only acknowledge after actual service configuration. Current
     * service is WMV2/WMAv2 fixed640x480@25,1Mbps/stereo44100Hz128kbps,
     * at most25frames/1second; no WMV3, alternate profiles or sample delivery. */
    int (*configure_writer)(uint32_t token,const MoviesMediaProfile *profile);
} MoviesMediaHost;
/* A token owns a real backend writer session. Both callbacks required, no guest
 * reentry; release must acknowledge actual resource release. No WMV3 encoding
 * capability is implied. Changing providers while writers live is rejected. */
int movies_media_set_host(const MoviesMediaHost *host);
typedef struct MoviesMediaPCM {
    uint32_t sample_rate,channels,bits,block_align,max_sample_bytes;
    uint32_t stream_number,source_stream_count;
    uint64_t duration_100ns;
} MoviesMediaPCM;
typedef struct MoviesMediaReaderHost {
    int (*available)(void);
    uint32_t (*create)(uint32_t source_bytes);
    int (*append)(uint32_t token,uint32_t offset,const uint8_t *bytes,uint32_t length);
    int (*open)(uint32_t token,MoviesMediaPCM *metadata);
    /* read:0=error,1=sample,2=EOS; all times100ns, no wallclock synthesis. */
    int (*read)(uint32_t token,uint8_t *bytes,uint32_t capacity,uint32_t *length,uint64_t *start,uint64_t *duration);
    int (*set_range)(uint32_t token,uint64_t start,uint64_t duration,int to_end);
    int (*release)(uint32_t token);
} MoviesMediaReaderHost;
/* Reader provider is immutable while reader identities live; callbacks must not
 * reenter guest code. OpenStream invokes the original guest IStream methods.
 * Initial reader support: one audio stream only, ASF WMAv2 decoded to PCM16LE
 * stereo44100Hz, <=32MiB input, <=1hour timestamps, <=8192-byte sample buffers.
 * Multi-stream media is rejected rather than silently dropping video. Samples
 * own backend allocations independently of the reader and survive Close.
 * SetRange duration0 means toEOF (the JS service receives null); unsupported
 * compressed/video outputs and other interfaces remain explicit failures. */
int movies_media_set_reader_host(const MoviesMediaReaderHost *host);
int movies_media_init(void);
/* WMCreateProfileManager exposes owned profiles and a strict32767-codeunit XML
 * subset. Unknown XML/profile features fail; no DTD, namespaces or PI. Loaded
 * profiles accept only WMV2/WMAv2, and configure requires exact backend settings.
 * Name/description are UTF16 up to511 codeunits; GetCurrentProfile returns a
 * snapshot. Stream mutation, profile serialization and encoder pins remain
 * unsupported traps, rather than reporting successful media processing. */
ImportHandler *movies_media_handlers(uint32_t *count);
#endif
