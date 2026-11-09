/* Layer II decoding per ISO/IEC 11172-3 clause 2.4.3.3 (bit allocation, scale
 * factor selection, grouped samples, requantization) and Annex A synthesis
 * subband filter (matrixing with N[i][k] = cos((16+i)(2k+1)pi/64), window D). */
#include "mp2-decode.h"
#include <math.h>
#include <string.h>
#include "mp2-tables.inc"

static float window_d[512], matrix_n[64][32], scale[64];
static int tables_ready;
static void init_tables(void) {
    if (tables_ready) return;
    for (int i = 0; i < 257; i++) {
        float v = (float)mp2_enwindow[i] / 65536.0f;
        window_d[i] = v;
        if (i && i < 256) window_d[512 - i] = (i & 63) ? -v : v;
    }
    for (int i = 0; i < 64; i++)
        for (int k = 0; k < 32; k++) matrix_n[i][k] = (float)cos((16 + i) * (2 * k + 1) * M_PI / 64.0);
    for (int i = 0; i < 63; i++) scale[i] = (float)(2.0 * pow(2.0, -i / 3.0));
    scale[63] = 0;
    tables_ready = 1;
}
void mp2_reset(Mp2Decoder *d) { memset(d, 0, sizeof *d); }

static const unsigned bitrate_v1[15] = {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384};
static const unsigned bitrate_v2[15] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
static const unsigned rate_v1[3] = {44100, 48000, 32000};

int mp2_header(const uint8_t *p, size_t size, Mp2Frame *f) {
    if (size < 4 || p[0] != 0xff || (p[1] & 0xe0) != 0xe0) return 0;
    unsigned version = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3, br = p[2] >> 4, sr = (p[2] >> 2) & 3;
    if (version == 1 || layer != 2 || br == 0 || br == 15 || sr == 3) return 0;
    unsigned kbps = version == 3 ? bitrate_v1[br] : bitrate_v2[br];
    unsigned rate = rate_v1[sr] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    f->rate = rate; f->channels = (p[3] >> 6) == 3 ? 1 : 2; f->samples = 1152;
    f->bytes = 144000u * kbps / rate + ((p[2] >> 1) & 1);
    return f->bytes >= 4;
}

typedef struct { const uint8_t *p; size_t bits, at; } Bits;
static unsigned get(Bits *b, unsigned n) {
    unsigned v = 0;
    for (unsigned i = 0; i < n; i++, b->at++) {
        unsigned bit = b->at < b->bits ? (b->p[b->at >> 3] >> (7 - (b->at & 7))) & 1 : 0;
        v = (v << 1) | bit;
    }
    return v;
}
static int select_table(unsigned version, unsigned kbps, unsigned channels, unsigned rate) {
    if (version != 3) return 4;
    unsigned per = kbps / channels;
    if ((rate == 48000 && per >= 56) || (per >= 56 && per <= 80)) return 0;
    if (rate != 48000 && per >= 96) return 1;
    if (rate != 32000 && per <= 48) return 2;
    return 3;
}
static void synthesize(Mp2Decoder *d, unsigned ch, const float s[32], int16_t *out, unsigned stride) {
    float *v = d->v[ch];
    memmove(v + 64, v, 960 * sizeof *v);
    for (int i = 0; i < 64; i++) {
        float sum = 0; const float *n = matrix_n[i];
        for (int k = 0; k < 32; k++) sum += n[k] * s[k];
        v[i] = sum;
    }
    for (int j = 0; j < 32; j++) {
        float sum = 0;
        for (int i = 0; i < 8; i++) {
            sum += v[i * 128 + j] * window_d[i * 64 + j];
            sum += v[i * 128 + 96 + j] * window_d[i * 64 + 32 + j];
        }
        int value = (int)lrintf(sum * 32768.0f);
        out[j * stride] = (int16_t)(value > 32767 ? 32767 : value < -32768 ? -32768 : value);
    }
}

