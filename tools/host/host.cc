// A host (x86, 32-bit Linux) test harness for the port: runs the same Exult
// and port code (port/*.cc, not m9b_posix.c) without the board.
//
// The SoC's I/O window (0x400000..0x7FFFFF) is mapped as plain memory
// at its own address, so the port's register accesses land in it: a thread
// keeps CLOCK_CNT counting at 36 MHz, and m9b_poll_hook() (SDL_PumpEvents
// calls it) plays the hardware's other part:
//  - every 16.7 ms a vertical blank (vblank_flip(), as the interrupt would),
//  - every 5.33 ms an audio half (sound_irq()), appended to OUT/audio.wav
//    (48 kHz 16-bit stereo, what the HDMI audio would get),
//  - the script's input, through the USB registers (keyboard on port 0,
//    mouse on port 1, as the hardware reports them),
//  - screenshots of the screen (the display buffer on screen, with the
//    palette the vertical blank loaded) to OUT/NAME.ppm.
// The game's allocations (malloc and friends, wrapped; operator new uses
// malloc, libstdc++ being linked in statically) go to the board's allocator
// over an arena of the board's heap size (host_malloc.c; U7_HEAP_KB in the
// environment changes it), so holes and overhead count as there; new memory
// is filled with 0xA5 (the board's heap holds whatever was in PSRAM before),
// and the heap's top and the most in use are reported.
//
// With U7_HEAPPROF set, the live allocations are also kept by where they
// were made (three return addresses), and the biggest sites printed at the
// end ("heapprof KB COUNT ADDR ADDR ADDR": addr2line -C -f -e exult-host).
//
// Environment: U7_DATA (the card's U7/ contents: exult.cfg, EXULT/, ...),
// U7_OUT (where shots and audio.wav go), U7_SCRIPT, lines:
//   "MS key HID" (press and release, 100 ms), "MS down HID", "MS up HID",
//   "MS text STRING" (typed: letters, digits, space), "MS move DX DY",
//   "MS click [left|right]", "MS shot NAME", "MS quit"
// MS is the time since start in milliseconds, HID a USB HID usage (0x28
// Enter, 0x29 Esc, 0x2C Space, 0x52 Up, ...).
#include <dirent.h>
#include <execinfo.h>
#include <malloc.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include "m9b_map.h"

#define IO_BASE      M9B_IO_BASE
#define IO_SIZE      0x00400000u
#define REG(a)       (*(volatile uint32_t*)(uintptr_t)(a))
#define CLOCK_CNT    REG((M9B_TIMING + 0xCu))
#define VRAM_PALETTE ((volatile uint32_t*)(uintptr_t)(M9B_VRAM + 0x800u))
#define AUDIO_CTRL   REG((M9B_CTRL + 0xCu))
#define AUDIO_PTR(h) REG((M9B_CTRL + 0x10u) + 4u * (h))
#define USB(p, o)    REG(M9B_USB + 0x20u * (p) + (o))
#define PSRAM_BASE   M9B_PSRAM_BASE

extern "C" void vblank_flip(void);
extern "C" void sound_irq(uint32_t pending);
extern "C" const uint8_t *m9b_video_shown(int *w, int *h);

static uint64_t t_start;
static std::string out_dir = ".";
static FILE *wav;
static uint32_t wav_bytes;

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint32_t now_ms() { return (uint32_t)((now_ns() - t_start) / 1000000u); }

// -------------------------------------------------------------------------
// The heap: the board's allocator over an arena of the board's heap size
// (host_malloc.c), counted, filled with a pattern, profiled
// -------------------------------------------------------------------------
extern "C" {
void *dlmalloc(size_t n);
void *dlcalloc(size_t n, size_t m);
void *dlrealloc(void *p, size_t n);
void *dlmemalign(size_t a, size_t n);
int dlposix_memalign(void **p, size_t a, size_t n);
void dlfree(void *p);
size_t dlmalloc_usable_size(const void *p);
size_t host_heap_top(void);
size_t host_heap_size(void);
}
static size_t heap_used, heap_peak;
static unsigned heap_refused;

// The allocation profile (U7_HEAPPROF): an open-addressed table by address.
#define PROF_N (1u << 20)
struct ProfEntry { void *p; size_t n; void *site[3]; };
static ProfEntry *prof;   // (from mmap: not the wrapped allocator)
static bool prof_busy;

static unsigned prof_hash(void *p) { return (unsigned)(((uintptr_t)p >> 3) * 2654435761u) & (PROF_N - 1); }

