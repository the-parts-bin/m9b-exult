// Ultima VII on the m9b SoC: the part of SDL2 Exult uses, over the SoC
// (SDL2's own headers, port/SDL2/, for the types and constants).
//
//  - Time: SDL_GetTicks from the 36 MHz CLOCK_CNT; SDL_Delay waits, polling.
//  - Events: USB keyboards and mice (../../core/rtl/usb_port.v, polled in
//    SDL_PumpEvents) and the UART's keys, as SDL key, text and mouse events.
//    SDL2's scancodes are the USB HID usages, so keyboard reports map
//    directly; key codes are SDL's US layout. A terminal only sends
//    presses, so a UART key counts as held until 150 ms after its last
//    repeat. The mouse moves over the window in its pixels (320 x 200).
//    SDL_AddTimer's callbacks run from SDL_PumpEvents.
//  - Surfaces: 8-bit (with a palette) and 32-bit, in the heap.
//  - Files: SDL_RWops over stdio (the paths: m9b_posix.c).
//  - Audio: the hardware plays 48 kHz 16-bit stereo from two 1 KB halves
//    and raises an interrupt as it moves on from one; the interrupt (the
//    trap handler -> sound_irq()) has Exult's mixer fill the next half at
//    24 kHz stereo (each frame held for 2). SDL_LockAudioDevice masks the
//    audio interrupts.
//  - Windows, displays, game controllers, touch: one 320 x 200 8-bit
//    "display"; none; none (Image_window: port/m9b_imagewin.cc).
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL.h>

#include "m9b_hw.h"
#include "m9b_map.h"
#include "m9b_music.h"

#define UART_DATA     (*(volatile uint32_t*)M9B_UART)
#define UART_RX_VALID (1u << 8)
#define IRQ_PENDING   (*(volatile uint32_t*)(M9B_CTRL + 0x4u))
#define IRQ_ENABLE    (*(volatile uint32_t*)(M9B_CTRL + 0x8u))
#define AUDIO_CTRL    (*(volatile uint32_t*)(M9B_CTRL + 0xCu))
#define AUDIO_PTR     ((volatile uint32_t*)(M9B_CTRL + 0x10u))   // [0] half A, [1] half B
#define IRQ_AUDIO_A   (1u << 1)
#define IRQ_AUDIO_B   (1u << 2)

#define WIN_W 320
#define WIN_H 200

extern "C" void m9b_screenshot(void);   // m9b_video.cc

// -------------------------------------------------------------------------
// Time
// -------------------------------------------------------------------------
extern "C" uint32_t m9b_ticks_ms(void) {
    static uint32_t last, ms, frac;   // CLOCK_CNT (36 MHz) wraps every 119 s
    uint32_t m = irq_off();           // (also called from the audio interrupt)
    uint32_t now = CLOCK_CNT;
    frac += now - last;
    last = now;
    ms += frac / CLOCK_PER_MS;
    frac %= CLOCK_PER_MS;
    uint32_t r = ms;
    irq_restore(m);
    return r;
}

Uint32 SDL_GetTicks(void) { return m9b_ticks_ms(); }
Uint64 SDL_GetTicks64(void) { return m9b_ticks_ms(); }

void SDL_Delay(Uint32 ms) {
    uint32_t t0 = m9b_ticks_ms();
    do SDL_PumpEvents();
    while (m9b_ticks_ms() - t0 < ms);
}

// -------------------------------------------------------------------------
// The event queue
// -------------------------------------------------------------------------
#define QUEUE 256
static SDL_Event queue[QUEUE];
static unsigned q_head, q_count;
static SDL_EventFilter filter;
static void *filter_data;
static Uint32 next_user_event = SDL_USEREVENT;

static int enqueue(const SDL_Event *e) {
    if (filter && !filter(filter_data, const_cast<SDL_Event *>(e))) return 0;
    if (q_count == QUEUE) {   // full: drop the oldest
        q_head = (q_head + 1) % QUEUE;
        q_count--;
    }
    queue[(q_head + q_count) % QUEUE] = *e;
    q_count++;
    return 1;
}

int SDL_PushEvent(SDL_Event *e) {
    e->common.timestamp = m9b_ticks_ms();
    return enqueue(e);
}

static void remove_at(unsigned i) {   // i: index from the head
    for (; i + 1 < q_count; i++) queue[(q_head + i) % QUEUE] = queue[(q_head + i + 1) % QUEUE];
    q_count--;
}

