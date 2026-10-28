// Ultima VII on the m9b SoC: music streamed from WAV files on the SD card
// (m9b_music.cc), in place of Exult's digital (Ogg) music tracks.
#ifndef M9B_MUSIC_H
#define M9B_MUSIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Plays U7/MUSIC/<NAME>.WAV (name: Exult's track name, e.g. "05bg.ogg";
// the extension is replaced, the case ignored). 0 if it can't.
int m9b_music_play(const char *name, int repeat);
void m9b_music_stop(void);
int m9b_music_playing(void);
void m9b_music_repeat(int repeat);
void m9b_music_volume(int vol);        // 0..100, Exult's digital music volume
uint32_t m9b_music_length_ms(void);    // UINT32_MAX when nothing plays
uint32_t m9b_music_position_ms(void);
// The main loop's part (SDL_PumpEvents): keeps the ring buffer filled.
void m9b_music_poll(void);
// The audio interrupt's part: the next sample (24 kHz), 0 when there's none.
int m9b_music_take(void);

#ifdef __cplusplus
}
#endif

#endif
