// Ultima VII on the m9b SoC: the display.
//
// m9b_video_show() (Image_window::show, port/m9b_imagewin.cc) copies the
// game's 8-bit surface into one of three display buffers, cleans it from
// the data cache and has the vertical-blank interrupt show it from the next
// frame, with the palette (Exult's SDL colors, as RGB565) loaded at the same
// time. 320-wide surfaces are shown with columns doubled; the lines are
// stretched to all 480 (320 x 200: 2 or 3 each, as DOS's 320 x 200 filled
// a 4:3 screen).
//
// m9b_screenshot() (0x1C on the UART, see m9b_sdl.cc) dumps the screen:
// "\nM9BSHOT\n", the width and height (16-bit), 256 RGB565 palette entries
// (little-endian), then the pixels of the buffer on screen
// (../../core/tools/m9bctl.py shot).
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <SDL.h>

#include "m9b_hw.h"
#include "m9b_map.h"

#define VRAM          ((volatile uint32_t*)M9B_VRAM)
#define VRAM_PALETTE  (VRAM + 512)   // 256 x RGB565, two per word
#define VRAM_TABLE    (VRAM + 640)   // one entry per line
#define VIDEO_CTRL    (*(volatile uint32_t*)M9B_CTRL)
#define VIDEO_HWIN    (*(volatile uint32_t*)(M9B_CTRL + 0x18u))
#define UART_DATA     (*(volatile uint32_t*)M9B_UART)
#define UART_STATUS   (*(volatile uint32_t*)(M9B_UART + 0x4u))
#define LINE_BLANK    (0u << 22)
#define LINE_8BPP     (2u << 22)
#define LINE_2X       (1u << 28)

#define NBUF 3
static uint8_t *disp[NBUF];
static int scr_w, scr_h;
static volatile int shown = -1, pending = -1;
static uint32_t pal_words[128], pal_next[128];
static volatile uint32_t pal_dirty;
static volatile uint32_t vblanks;
static uint16_t row_of[480];

uint32_t m9b_frames, m9b_copy_cycles;   // for the profile report

static uint32_t line_8bpp(uintptr_t addr) {
    uint32_t off = (uint32_t)addr - PSRAM_BASE;
    return LINE_8BPP | ((off & 255u) / 2u) << 15 | (off >> 8);
}

#ifdef M9B_PROFILE
extern "C" uint32_t m9b_irq_pc, m9b_irq_ra;   // ../../core/runtime/trap.c
static uint32_t frames_seen, stalled;
#endif

// From the trap handler (../../core/runtime/trap.c) on each vertical blank.
extern "C" void vblank_flip(void) {
    vblanks++;
#ifdef M9B_PROFILE
    // Nothing shown for 5 s: where the program is (for addr2line).
    if (m9b_frames != frames_seen) {
        frames_seen = m9b_frames;
        stalled = 0;
    } else if (++stalled % 300u == 0) {
        printf("stall %lu s: pc %08lx ra %08lx\n", (unsigned long)(stalled / 60u), (unsigned long)m9b_irq_pc,
               (unsigned long)m9b_irq_ra);
    }
#endif
    int p = pending;
    if (p < 0) return;
    if (pal_dirty) {
        for (uint32_t i = 0; i < 128u; i++) VRAM_PALETTE[i] = pal_words[i];
        pal_dirty = 0;
    }
    const uint8_t *buf = disp[p];
    uint32_t x2 = scr_w <= 360 ? LINE_2X : 0u;
    for (uint32_t y = 0; y < 480u; y++) VRAM_TABLE[y] = line_8bpp((uintptr_t)&buf[row_of[y] * scr_w]) | x2;
    shown = p;
    pending = -1;
}

extern "C" uint32_t m9b_vblanks(void) { return vblanks; }

// The display buffers and the line table's rows for a w x h screen.
static bool set_display(int w, int h) {
    uint32_t m = irq_off();
    pending = shown = -1;
    for (uint32_t y = 0; y < 480u; y++) VRAM_TABLE[y] = LINE_BLANK;
    irq_restore(m);
    for (int i = 0; i < NBUF; i++) {
        free(disp[i]);
        disp[i] = static_cast<uint8_t *>(aligned_alloc(256, ((size_t)w * h + 255u) & ~(size_t)255u));
        if (!disp[i]) return false;
    }
    scr_w = w;
    scr_h = h;
    for (uint32_t y = 0; y < 480u; y++) row_of[y] = (uint16_t)(y * (uint32_t)h / 480u);
    uint32_t cols = w <= 360 ? 2u * (uint32_t)w : (uint32_t)w;
    if (cols > 720u) cols = 720u;
    uint32_t left = (720u - cols) / 2u;
    VIDEO_HWIN = (left + cols) << 16 | left;
    VIDEO_CTRL = 1;
    return true;
}

#ifdef M9B_PROFILE
#	include <cstdio>
#	include <unistd.h>