static void prof_add(void *p, size_t n) {
    if (!prof || !p || prof_busy) return;
    prof_busy = true;   // (backtrace may allocate)
    unsigned i = prof_hash(p);
    while (prof[i].p) i = (i + 1) & (PROF_N - 1);
    void *bt[6];
    int k = backtrace(bt, 6);
    prof[i].p = p;
    prof[i].n = n;
    for (int j = 0; j < 3; j++) prof[i].site[j] = k > j + 3 ? bt[j + 3] : nullptr;
    prof_busy = false;
}

static void prof_del(void *p) {
    if (!prof || !p) return;
    unsigned i = prof_hash(p);
    while (prof[i].p && prof[i].p != p) i = (i + 1) & (PROF_N - 1);
    if (prof[i].p != p) return;
    prof[i].p = nullptr;
    for (unsigned j = (i + 1) & (PROF_N - 1); prof[j].p; j = (j + 1) & (PROF_N - 1)) {   // re-insert the run
        ProfEntry e = prof[j];
        prof[j].p = nullptr;
        unsigned k = prof_hash(e.p);
        while (prof[k].p) k = (k + 1) & (PROF_N - 1);
        prof[k] = e;
    }
}

static void prof_dump() {
    if (!prof) return;
    static struct { void *site[3]; size_t n; unsigned count; } agg[8192];
    unsigned na = 0;
    for (unsigned i = 0; i < PROF_N; i++) {
        if (!prof[i].p) continue;
        unsigned j = 0;
        while (j < na && memcmp(agg[j].site, prof[i].site, sizeof(agg[j].site))) j++;
        if (j == na) {
            if (na == 8192) continue;
            memcpy(agg[na].site, prof[i].site, sizeof(agg[na].site));
            agg[na].n = 0;
            agg[na].count = 0;
            na++;
        }
        agg[j].n += prof[i].n;
        agg[j].count++;
    }
    for (unsigned a = 0; a < na && a < 40; a++) {   // the 40 biggest, in order
        unsigned best = a;
        for (unsigned b = a + 1; b < na; b++)
            if (agg[b].n > agg[best].n) best = b;
        std::swap(agg[a], agg[best]);
        fprintf(stderr, "heapprof %zu %u %p %p %p\n", agg[a].n / 1024, agg[a].count, agg[a].site[0], agg[a].site[1],
                agg[a].site[2]);
    }
}

static void *heap_got(void *p, size_t n, size_t fill) {
    if (!p) {
        if (heap_refused++ < 20)
            fprintf(stderr, "[host %u ms] malloc of %zu bytes failed (heap top %zu KB, %zu KB in use)\n", now_ms(), n,
                    host_heap_top() / 1024, heap_used / 1024);
        return p;
    }
    size_t got = dlmalloc_usable_size(p);
    prof_add(p, got);
    heap_used += got;
    if (heap_used > heap_peak) heap_peak = heap_used;
    if (fill) memset(p, 0xA5, fill);
    return p;
}

extern "C" void *__wrap_malloc(size_t n) { return heap_got(dlmalloc(n), n, n); }
extern "C" void *__wrap_calloc(size_t n, size_t m) { return heap_got(dlcalloc(n, m), n * m, 0); }
extern "C" void *__wrap_aligned_alloc(size_t a, size_t n) { return heap_got(dlmemalign(a, n), n, n); }
extern "C" void *__wrap_memalign(size_t a, size_t n) { return heap_got(dlmemalign(a, n), n, n); }
extern "C" int __wrap_posix_memalign(void **pp, size_t a, size_t n) {
    int r = dlposix_memalign(pp, a, n);
    heap_got(r ? nullptr : *pp, n, n);
    return r;
}
extern "C" void __wrap_free(void *p) {
    prof_del(p);
    if (p) heap_used -= dlmalloc_usable_size(p);
    dlfree(p);
}
extern "C" void *__wrap_realloc(void *p, size_t n) {
    size_t old = p ? dlmalloc_usable_size(p) : 0;
    void *q = dlrealloc(p, n);
    if (!q) return heap_got(q, n, 0);
    prof_del(p);
    heap_used -= old;
    heap_got(q, n, 0);
    size_t now = dlmalloc_usable_size(q);
    if (now > old) memset(static_cast<char *>(q) + old, 0xA5, now - old);
    return q;
}

// Files open now (the board has 40: ../../core/runtime/syscalls.c's MAX_FILES).
static int open_files() {
    DIR *d = opendir("/proc/self/fd");
    int n = 0;
    while (dirent *e = d ? readdir(d) : nullptr)
        if (e->d_name[0] != '.') n++;
    if (d) closedir(d);
    return n - 4;   // stdin, stdout, stderr and the directory itself
}
static int files_peak;