int SDL_PeepEvents(SDL_Event *events, int numevents, SDL_eventaction action, Uint32 minType, Uint32 maxType) {
    if (action == SDL_ADDEVENT) {
        int n = 0;
        for (; n < numevents; n++)
            if (!enqueue(&events[n])) break;
        return n;
    }
    int n = 0;
    for (unsigned i = 0; i < q_count && n < numevents;) {
        SDL_Event &e = queue[(q_head + i) % QUEUE];
        if (e.type >= minType && e.type <= maxType) {
            if (events) events[n] = e;
            n++;
            if (action == SDL_GETEVENT) {
                remove_at(i);
                continue;
            }
        }
        i++;
    }
    return n;
}

int SDL_PollEvent(SDL_Event *e) {
    SDL_PumpEvents();
    if (!q_count) return 0;
    if (e) {
        *e = queue[q_head];
        q_head = (q_head + 1) % QUEUE;
        q_count--;
    }
    return 1;
}

void SDL_SetEventFilter(SDL_EventFilter f, void *userdata) {
    filter = f;
    filter_data = userdata;
}

void SDL_AddEventWatch(SDL_EventFilter f, void *userdata) { (void)f; (void)userdata; }

Uint32 SDL_RegisterEvents(int numevents) {
    Uint32 first = next_user_event;
    next_user_event += (Uint32)numevents;
    return first;
}

// -------------------------------------------------------------------------
// Keys
// -------------------------------------------------------------------------
static Uint8 key_state[SDL_NUM_SCANCODES];
static Uint16 mods;   // KMOD_*
static bool text_input = true;

// The characters of HID usages 0x04..0x38, unshifted and shifted (US).
static const char plain[] = "\0\0\0\0abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\b\t -=[]\\#;'`,./";
static const char shifted[] = "\0\0\0\0ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\x1b\b\t _+{}|~:\"~<>?";

SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode sc) {
    if (sc >= 4 && sc <= 0x38 && plain[sc]) return (SDL_Keycode)(unsigned char)plain[sc];
    if (sc == SDL_SCANCODE_DELETE) return SDLK_DELETE;
    return SDL_SCANCODE_TO_KEYCODE(sc);
}

SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key) {
    for (int sc = 0; sc < SDL_NUM_SCANCODES; sc++)
        if (SDL_GetKeyFromScancode((SDL_Scancode)sc) == key) return (SDL_Scancode)sc;
    return SDL_SCANCODE_UNKNOWN;
}

const Uint8 *SDL_GetKeyboardState(int *numkeys) {
    if (numkeys) *numkeys = SDL_NUM_SCANCODES;
    return key_state;
}

SDL_Keymod SDL_GetModState(void) { return (SDL_Keymod)mods; }

void SDL_StartTextInput(void) { text_input = true; }
void SDL_StopTextInput(void) { text_input = false; }
SDL_bool SDL_IsTextInputActive(void) { return text_input ? SDL_TRUE : SDL_FALSE; }

// HID modifier bits (0 LCtrl, 1 LShift, 2 LAlt, 3 LGUI, 4 RCtrl, ...) as KMOD_*.
static const Uint16 mod_bits[8] = {KMOD_LCTRL, KMOD_LSHIFT, KMOD_LALT, KMOD_LGUI,
                                   KMOD_RCTRL, KMOD_RSHIFT, KMOD_RALT, KMOD_RGUI};

static void key_event(int sc, bool down) {
    if (sc <= 0 || sc >= SDL_NUM_SCANCODES) return;
    if (sc >= 0xE0 && sc <= 0xE7) {
        if (down) mods |= mod_bits[sc - 0xE0];
        else mods &= (Uint16)~mod_bits[sc - 0xE0];
    }
    key_state[sc] = down;
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = (SDL_Scancode)sc;
    e.key.keysym.sym = SDL_GetKeyFromScancode((SDL_Scancode)sc);
    e.key.keysym.mod = mods;
    e.key.windowID = 1;
    SDL_PushEvent(&e);
    // Text, as SDL sends it after the key: printable keys without Ctrl or Alt.
    if (down && text_input && sc >= 4 && sc <= 0x38 && sc != SDL_SCANCODE_RETURN && sc != SDL_SCANCODE_ESCAPE &&
        sc != SDL_SCANCODE_BACKSPACE && sc != SDL_SCANCODE_TAB && !(mods & (KMOD_CTRL | KMOD_ALT))) {
        bool shift = (mods & KMOD_SHIFT) != 0;
        if ((mods & KMOD_CAPS) && sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) shift = !shift;
        char c = (shift ? shifted : plain)[sc];
        if (!c) return;
        memset(&e, 0, sizeof(e));
        e.type = SDL_TEXTINPUT;
        e.text.windowID = 1;
        e.text.text[0] = c;
        SDL_PushEvent(&e);
    }
}