size_t mp2_decode_frame(Mp2Decoder *d, const uint8_t *p, size_t size, int16_t *pcm, Mp2Frame *f) {
    init_tables();
    if (!mp2_header(p, size, f) || f->bytes > size) return 0;
    unsigned version = (p[1] >> 3) & 3, protection = !(p[1] & 1), br = p[2] >> 4, mode = p[3] >> 6, ext = (p[3] >> 4) & 3;
    unsigned kbps = version == 3 ? bitrate_v1[br] : bitrate_v2[br], channels = f->channels;
    int table = select_table(version, kbps, channels, f->rate);
    unsigned sblimit = (unsigned)mp2_sblimit[table], bound = mode == 1 ? (ext + 1) * 4 : sblimit;
    if (bound > sblimit) bound = sblimit;
    Bits b = {p, (size_t)f->bytes * 8, 32 + (protection ? 16 : 0)};
    const uint8_t *alloc_table = mp2_alloc_tables[table];
    unsigned alloc[2][32] = {{0}}, scfsi[2][32] = {{0}}, sf[2][32][3] = {{{0}}};
    const uint8_t *entry[32];
    const uint8_t *t = alloc_table;
    for (unsigned sb = 0; sb < sblimit; sb++) {
        unsigned nbal = t[0]; entry[sb] = t;
        for (unsigned ch = 0; ch < channels; ch++) {
            if (sb < bound || ch == 0) alloc[ch][sb] = get(&b, nbal);
            else alloc[ch][sb] = alloc[0][sb];
        }
        t += 1 + (1u << nbal) - 1;
    }
    for (unsigned sb = 0; sb < sblimit; sb++)
        for (unsigned ch = 0; ch < channels; ch++) if (alloc[ch][sb]) scfsi[ch][sb] = get(&b, 2);
    for (unsigned sb = 0; sb < sblimit; sb++)
        for (unsigned ch = 0; ch < channels; ch++) if (alloc[ch][sb]) {
            unsigned *s = sf[ch][sb];
            switch (scfsi[ch][sb]) {
                case 0: s[0] = get(&b, 6); s[1] = get(&b, 6); s[2] = get(&b, 6); break;
                case 1: s[0] = s[1] = get(&b, 6); s[2] = get(&b, 6); break;
                case 2: s[0] = s[1] = s[2] = get(&b, 6); break;
                default: s[0] = get(&b, 6); s[1] = s[2] = get(&b, 6); break;
            }
        }
    float samples[2][3][32];
    for (unsigned gr = 0; gr < 12; gr++) {
        unsigned part = gr / 4;
        memset(samples, 0, sizeof samples);
        for (unsigned sb = 0; sb < sblimit; sb++) {
            /* Below the joint-stereo bound each channel has its own codes; above it
             * one set of codes is shared and scaled by each channel's factors. */
            unsigned coded = sb < bound ? channels : 1;
            for (unsigned ch = 0; ch < coded; ch++) {
                unsigned a = alloc[ch][sb];
                if (!a) continue;
                unsigned q = entry[sb][a], steps = (unsigned)mp2_quant_steps[q];
                int bits = mp2_quant_bits[q];
                unsigned v[3];
                if (bits < 0) {
                    unsigned code = get(&b, (unsigned)-bits);
                    for (int i = 0; i < 3; i++) { v[i] = code % steps; code /= steps; }
                } else for (int i = 0; i < 3; i++) v[i] = get(&b, (unsigned)bits);
                for (int i = 0; i < 3; i++) {
                    float fraction = (2.0f * (float)v[i] + 1.0f - (float)steps) / (float)steps;
                    if (coded == channels) samples[ch][i][sb] = fraction * scale[sf[ch][sb][part]];
                    else for (unsigned c = 0; c < channels; c++) samples[c][i][sb] = fraction * scale[sf[c][sb][part]];
                }
            }
        }
        for (int i = 0; i < 3; i++)
            for (unsigned ch = 0; ch < channels; ch++)
                synthesize(d, ch, samples[ch][i], pcm + ((gr * 3 + (unsigned)i) * 32) * channels + ch, channels);
    }
    return f->bytes;
}