// Bytes the process has read (files: Exult's reads from the card), from
// /proc/self/io.
static unsigned long long bytes_read() {
    FILE *f = fopen("/proc/self/io", "r");
    unsigned long long n = 0;
    char line[64];
    while (f && fgets(line, sizeof(line), f))
        if (!strncmp(line, "rchar: ", 7)) n = strtoull(line + 7, nullptr, 10);
    if (f) fclose(f);
    return n;
}

static void report_memory() {
    static unsigned long long last_read;
    unsigned long long r = bytes_read();
    fprintf(stderr, "[host %u ms] read %llu KB\n", now_ms(), (r - last_read) / 1024);
    last_read = r;
    fprintf(stderr, "[host %u ms] heap top %zu KB of %zu, in use %zu KB, most %zu KB, %u failed; %d files open, most %d\n",
            now_ms(), host_heap_top() / 1024, host_heap_size() / 1024, heap_used / 1024, heap_peak / 1024, heap_refused,
            open_files(), files_peak);
}

// -------------------------------------------------------------------------
// Script
// -------------------------------------------------------------------------
enum Op { DOWN, UP, MOVE, CLICK, SHOT, QUIT };
struct Step {
    uint32_t ms;
    Op op;
    int a, b;
    std::string name;
};
static std::vector<Step> steps;
static size_t next_step;

static int hid_of_char(char c) {
    if (c >= 'a' && c <= 'z') return 4 + (c - 'a');
    if (c >= 'A' && c <= 'Z') return 4 + (c - 'A');
    if (c >= '1' && c <= '9') return 0x1E + (c - '1');
    if (c == '0') return 0x27;
    if (c == ' ') return 0x2C;
    return 0;
}

static void load_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (char *h = strchr(line, '#')) *h = 0;
        char op[16] = "", a[128] = "", b[64] = "";
        unsigned ms;
        if (sscanf(line, "%u %15s %127s %63s", &ms, op, a, b) < 2) continue;
        auto add = [&](uint32_t t, Op o, int x, int y, const char *n = "") { steps.push_back({t, o, x, y, n}); };
        if (!strcmp(op, "key")) { int k = (int)strtol(a, nullptr, 0); add(ms, DOWN, k, 0); add(ms + 100, UP, k, 0); }
        else if (!strcmp(op, "down")) add(ms, DOWN, (int)strtol(a, nullptr, 0), 0);
        else if (!strcmp(op, "up")) add(ms, UP, (int)strtol(a, nullptr, 0), 0);
        else if (!strcmp(op, "text")) {
            uint32_t t = ms;
            for (char *c = a; *c; c++, t += 120) {
                int k = hid_of_char(*c);
                bool caps = *c >= 'A' && *c <= 'Z';
                if (caps) add(t, DOWN, 0xE1, 0);
                add(t + 10, DOWN, k, 0);
                add(t + 60, UP, k, 0);
                if (caps) add(t + 70, UP, 0xE1, 0);
            }
        } else if (!strcmp(op, "move")) add(ms, MOVE, atoi(a), atoi(b));
        else if (!strcmp(op, "click")) { int r = !strcmp(a, "right") ? 2 : 1; add(ms, CLICK, r, 0); add(ms + 100, CLICK, -r, 0); }
        else if (!strcmp(op, "shot")) add(ms, SHOT, 0, 0, a);
        else if (!strcmp(op, "quit")) add(ms, QUIT, 0, 0);
        else { fprintf(stderr, "script: unknown '%s'\n", op); exit(1); }
    }
    fclose(f);
    for (size_t i = 1; i < steps.size(); i++)   // stable sort by time
        for (size_t j = i; j > 0 && steps[j - 1].ms > steps[j].ms; j--) std::swap(steps[j], steps[j - 1]);
}

// Keyboard: up to 4 keys held, and the modifiers, as the USB registers report them.
static uint8_t held[4], held_mods;

static void report_keys() {
    USB(0, 0x04) = held_mods | (uint32_t)held[0] << 8 | (uint32_t)held[1] << 16 | (uint32_t)held[2] << 24;
    USB(0, 0x08) = held[3];
    USB(0, 0x00) = 1u | (((USB(0, 0x00) >> 8) + 1u) & 0xFFu) << 8;   // keyboard, next report
}

static void screenshot(const std::string &name) {
    int w, h;
    const uint8_t *px = m9b_video_shown(&w, &h);
    if (!px) return;
    std::string path = out_dir + "/" + name + ".ppm";
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint32_t c = VRAM_PALETTE[px[i] / 2u] >> (16u * (px[i] & 1u)) & 0xFFFFu;
        uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)((c >> 5 & 63u) << 2), (uint8_t)((c & 31u) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "[host %u ms] shot %s\n", now_ms(), path.c_str());
}