// -------------------------------------------------------------------------
// Mouse
// -------------------------------------------------------------------------
static int mouse_x = WIN_W / 2, mouse_y = WIN_H / 2;
static Uint32 mouse_buttons;   // SDL_BUTTON_*MASK
static Uint32 last_click_time;
static int last_click_button, click_count;

Uint32 SDL_GetMouseState(int *x, int *y) {
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    return mouse_buttons;
}

void SDL_WarpMouseInWindow(SDL_Window *w, int x, int y) {
    (void)w;
    mouse_x = x;
    mouse_y = y;
}

int SDL_ShowCursor(int toggle) { return toggle == SDL_QUERY ? SDL_DISABLE : toggle; }

static void mouse_motion(int dx, int dy) {
    int x = mouse_x + dx, y = mouse_y + dy;
    x = x < 0 ? 0 : x >= WIN_W ? WIN_W - 1 : x;
    y = y < 0 ? 0 : y >= WIN_H ? WIN_H - 1 : y;
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_MOUSEMOTION;
    e.motion.windowID = 1;
    e.motion.state = mouse_buttons;
    e.motion.x = x;
    e.motion.y = y;
    e.motion.xrel = x - mouse_x;
    e.motion.yrel = y - mouse_y;
    mouse_x = x;
    mouse_y = y;
    SDL_PushEvent(&e);
}

static void mouse_button(int hid_bit, bool down) {   // HID: 0 left, 1 right, 2 middle
    static const Uint8 sdl_button[3] = {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE};
    Uint8 b = sdl_button[hid_bit];
    if (down) mouse_buttons |= SDL_BUTTON(b);
    else mouse_buttons &= ~SDL_BUTTON(b);
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.windowID = 1;
    e.button.button = b;
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    if (down) {   // double clicks: the same button again within 500 ms
        uint32_t now = m9b_ticks_ms();
        click_count = (b == last_click_button && now - last_click_time < 500u) ? click_count + 1 : 1;
        last_click_time = now;
        last_click_button = b;
    }
    e.button.clicks = (Uint8)click_count;
    e.button.x = mouse_x;
    e.button.y = mouse_y;
    SDL_PushEvent(&e);
}

// -------------------------------------------------------------------------
// UART keys
// -------------------------------------------------------------------------
#define HOLD_MS 150u
static struct { int key; uint32_t until; } held[8];
static uint32_t esc_state, esc_time;   // 0, 1 after ESC, 2 after ESC [

static void uart_key(int sc, bool shift) {
    uint32_t now = m9b_ticks_ms();
    for (auto &h : held)
        if (h.key == sc) { h.until = now + HOLD_MS; return; }
    for (auto &h : held)
        if (!h.key) {
            h.key = sc;
            h.until = now + HOLD_MS;
            if (shift) key_event(SDL_SCANCODE_LSHIFT, true);
            key_event(sc, true);
            if (shift) key_event(SDL_SCANCODE_LSHIFT, false);
            return;
        }
}

static int scancode_of_char(unsigned char c, bool *shift) {
    *shift = false;
    for (int k = 4; k <= 0x38; k++) {
        if (k >= 0x28 && k <= 0x2B) continue;   // Enter, Esc, Backspace, Tab: by code
        if (plain[k] && plain[k] == (char)c) return k;
        if (shifted[k] && shifted[k] == (char)c) { *shift = true; return k; }
    }
    return 0;
}

static void poll_uart(void) {
    uint32_t now = m9b_ticks_ms();
    if (esc_state && now - esc_time > 30u) { esc_state = 0; uart_key(SDL_SCANCODE_ESCAPE, false); }
    for (;;) {
        uint32_t d = UART_DATA;
        if (!(d & UART_RX_VALID)) break;
        unsigned char c = (unsigned char)d;
        if (esc_state == 1) {
            esc_state = c == '[' ? 2u : 0u;
            if (!esc_state) uart_key(SDL_SCANCODE_ESCAPE, false);
            continue;
        }
        if (esc_state == 2) {
            esc_state = 0;
            if (c == 'A') uart_key(SDL_SCANCODE_UP, false);
            else if (c == 'B') uart_key(SDL_SCANCODE_DOWN, false);
            else if (c == 'C') uart_key(SDL_SCANCODE_RIGHT, false);
            else if (c == 'D') uart_key(SDL_SCANCODE_LEFT, false);
            continue;
        }
        if (c == 27) { esc_state = 1; esc_time = now; }
        else if (c == 0x1Cu) m9b_screenshot();
        else if (c == '\r' || c == '\n') uart_key(SDL_SCANCODE_RETURN, false);
        else if (c == 8 || c == 127) uart_key(SDL_SCANCODE_BACKSPACE, false);
        else if (c == '\t') uart_key(SDL_SCANCODE_TAB, false);
        else if (c >= 32 && c < 127) {
            bool shift;
            int k = scancode_of_char(c, &shift);
            if (k) uart_key(k, shift);
        }
    }
    for (auto &h : held)
        if (h.key && (int32_t)(now - h.until) >= 0) {
            key_event(h.key, false);
            h.key = 0;
        }
}

