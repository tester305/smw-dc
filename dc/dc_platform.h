/*
 * Thin KallistiOS layer for the SNES Mario Dreamcast port.
 * Deliberately free of any game headers so KOS and the game's own macros
 * (WORD, BYTE, uint8, ...) never meet in one translation unit.
 */
#ifndef DC_PLATFORM_H
#define DC_PLATFORM_H

#include <stdint.h>
#include <stddef.h>

/* SNES joypad bit layout, as used by the game's input word. */
enum {
  DC_PAD_B = 1 << 0, DC_PAD_Y = 1 << 1, DC_PAD_SELECT = 1 << 2, DC_PAD_START = 1 << 3,
  DC_PAD_UP = 1 << 4, DC_PAD_DOWN = 1 << 5, DC_PAD_LEFT = 1 << 6, DC_PAD_RIGHT = 1 << 7,
  DC_PAD_A = 1 << 8, DC_PAD_X = 1 << 9, DC_PAD_L = 1 << 10, DC_PAD_R = 1 << 11,
};

typedef void (*dc_audio_fill_fn)(int16_t *dst_interleaved_stereo, int frames);

int  dc_init(void);                                  /* video mode + PVR + texture; 0 = ok */
void dc_present(const uint16_t *rgb565_256x256);     /* upload a 256x256 RGB565 image and show it */
void dc_wait_vbl(void);
uint32_t dc_read_pad(int port, int *quit, int *present); /* SNES-layout bits; *quit set on A+B+X+Y+Start */
int  dc_audio_start(dc_audio_fill_fn fn, int rate);  /* stereo, 16-bit; 0 = ok */
void dc_audio_poll(void);
void dc_audio_stop(void);
uint64_t dc_time_us(void);
uint8_t *dc_load_file(const char *path, size_t *size);
void dc_fatal(const char *msg);                      /* shows msg for a few seconds, then exits */
void dc_shutdown(void);

#endif