// Every 5 s: frames shown per second (Exult shows a frame when something
// changed), time in the sound interrupt, the frame copy and file reads,
// misaligned accesses emulated (../../core/runtime/trap.c), the heap's top;
// every 15 s the PC samples (../../core/tools/prof.py).
extern uint32_t sound_cycles;   // m9b_sdl.cc
extern "C" uint32_t misaligned_traps, m9b_read_cycles, m9b_read_bytes;   // ../../core/runtime/
extern "C" void prof_report(void);
extern "C" char __heap_start;   // (picolibc.ld)

static void profile_frame() {
    static uint32_t frames0, t0, snd0, mis0, cp0, rd0, rb0, reports;
    uint32_t now = CLOCK_CNT, dt = now - t0;
    if (!t0) {
        t0 = now;
        frames0 = m9b_frames;
        return;
    }
    if (dt < 5u * 36000000u) return;
    uint32_t fr = m9b_frames - frames0;
    printf("%lu.%lu fps, sound %lu%%, frame copy %lu%%, reads %lu%% (%lu KB), %lu misaligned, heap top %lu KB\n",
           (unsigned long)(fr * 360000000ull / dt / 10), (unsigned long)(fr * 360000000ull / dt % 10),
           (unsigned long)((uint64_t)(sound_cycles - snd0) * 100 / dt),
           (unsigned long)((uint64_t)(m9b_copy_cycles - cp0) * 100 / dt),
           (unsigned long)((uint64_t)(m9b_read_cycles - rd0) * 100 / dt), (unsigned long)((m9b_read_bytes - rb0) / 1024),
           (unsigned long)(misaligned_traps - mis0),
           (unsigned long)((static_cast<char *>(sbrk(0)) - &__heap_start) / 1024));
    t0 = now;
    frames0 = m9b_frames;
    snd0 = sound_cycles;
    mis0 = misaligned_traps;
    cp0 = m9b_copy_cycles;
    rd0 = m9b_read_cycles;
    rb0 = m9b_read_bytes;
    if (++reports % 3u == 0) prof_report();
}
#endif

extern "C" void m9b_video_show(const unsigned char *pixels, int pitch, int w, int h, const SDL_Color *palette) {
    if ((w != scr_w || h != scr_h) && !set_display(w, h)) return;
    for (int i = 0; i < 128; i++) {
        const SDL_Color &a = palette[2 * i], &b = palette[2 * i + 1];
        pal_next[i] = (uint32_t)((a.r >> 3) << 11 | (a.g >> 2) << 5 | (a.b >> 3)) |
                      (uint32_t)((b.r >> 3) << 11 | (b.g >> 2) << 5 | (b.b >> 3)) << 16;
    }
    // A buffer neither on screen nor waiting (of three, there always is one).
    int b = 0;
    while (b == shown || b == pending) b++;
    uint32_t c0 = CLOCK_CNT;
    uint8_t *dst = disp[b];
    if (pitch == w) {
        memcpy(dst, pixels, (size_t)w * h);
    } else {
        for (int y = 0; y < h; y++) memcpy(dst + y * w, pixels + y * pitch, (size_t)w);
    }
    cache_clean(dst, (uint32_t)(w * h));
    m9b_copy_cycles += CLOCK_CNT - c0;
    uint32_t m = irq_off();
    memcpy(pal_words, pal_next, sizeof(pal_words));
    pal_dirty = 1;
    pending = b;
    irq_restore(m);
    m9b_frames++;
#ifdef M9B_PROFILE
    profile_frame();
#endif
}


// The buffer on screen (the host test harness's screenshots).
extern "C" const uint8_t *m9b_video_shown(int *w, int *h) {
    *w = scr_w;
    *h = scr_h;
    return shown >= 0 ? disp[shown] : nullptr;
}

// -------------------------------------------------------------------------
// Screenshots over the UART
// -------------------------------------------------------------------------
static void uart_raw(uint8_t c) {
    while (UART_STATUS & 1u) {
    }
    UART_DATA = c;
}

extern "C" void m9b_screenshot(void) {
    static const char tag[] = "\nM9BSHOT\n";
    for (const char *s = tag; *s; s++) uart_raw((uint8_t)*s);
    uart_raw((uint8_t)scr_w); uart_raw((uint8_t)(scr_w >> 8));
    uart_raw((uint8_t)scr_h); uart_raw((uint8_t)(scr_h >> 8));
    for (uint32_t i = 0; i < 128u; i++) {
        uint32_t w = pal_words[i];
        uart_raw((uint8_t)w); uart_raw((uint8_t)(w >> 8));
        uart_raw((uint8_t)(w >> 16)); uart_raw((uint8_t)(w >> 24));
    }
    const uint8_t *px = shown >= 0 ? disp[shown] : nullptr;
    for (int i = 0; px && i < scr_w * scr_h; i++) uart_raw(px[i]);
}