// -------------------------------------------------------------------------
// USB keyboards and mice (see ../../doom/port/doomgeneric_m9b.c)
// -------------------------------------------------------------------------
#define USB_REG(p, o)    (*(volatile uint32_t*)(M9B_USB + 0x20u * (p) + (o)))
#define USB_STATUS(p)    USB_REG(p, 0x00u)   // bits 1:0 type, 15:8 report count
#define USB_KEYS(p)      USB_REG(p, 0x04u)   // {k3, k2, k1, modifiers}
#define USB_KEYS2(p)     USB_REG(p, 0x08u)   // k4
#define USB_MOUSE(p)     USB_REG(p, 0x0Cu)   // {dy, dx, buttons}
#define USB_MOUSE_POS(p) USB_REG(p, 0x14u)   // {y, x}, 16-bit running sums

static struct {
    uint32_t type, count;
    uint8_t keys[4], mods, buttons;
    uint32_t pos;
} usb[2];

static bool key_in(uint8_t k, const uint8_t *keys) {
    for (int i = 0; i < 4; i++)
        if (keys[i] == k) return true;
    return false;
}

static void usb_keys_changed(const uint8_t *old_keys, uint8_t old_mods, const uint8_t *keys, uint8_t new_mods) {
    for (int b = 0; b < 8; b++)   // modifier bits = HID usages 0xE0..0xE7
        if (((old_mods ^ new_mods) >> b) & 1u) key_event(0xE0 + b, (new_mods >> b) & 1);
    for (int i = 0; i < 4; i++)
        if (old_keys[i] > 3u && !key_in(old_keys[i], keys)) key_event(old_keys[i], false);
    for (int i = 0; i < 4; i++)
        if (keys[i] > 3u && !key_in(keys[i], old_keys)) key_event(keys[i], true);
}

static void usb_poll(void) {
    static const uint8_t none[4] = {0, 0, 0, 0};
    for (uint32_t p = 0; p < 2u; p++) {
        uint32_t st = USB_STATUS(p), type = st & 3u, count = st >> 8 & 0xFFu;
        if (type != usb[p].type) {   // unplugged or replaced: release everything
            usb_keys_changed(usb[p].keys, usb[p].mods, none, 0);
            for (int b = 0; b < 3; b++)
                if ((usb[p].buttons >> b) & 1u) mouse_button(b, false);
            memset(usb[p].keys, 0, 4);
            usb[p].mods = usb[p].buttons = 0;
            usb[p].pos = USB_MOUSE_POS(p);
            usb[p].type = type;
            usb[p].count = count;
            continue;
        }
        if (type == 2u) {
            uint32_t pos = USB_MOUSE_POS(p);
            int dx = (int16_t)(pos - usb[p].pos), dy = (int16_t)((pos >> 16) - (usb[p].pos >> 16));
            usb[p].pos = pos;
            if (dx || dy) mouse_motion(dx, dy);
            uint8_t b = (uint8_t)(USB_MOUSE(p) & 7u);
            for (int i = 0; i < 3; i++)
                if (((b ^ usb[p].buttons) >> i) & 1u) mouse_button(i, (b >> i) & 1);
            usb[p].buttons = b;
            continue;
        }
        if (type != 1u || count == usb[p].count) continue;
        usb[p].count = count;
        uint32_t k = USB_KEYS(p);
        uint8_t keys[4] = {(uint8_t)(k >> 8), (uint8_t)(k >> 16), (uint8_t)(k >> 24), (uint8_t)USB_KEYS2(p)};
        if (keys[0] == 0x01u) continue;   // ErrorRollOver
        usb_keys_changed(usb[p].keys, usb[p].mods, keys, (uint8_t)k);
        memcpy(usb[p].keys, keys, 4);
        usb[p].mods = (uint8_t)k;
    }
}

