// Ultima VII on the m9b SoC: music. The games' MIDI music is rendered on a
// PC (tools/make_music.sh) to U7/MUSIC/*.WAV, 24 kHz 16-bit mono, named as
// Exult names its digital music tracks; Exult's Ogg track functions
// (audio/Midi.cc, M9B) call these instead. The main loop (SDL_PumpEvents,
// several times a frame) keeps a 64 KB ring (1.4 s: the heap is tight) filled
// from the card; the audio interrupt (m9b_sdl.cc) adds a sample to each
// of Exult's 24 kHz mix samples. Volume 100 plays the render as it is
// (normalized to about -18 LUFS, peaks at most -1 dBFS).
#include "m9b_music.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#define MUS_RATE  24000u
#define MUS_RING  (32u * 1024u)    // samples
#define MUS_CHUNK (8u * 1024u)     // samples per read (16 KB)

static int16_t ring[MUS_RING] __attribute__((aligned(64)));
static volatile uint32_t ring_r, ring_w;   // read by the interrupt, written by the main loop
static volatile int playing;
static int fd = -1, loop, eof;
static uint32_t data_start, data_end, played;   // played: samples since the start (for the position)
static int gain = 256;                          // Q8

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

extern "C" int m9b_music_take(void) {
    if (!playing || ring_r == ring_w) return 0;
    int v = (ring[ring_r++ & (MUS_RING - 1u)] * gain) >> 8;
    played++;
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

extern "C" void m9b_music_stop(void) {
    playing = 0;
    if (fd >= 0) close(fd);
    fd = -1;
}

// Opens a WAV file: 0 if it is 24 kHz 16-bit mono PCM, positioned at its samples.
static int open_wav(const char *path) {
    uint8_t h[16];
    uint32_t pos = 12;
    int fmt_ok = 0;
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    if (read(fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;
    for (;;) {   // chunks: "fmt " then "data" (others skipped)
        if (lseek(fd, pos, SEEK_SET) != (off_t)pos || read(fd, h, 8) != 8) return -1;
        uint32_t size = le32(h + 4);
        if (!memcmp(h, "fmt ", 4)) {
            if (read(fd, h, 16) != 16) return -1;
            fmt_ok = h[0] == 1 && h[1] == 0 && h[2] == 1 && le32(h + 4) == MUS_RATE && h[14] == 16;
        } else if (!memcmp(h, "data", 4)) {
            if (!fmt_ok) return -1;
            data_start = pos + 8u;
            data_end = data_start + (size & ~1u);
            return lseek(fd, data_start, SEEK_SET) == (off_t)data_start ? 0 : -1;
        }
        pos += 8u + ((size + 1u) & ~1u);
    }
}

static void fill(int max_chunks) {
    if (fd < 0 || eof) return;
    while (max_chunks-- > 0) {
        uint32_t w = ring_w, space = MUS_RING - (w - ring_r);
        uint32_t at = w & (MUS_RING - 1u), n = MUS_RING - at;
        if (n > MUS_CHUNK) n = MUS_CHUNK;
        if (space < n) return;
        off_t pos = lseek(fd, 0, SEEK_CUR);
        uint32_t left = (data_end - (uint32_t)pos) / 2u;
        if (n > left) n = left;
        if (n) {
            ssize_t got = read(fd, &ring[at], n * 2u);
            if (got <= 0) { eof = 1; return; }
            ring_w = w + (uint32_t)got / 2u;
            continue;
        }
        if (!loop || lseek(fd, data_start, SEEK_SET) < 0) { eof = 1; return; }
        max_chunks++;   // the seek read nothing
    }
}

extern "C" int m9b_music_play(const char *name, int repeat) {
    char path[48], base[24];
    unsigned i;
    m9b_music_stop();
    for (i = 0; name[i] && name[i] != '.' && i < sizeof(base) - 1; i++) base[i] = (char)toupper((unsigned char)name[i]);
    base[i] = 0;
    snprintf(path, sizeof(path), "MUSIC/%s.WAV", base);   // relative: U7/MUSIC/ (open() adds U7/)
    if (open_wav(path) < 0) {
        m9b_music_stop();
        return 0;
    }
    loop = repeat;
    eof = 0;
    ring_r = ring_w = 0;
    played = 0;
    fill(2);
    playing = 1;
    return 1;
}

extern "C" int m9b_music_playing(void) { return playing && !(eof && ring_r == ring_w); }
extern "C" void m9b_music_repeat(int repeat) { loop = repeat; }
extern "C" void m9b_music_volume(int vol) { gain = (vol < 0 ? 0 : vol > 100 ? 100 : vol) * 256 / 100; }

extern "C" uint32_t m9b_music_length_ms(void) {
    return m9b_music_playing() ? (uint32_t)((uint64_t)(data_end - data_start) / 2u * 1000u / MUS_RATE) : UINT32_MAX;
}

extern "C" uint32_t m9b_music_position_ms(void) {
    if (!m9b_music_playing()) return UINT32_MAX;
    uint32_t total = (data_end - data_start) / 2u;
    return (uint32_t)((uint64_t)(total ? played % total : 0) * 1000u / MUS_RATE);
}

extern "C" void m9b_music_poll(void) {
    if (playing) fill(2);
}