static void finish() {
    report_memory();
    prof_dump();
    if (wav) {   // the sizes in the header
        uint32_t riff = 36u + wav_bytes;
        fseek(wav, 4, SEEK_SET);
        fwrite(&riff, 4, 1, wav);
        fseek(wav, 40, SEEK_SET);
        fwrite(&wav_bytes, 4, 1, wav);
        fclose(wav);
    }
    fprintf(stderr, "[host %u ms] quit\n", now_ms());
    fflush(stderr);
    _exit(0);
}

static void run_script() {
    uint32_t t = now_ms();
    while (next_step < steps.size() && steps[next_step].ms <= t) {
        const Step &s = steps[next_step++];
        switch (s.op) {
        case DOWN:
        case UP:
            if (s.a >= 0xE0 && s.a <= 0xE7) {
                uint8_t bit = (uint8_t)(1u << (s.a - 0xE0));
                held_mods = s.op == DOWN ? held_mods | bit : held_mods & (uint8_t)~bit;
            } else if (s.op == DOWN) {
                for (auto &k : held) if (!k) { k = (uint8_t)s.a; break; }
            } else {
                for (auto &k : held) if (k == s.a) k = 0;
            }
            report_keys();
            break;
        case MOVE: {
            uint32_t pos = USB(1, 0x14);
            uint16_t x = (uint16_t)(pos + (uint32_t)s.a), y = (uint16_t)((pos >> 16) + (uint32_t)s.b);
            USB(1, 0x14) = (uint32_t)y << 16 | x;
            break;
        }
        case CLICK:
            if (s.a > 0) USB(1, 0x0C) |= s.a == 2 ? 2u : 1u;
            else USB(1, 0x0C) &= ~(s.a == -2 ? 2u : 1u);
            break;
        case SHOT:
            screenshot(s.name);
            break;
        case QUIT:
            finish();
        }
    }
}

// -------------------------------------------------------------------------
// The hardware's events, from the game's loop
// -------------------------------------------------------------------------
// The script and outputs: on the first call (this file's C++ objects are
// constructed after host_start, which runs first to map the I/O window).
static void start_script() {
    if (const char *o = getenv("U7_OUT")) out_dir = o;
    if (const char *s = getenv("U7_SCRIPT")) load_script(s);
    wav = fopen((out_dir + "/audio.wav").c_str(), "wb");
    if (wav) {
        static const uint8_t hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                        16, 0, 0, 0, 1, 0, 2, 0, 0x80, 0xBB, 0, 0, 0, 0xEE, 2, 0,
                                        4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
        fwrite(hdr, 1, 44, wav);
    }
}

extern "C" void m9b_poll_hook(void) {
    static uint32_t vb_last, au_last_us, mem_last, files_last;
    static int half;
    static bool started;
    if (!started) {
        started = true;
        start_script();
    }
    uint32_t t = now_ms();
    if (t - vb_last >= 17u) {
        vb_last = t;
        vblank_flip();
    }
    uint32_t us = (uint32_t)((now_ns() - t_start) / 1000u);
    while (us - au_last_us >= 5333u) {   // 256 frames at 48 kHz
        au_last_us += 5333u;
        if (!AUDIO_CTRL) continue;
        sound_irq(half ? 4u : 2u);
        const void *buf = (const void *)(uintptr_t)(AUDIO_PTR(half) + PSRAM_BASE);
        if (wav) { fwrite(buf, 4, 256, wav); wav_bytes += 1024u; }
        half ^= 1;
    }
    if (t - files_last >= 200u) {
        files_last = t;
        int n = open_files();
        if (n > files_peak) files_peak = n;
    }
    if (t - mem_last >= 10000u) {
        mem_last = t;
        report_memory();
    }
    run_script();
}

static void *clock_thread(void *) {
    for (;;) {
        CLOCK_CNT = (uint32_t)((now_ns() - t_start) * 36u / 1000u);
        usleep(50);
    }
    return nullptr;
}

__attribute__((constructor(101))) static void host_start() {
    void *p = mmap((void *)(uintptr_t)IO_BASE, IO_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)IO_BASE) {
        perror("mapping the I/O window");
        exit(1);
    }
    t_start = now_ns();
    pthread_t t;
    pthread_create(&t, nullptr, clock_thread, nullptr);
    if (getenv("U7_HEAPPROF")) {
        void *m = mmap(nullptr, sizeof(ProfEntry) * PROF_N, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED) prof = static_cast<ProfEntry *>(m);
    }
    const char *data = getenv("U7_DATA");
    if (!data || chdir(data)) {
        fprintf(stderr, "U7_DATA: the card's U7/ contents (exult.cfg, EXULT/, ...)\n");
        exit(1);
    }
    USB(0, 0x00) = 1u;   // a keyboard on port 0
    USB(1, 0x00) = 2u;   // a mouse on port 1
}