// -------------------------------------------------------------------------
// Timers (run from SDL_PumpEvents)
// -------------------------------------------------------------------------
static struct { SDL_TimerID id; Uint32 interval, due; SDL_TimerCallback cb; void *param; } timers[8];
static SDL_TimerID next_timer = 1;

SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback cb, void *param) {
    for (auto &t : timers)
        if (!t.id) {
            t.id = next_timer++;
            t.interval = interval;
            t.due = m9b_ticks_ms() + interval;
            t.cb = cb;
            t.param = param;
            return t.id;
        }
    return 0;
}

SDL_bool SDL_RemoveTimer(SDL_TimerID id) {
    for (auto &t : timers)
        if (t.id == id && id) {
            t.id = 0;
            return SDL_TRUE;
        }
    return SDL_FALSE;
}

static void run_timers(void) {
    uint32_t now = m9b_ticks_ms();
    for (auto &t : timers)
        if (t.id && (int32_t)(now - t.due) >= 0) {
            Uint32 next = t.cb(t.interval, t.param);
            if (next) {
                t.interval = next;
                t.due = now + next;
            } else {
                t.id = 0;
            }
        }
}

// The host test harness's (tools/host/host.cc): the hardware's other side.
extern "C" void m9b_poll_hook(void) __attribute__((weak));

void SDL_PumpEvents(void) {
    if (m9b_poll_hook) m9b_poll_hook();
    poll_uart();
    usb_poll();
    m9b_music_poll();
    run_timers();
}

// -------------------------------------------------------------------------
// Surfaces
// -------------------------------------------------------------------------
SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int w, int h, int depth, int pitch, Uint32 Rmask, Uint32 Gmask,
                                      Uint32 Bmask, Uint32 Amask) {
    auto *s = static_cast<SDL_Surface *>(calloc(1, sizeof(SDL_Surface)));
    auto *f = static_cast<SDL_PixelFormat *>(calloc(1, sizeof(SDL_PixelFormat)));
    if (!s || !f) {
        free(s);
        free(f);
        return nullptr;
    }
    f->BitsPerPixel = (Uint8)depth;
    f->BytesPerPixel = (Uint8)((depth + 7) / 8);
    f->Rmask = Rmask;
    f->Gmask = Gmask;
    f->Bmask = Bmask;
    f->Amask = Amask;
    for (int c = 0; c < 4; c++) {   // shifts and losses from the masks
        Uint32 m = c == 0 ? Rmask : c == 1 ? Gmask : c == 2 ? Bmask : Amask;
        Uint8 shift = 0, bits = 0;
        if (m) {
            while (!(m & 1u)) { m >>= 1; shift++; }
            while (m & 1u) { m >>= 1; bits++; }
        }
        Uint8 loss = (Uint8)(8 - bits);
        if (c == 0) { f->Rshift = shift; f->Rloss = loss; }
        else if (c == 1) { f->Gshift = shift; f->Gloss = loss; }
        else if (c == 2) { f->Bshift = shift; f->Bloss = loss; }
        else { f->Ashift = shift; f->Aloss = loss; }
    }
    if (depth == 8) {
        f->format = SDL_PIXELFORMAT_INDEX8;
        auto *pal = static_cast<SDL_Palette *>(calloc(1, sizeof(SDL_Palette)));
        auto *colors = static_cast<SDL_Color *>(calloc(256, sizeof(SDL_Color)));
        if (!pal || !colors) {
            free(pal);
            free(colors);
            free(f);
            free(s);
            return nullptr;
        }
        for (int i = 0; i < 256; i++) colors[i] = SDL_Color{(Uint8)i, (Uint8)i, (Uint8)i, 255};
        pal->ncolors = 256;
        pal->colors = colors;
        pal->refcount = 1;
        f->palette = pal;
    } else {
        f->format = SDL_PIXELFORMAT_ARGB8888;
    }
    f->refcount = 1;
    s->format = f;
    s->w = w;
    s->h = h;
    s->pitch = pitch;
    s->pixels = pixels;
    s->flags = SDL_PREALLOC;
    s->clip_rect = SDL_Rect{0, 0, w, h};
    s->refcount = 1;
    return s;
}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask,
                                  Uint32 Amask) {
    (void)flags;
    int pitch = (w * ((depth + 7) / 8) + 3) & ~3;
    void *pixels = aligned_alloc(64, ((size_t)pitch * h + 63u) & ~(size_t)63u);
    if (!pixels) return nullptr;
    memset(pixels, 0, (size_t)pitch * h);
    SDL_Surface *s = SDL_CreateRGBSurfaceFrom(pixels, w, h, depth, pitch, Rmask, Gmask, Bmask, Amask);
    if (!s) {
        free(pixels);
        return nullptr;
    }
    s->flags = 0;
    return s;
}

