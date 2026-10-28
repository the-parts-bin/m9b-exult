// Ultima VII on the m9b SoC: start-up, and what the features left out of
// the build (the cheat screen, the sound tester, touch input, the MIDI
// drivers) leave behind for the rest of Exult.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <malloc.h>
#include <new>
#include <unistd.h>

#include "m9b_hw.h"

#include "LowLevelMidiDriver.h"
#include "cheat_screen.h"
#include "soundtest.h"
#include "touchui.h"

// Exult's main, with the traps (misaligned accesses), the vertical-blank
// interrupt (page flips) and its options: the configuration in U7/ (the
// paths: m9b_posix.c), and no checksums of Exult's data files (minutes of
// reading here, and a game's exult_*.flx may be left out with the game).
// The audio interrupts start with the mixer (SDL_OpenAudioDevice,
// m9b_sdl.cc).
// What ends the program, on the UART: this libstdc++ has no verbose
// terminate handler (an uncaught exception aborts without a word), and the
// heap running out is worth a line before the bad_alloc.
extern "C" char __heap_start __attribute__((weak));   // (picolibc.ld; none on the host)

static unsigned long heap_kb() {
    if (!&__heap_start) return 0;
    return (unsigned long)((static_cast<char *>(sbrk(0)) - &__heap_start) / 1024);
}

[[noreturn]] static void on_terminate() {
    try {
        if (std::current_exception()) throw;
        printf("terminate (heap top %lu KB)\n", heap_kb());
    } catch (const std::exception &e) {
        printf("terminate: uncaught %s (heap top %lu KB)\n", e.what(), heap_kb());
    } catch (...) {
        printf("terminate: uncaught exception (heap top %lu KB)\n", heap_kb());
    }
    abort();
}

static void on_out_of_memory() {
    struct mallinfo mi = mallinfo();   // (the free list: what is lost to holes)
    printf("out of memory (heap top %lu KB, %lu KB in use, %lu KB free in %lu blocks)\n", heap_kb(),
           (unsigned long)mi.uordblks / 1024, (unsigned long)mi.fordblks / 1024, (unsigned long)mi.ordblks);
    std::set_new_handler(nullptr);   // once: new throws bad_alloc now
}

extern "C" int __real_main(int argc, char **argv);
extern "C" int __wrap_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    static char arg0[] = "exult", arg1[] = "-c", arg2[] = "exult.cfg", arg3[] = "--nocrc";
    static char *args[] = {arg0, arg1, arg2, arg3, nullptr};
    hw_start(1u);
    std::set_terminate(on_terminate);
    std::set_new_handler(on_out_of_memory);
    return __real_main(4, args);
}

void CheatScreen::show_screen() {}
void SoundTester::test_sound() {}
uint32 TouchUI::eventType;
bool LowLevelMidiDriver::precacheTimbresOnStartup;
bool LowLevelMidiDriver::precacheTimbresOnPlay;