void SDL_FreeSurface(SDL_Surface *s) {
    if (!s || --s->refcount > 0) return;
    if (!(s->flags & SDL_PREALLOC)) free(s->pixels);
    if (s->format) {
        if (s->format->palette) {
            free(s->format->palette->colors);
            free(s->format->palette);
        }
        free(s->format);
    }
    free(s);
}

int SDL_SetPaletteColors(SDL_Palette *p, const SDL_Color *colors, int first, int n) {
    if (!p || first < 0 || first + n > p->ncolors) return -1;
    memcpy(&p->colors[first], colors, sizeof(SDL_Color) * (size_t)n);
    p->version++;
    return 0;
}

Uint32 SDL_MapRGB(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b) {
    if (f->palette) {   // the nearest entry
        int best = 0, bd = 1 << 30;
        for (int i = 0; i < f->palette->ncolors; i++) {
            const SDL_Color &c = f->palette->colors[i];
            int d = (c.r - r) * (c.r - r) + (c.g - g) * (c.g - g) + (c.b - b) * (c.b - b);
            if (d < bd) { bd = d; best = i; }
        }
        return (Uint32)best;
    }
    return (Uint32)(r >> f->Rloss) << f->Rshift | (Uint32)(g >> f->Gloss) << f->Gshift |
           (Uint32)(b >> f->Bloss) << f->Bshift | f->Amask;
}

int SDL_FillRect(SDL_Surface *s, const SDL_Rect *r, Uint32 color) {
    SDL_Rect a = r ? *r : SDL_Rect{0, 0, s->w, s->h};
    if (a.x < 0) { a.w += a.x; a.x = 0; }
    if (a.y < 0) { a.h += a.y; a.y = 0; }
    if (a.x + a.w > s->w) a.w = s->w - a.x;
    if (a.y + a.h > s->h) a.h = s->h - a.y;
    if (a.w <= 0 || a.h <= 0) return 0;
    int bpp = s->format->BytesPerPixel;
    for (int y = a.y; y < a.y + a.h; y++) {
        auto *row = static_cast<Uint8 *>(s->pixels) + y * s->pitch + a.x * bpp;
        if (bpp == 1) memset(row, (int)color, (size_t)a.w);
        else
            for (int x = 0; x < a.w; x++) memcpy(row + x * bpp, &color, (size_t)bpp);
    }
    return 0;
}

int SDL_SetColorKey(SDL_Surface *s, int flag, Uint32 key) {
    (void)s; (void)flag; (void)key;
    return 0;
}

SDL_bool SDL_EnclosePoints(const SDL_Point *points, int count, const SDL_Rect *clip, SDL_Rect *result) {
    bool any = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (int i = 0; i < count; i++) {
        int x = points[i].x, y = points[i].y;
        if (clip && (x < clip->x || y < clip->y || x >= clip->x + clip->w || y >= clip->y + clip->h)) continue;
        if (!any) { x0 = x1 = x; y0 = y1 = y; any = true; }
        x0 = x < x0 ? x : x0; x1 = x > x1 ? x : x1;
        y0 = y < y0 ? y : y0; y1 = y > y1 ? y : y1;
    }
    if (any && result) *result = SDL_Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    return any ? SDL_TRUE : SDL_FALSE;
}

// -------------------------------------------------------------------------
// Files: SDL_RWops over stdio
// -------------------------------------------------------------------------
static Sint64 rw_size(SDL_RWops *rw) {
    FILE *f = rw->hidden.stdio.fp;
    long pos = ftell(f);
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, pos, SEEK_SET);
    return end;
}

static Sint64 rw_seek(SDL_RWops *rw, Sint64 off, int whence) {
    FILE *f = rw->hidden.stdio.fp;
    if (fseek(f, (long)off, whence) != 0) return -1;
    return ftell(f);
}

static size_t rw_read(SDL_RWops *rw, void *p, size_t size, size_t n) { return fread(p, size, n, rw->hidden.stdio.fp); }
static size_t rw_write(SDL_RWops *rw, const void *p, size_t size, size_t n) {
    return fwrite(p, size, n, rw->hidden.stdio.fp);
}

static int rw_close(SDL_RWops *rw) {
    int r = rw->hidden.stdio.autoclose ? fclose(rw->hidden.stdio.fp) : 0;
    free(rw);
    return r;
}

SDL_RWops *SDL_RWFromFile(const char *file, const char *mode) {
    FILE *f = fopen(file, mode);
    if (!f) return nullptr;
    auto *rw = static_cast<SDL_RWops *>(calloc(1, sizeof(SDL_RWops)));
    if (!rw) {
        fclose(f);
        return nullptr;
    }
    rw->size = rw_size;
    rw->seek = rw_seek;
    rw->read = rw_read;
    rw->write = rw_write;
    rw->close = rw_close;
    rw->type = SDL_RWOPS_STDFILE;
    rw->hidden.stdio.autoclose = SDL_TRUE;
    rw->hidden.stdio.fp = f;
    return rw;
}

size_t SDL_RWread(SDL_RWops *rw, void *p, size_t size, size_t n) { return rw->read(rw, p, size, n); }
size_t SDL_RWwrite(SDL_RWops *rw, const void *p, size_t size, size_t n) { return rw->write(rw, p, size, n); }
Sint64 SDL_RWseek(SDL_RWops *rw, Sint64 off, int whence) { return rw->seek(rw, off, whence); }
Sint64 SDL_RWtell(SDL_RWops *rw) { return rw->seek(rw, 0, RW_SEEK_CUR); }
Sint64 SDL_RWsize(SDL_RWops *rw) { return rw->size(rw); }
int SDL_RWclose(SDL_RWops *rw) { return rw->close(rw); }

// -------------------------------------------------------------------------
// Audio
// -------------------------------------------------------------------------
#define FRAMES   256u   // per half (48 kHz)
#define MIX_RATE 24000
#define NMIX     (FRAMES / 2u)

static uint32_t audio_buf[2][FRAMES] __attribute__((aligned(256)));
static uint32_t silence[FRAMES] __attribute__((aligned(256)));
static int16_t mix_buf[NMIX * 2];
static SDL_AudioCallback audio_cb;
static void *audio_data;
static volatile bool audio_paused = true;
static int audio_locks;

uint32_t sound_cycles, sound_mixes;   // time in sound_irq (clock ticks), halves mixed

static void mix_half(uint32_t half) {
    if (!audio_cb || audio_paused) {
        AUDIO_PTR[half] = (uint32_t)(uintptr_t)silence - PSRAM_BASE;
        return;
    }
    audio_cb(audio_data, reinterpret_cast<Uint8 *>(mix_buf), (int)sizeof(mix_buf));
    uint32_t *out = audio_buf[half];
    for (uint32_t i = 0; i < NMIX; i++) {   // plus the music (m9b_music.cc), 24 kHz as the mix
        int m = m9b_music_take();
        int l = mix_buf[2 * i] + m, r = mix_buf[2 * i + 1] + m;
        l = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
        r = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
        uint32_t v = (uint16_t)l | (uint32_t)(uint16_t)r << 16;
        out[2 * i] = v;
        out[2 * i + 1] = v;
    }
    cache_clean(out, sizeof(audio_buf[0]));
    AUDIO_PTR[half] = (uint32_t)(uintptr_t)out - PSRAM_BASE;
    sound_mixes++;
}

extern "C" void sound_irq(uint32_t pending) {   // from the trap handler
    if (!(pending & (IRQ_AUDIO_A | IRQ_AUDIO_B))) return;
    uint32_t t0 = CLOCK_CNT;
    if (pending & IRQ_AUDIO_A) mix_half(0);
    if (pending & IRQ_AUDIO_B) mix_half(1);
    sound_cycles += CLOCK_CNT - t0;
}

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained, int allowed_changes) {
    (void)device; (void)iscapture; (void)allowed_changes;
    if (!desired || !desired->callback) return 0;
    SDL_AudioSpec spec = *desired;
    spec.freq = MIX_RATE;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = NMIX;
    spec.silence = 0;
    spec.size = sizeof(mix_buf);
    if (obtained) *obtained = spec;
    audio_cb = desired->callback;
    audio_data = desired->userdata;
    audio_paused = true;
    memset(silence, 0, sizeof(silence));
    cache_clean(silence, sizeof(silence));
    uint32_t m = irq_off();
    AUDIO_PTR[0] = (uint32_t)(uintptr_t)silence - PSRAM_BASE;
    AUDIO_PTR[1] = (uint32_t)(uintptr_t)silence - PSRAM_BASE;
    IRQ_PENDING = IRQ_AUDIO_A | IRQ_AUDIO_B;
    IRQ_ENABLE |= IRQ_AUDIO_A | IRQ_AUDIO_B;
    AUDIO_CTRL = 1u;
    irq_restore(m);
    return 1;
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on) {
    (void)dev;
    audio_paused = pause_on != 0;
}

void SDL_LockAudioDevice(SDL_AudioDeviceID dev) {
    (void)dev;
    uint32_t m = irq_off();
    if (audio_locks++ == 0) IRQ_ENABLE &= ~(IRQ_AUDIO_A | IRQ_AUDIO_B);
    irq_restore(m);
}

void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev) {
    (void)dev;
    uint32_t m = irq_off();
    if (audio_locks > 0 && --audio_locks == 0) IRQ_ENABLE |= IRQ_AUDIO_A | IRQ_AUDIO_B;
    irq_restore(m);
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) {
    (void)dev;
    uint32_t m = irq_off();
    AUDIO_CTRL = 0u;
    IRQ_ENABLE &= ~(IRQ_AUDIO_A | IRQ_AUDIO_B);
    audio_cb = nullptr;
    irq_restore(m);
}

// -------------------------------------------------------------------------
// Start-up, windows, displays, hints, errors
// -------------------------------------------------------------------------
int SDL_Init(Uint32 flags) { (void)flags; return 0; }
int SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }
void SDL_QuitSubSystem(Uint32 flags) { (void)flags; }
void SDL_Quit(void) {}
void SDL_VideoQuit(void) {}
SDL_bool SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; return SDL_TRUE; }
const char *SDL_GetError(void) { return ""; }
int SDL_OpenURL(const char *url) { (void)url; return -1; }
void *SDL_memset(void *dst, int c, size_t len) { return memset(dst, c, len); }

static int the_window;   // (its address is the SDL_Window*)

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags) {
    (void)title; (void)x; (void)y; (void)w; (void)h; (void)flags;
    return reinterpret_cast<SDL_Window *>(&the_window);
}
void SDL_DestroyWindow(SDL_Window *w) { (void)w; }
int SDL_SetWindowFullscreen(SDL_Window *w, Uint32 flags) { (void)w; (void)flags; return 0; }
void SDL_SetWindowIcon(SDL_Window *w, SDL_Surface *icon) { (void)w; (void)icon; }
void SDL_SetWindowSize(SDL_Window *w, int width, int height) { (void)w; (void)width; (void)height; }
void SDL_SetWindowTitle(SDL_Window *w, const char *title) { (void)w; (void)title; }
void SDL_SetWindowGrab(SDL_Window *w, SDL_bool grabbed) { (void)w; (void)grabbed; }
void SDL_GetWindowSize(SDL_Window *w, int *width, int *height) {
    (void)w;
    if (width) *width = WIN_W;
    if (height) *height = WIN_H;
}

int SDL_GetNumDisplayModes(int display) { (void)display; return 1; }
int SDL_GetDisplayMode(int display, int index, SDL_DisplayMode *mode) {
    (void)display; (void)index;
    *mode = SDL_DisplayMode{SDL_PIXELFORMAT_INDEX8, WIN_W, WIN_H, 60, nullptr};
    return 0;
}
int SDL_GetDesktopDisplayMode(int display, SDL_DisplayMode *mode) { return SDL_GetDisplayMode(display, 0, mode); }

SDL_bool SDL_PixelFormatEnumToMasks(Uint32 format, int *bpp, Uint32 *R, Uint32 *G, Uint32 *B, Uint32 *A) {
    *R = *G = *B = *A = 0;
    if (format == SDL_PIXELFORMAT_INDEX8) {
        *bpp = 8;
        return SDL_TRUE;
    }
    *bpp = 32;
    *R = 0x00FF0000u;
    *G = 0x0000FF00u;
    *B = 0x000000FFu;
    *A = 0xFF000000u;
    return SDL_TRUE;
}

// No game controllers, joysticks or touch.
int SDL_NumJoysticks(void) { return 0; }
SDL_bool SDL_IsGameController(int index) { (void)index; return SDL_FALSE; }
SDL_GameController *SDL_GameControllerOpen(int index) { (void)index; return nullptr; }
void SDL_GameControllerClose(SDL_GameController *c) { (void)c; }
SDL_GameController *SDL_GameControllerFromInstanceID(SDL_JoystickID id) { (void)id; return nullptr; }
Sint16 SDL_GameControllerGetAxis(SDL_GameController *c, SDL_GameControllerAxis a) { (void)c; (void)a; return 0; }
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *c) { (void)c; return nullptr; }
const char *SDL_GameControllerName(SDL_GameController *c) { (void)c; return ""; }
SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int index) { (void)index; return -1; }
int SDL_GetNumTouchFingers(SDL_TouchID id) { (void)id; return 0; }
SDL_Finger *SDL_GetTouchFinger(SDL_TouchID id, int index) { (void)id; (void)index; return nullptr; }
