/* KallistiOS platform layer: video (PVR texture quad), audio (AICA stream), input (maple). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <kos/init.h>
#include <arch/timer.h>
#include <dc/video.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <dc/biosfont.h>
#include <dc/fs_vmu.h>
#include <dc/vmu_pkg.h>

#include "dc_platform.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

#define AUDIO_BUF_BYTES 16384
#define STICK_DEADZONE 40
#define TRIGGER_ON 110

/* 1 = sharp 2x (512x448 centred on 640x480); 0 = stretch to the whole screen. */
#define INTEGER_SCALE 1

static int g_fb_width;
static int g_fb_height;
static int g_vmu_fs_ready;

static snd_stream_hnd_t g_stream = -1;
static dc_audio_fill_fn g_fill;
static int16_t g_abuf[AUDIO_BUF_BYTES / 2] __attribute__((aligned(32)));

/* Emulator-safe video selection.
 * Ask KOS for its generic progressive 320x240 mode. KOS selects 320x240 VGA
 * when the cable detector reports VGA, and the CT_ANY 320x240 NTSC mode
 * otherwise. This avoids relying on interlaced 480i support in emulators.
 */
static int dc_display_mode(void) {
  return DM_320x240;
}

int dc_init(void) {
  /* Set up and validate the framebuffer before using any KOS framebuffer API. */
  vid_set_mode(dc_display_mode(), PM_RGB565);
  if (!vid_mode || !vram_s || vid_mode->width < 320 || vid_mode->height < 240)
    return -1;

  g_fb_width = vid_mode->width;
  g_fb_height = vid_mode->height;
  vid_clear(0, 0, 0);

  /* VMU setup must not prevent video from being brought up. */
  g_vmu_fs_ready = (fs_vmu_init() == 0);
  if (!g_vmu_fs_ready) fprintf(stderr, "VMU filesystem unavailable\\n");
  return 0;
}

/*
 * CPU RGB565 framebuffer path: no PVR scene or texture upload is required.
 * Scale the 256x224 source to the active progressive display mode. This also
 * keeps the menu visible because it uses the same 256-pixel-wide source buffer.
 */
void dc_boot_mark(const char *message) {
  if (!message || !vram_s || !vid_mode) return;
  vid_clear(0, 0, 0);
  bfont_set_encoding(BFONT_CODE_ISO8859_1);
  bfont_set_foreground_color(0xFFFF);
  bfont_set_background_color(0x0000);
  bfont_draw_str(vram_s + 12 * vid_mode->width + 12, vid_mode->width, true, message);
}

void dc_present(const uint16_t *img) {
  if (!img || !vram_s || g_fb_width <= 0 || g_fb_height <= 0)
    return;

  uint16_t *fb = (uint16_t *)vram_s;
  for (int y = 0; y < g_fb_height; ++y) {
    int sy = (y * 224) / g_fb_height;
    const uint16_t *src = img + sy * 256;
    uint16_t *dst = fb + y * g_fb_width;
    for (int x = 0; x < g_fb_width; ++x) {
      int sx = (x * 256) / g_fb_width;
      dst[x] = src[sx];
    }
  }
}

void dc_wait_vbl(void) { vid_waitvbl(); }

uint32_t dc_read_pad(int port, int *quit, int *present) {
  maple_device_t *dev = maple_enum_type(port, MAPLE_FUNC_CONTROLLER);
  cont_state_t *st = dev ? (cont_state_t *)maple_dev_status(dev) : NULL;
  uint32_t pad = 0;
  if (present) *present = (st != NULL);
  const uint32_t combo = CONT_A | CONT_B | CONT_X | CONT_Y | CONT_START;
  if (!st) return 0;
  if (quit && (st->buttons & combo) == combo) *quit = 1;

  /* Dreamcast A/B/X/Y sit where SNES B/A/Y/X sit (bottom/right/left/top). */
  if (st->buttons & CONT_A) pad |= DC_PAD_B;
  if (st->buttons & CONT_B) pad |= DC_PAD_A;
  if (st->buttons & CONT_X) pad |= DC_PAD_Y;
  if (st->buttons & CONT_Y) pad |= DC_PAD_X;
  if (st->buttons & CONT_START) pad |= DC_PAD_START;
  if (st->buttons & CONT_DPAD_UP) pad |= DC_PAD_UP;
  if (st->buttons & CONT_DPAD_DOWN) pad |= DC_PAD_DOWN;
  if (st->buttons & CONT_DPAD_LEFT) pad |= DC_PAD_LEFT;
  if (st->buttons & CONT_DPAD_RIGHT) pad |= DC_PAD_RIGHT;
  if (st->joyx > STICK_DEADZONE) pad |= DC_PAD_RIGHT;
  if (st->joyx < -STICK_DEADZONE) pad |= DC_PAD_LEFT;
  if (st->joyy > STICK_DEADZONE) pad |= DC_PAD_DOWN;
  if (st->joyy < -STICK_DEADZONE) pad |= DC_PAD_UP;

  /* Triggers: L / R alone; both together = Select (the Dreamcast has no Select button). */
  int l = st->ltrig > TRIGGER_ON, r = st->rtrig > TRIGGER_ON;
  if (l && r) pad |= DC_PAD_SELECT;
  else if (l) pad |= DC_PAD_L;
  else if (r) pad |= DC_PAD_R;
  return pad;
}

int dc_perf_toggle_pressed(int port) {
  static uint8_t was_pressed[4] = {0};
  if (port < 0 || port >= 4) return 0;
  maple_device_t *dev = maple_enum_type(port, MAPLE_FUNC_CONTROLLER);
  cont_state_t *st = dev ? (cont_state_t *)maple_dev_status(dev) : NULL;
  if (!st) { was_pressed[port] = 0; return 0; }
  int pressed = (st->buttons & (CONT_START | CONT_Y)) == (CONT_START | CONT_Y);
  int triggered = pressed && !was_pressed[port];
  was_pressed[port] = (uint8_t)pressed;
  return triggered;
}

static void *audio_cb(snd_stream_hnd_t hnd, int req_bytes, int *got_bytes) {
  int frames = req_bytes / 4;
  (void)hnd;
  if (frames > AUDIO_BUF_BYTES / 4) frames = AUDIO_BUF_BYTES / 4;
  if (frames < 0) frames = 0;
  g_fill(g_abuf, frames);
  *got_bytes = frames * 4;
  return g_abuf;
}

int dc_audio_start(dc_audio_fill_fn fn, int rate) {
  g_fill = fn;
  if (snd_stream_init() != 0) return -1;
  g_stream = snd_stream_alloc(audio_cb, AUDIO_BUF_BYTES);
  if (g_stream < 0) { snd_stream_shutdown(); return -2; }
  snd_stream_start(g_stream, rate, 1 /* stereo */);
  return 0;
}

void dc_audio_poll(void) { if (g_stream >= 0) snd_stream_poll(g_stream); }

void dc_audio_stop(void) {
  if (g_stream >= 0) {
    snd_stream_stop(g_stream);
    snd_stream_destroy(g_stream);
    snd_stream_shutdown();
    g_stream = -1;
  }
}

uint64_t dc_time_us(void) { return timer_us_gettime64(); }

uint8_t *dc_load_file(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long end = ftell(f);
  rewind(f);
  if (end <= 0) { fclose(f); return NULL; }
  uint8_t *buf = (uint8_t *)malloc((size_t)end);
  if (!buf) { fclose(f); return NULL; }
  if (fread(buf, 1, (size_t)end, f) != (size_t)end) { free(buf); fclose(f); return NULL; }
  fclose(f);
  *size = (size_t)end;
  return buf;
}

/* SRAM is stored as a BIOS-visible VMU package in controller port 1, slot 1. */
#define DC_VMU_SAVE_PATH "/vmu/a1/SMW.SRM"

int dc_sram_load(uint8_t *dst, size_t len) {
  if (!g_vmu_fs_ready) return 0;
  FILE *f = fopen(DC_VMU_SAVE_PATH, "rb");
  if (!f) return 0; /* No existing save. */
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long file_len = ftell(f);
  if (file_len <= 0 || file_len > 65536 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return -1;
  }
  uint8_t *raw = (uint8_t *)malloc((size_t)file_len);
  if (!raw) { fclose(f); return -1; }
  size_t got = fread(raw, 1, (size_t)file_len, f);
  fclose(f);
  vmu_pkg_t pkg;
  int ok = (got == (size_t)file_len &&
            vmu_pkg_parse(raw, (size_t)file_len, &pkg) == 0 &&
            pkg.data_len == (int)len);
  if (ok) memcpy(dst, pkg.data, len);
  free(raw);
  if (!ok) fprintf(stderr, "VMU save is invalid or has the wrong SRAM size\n");
  return ok ? 1 : -1;
}

int dc_sram_store(const uint8_t *src, size_t len) {
  if (!g_vmu_fs_ready || len > 32768) return -1;
  vmu_pkg_t pkg;
  memset(&pkg, 0, sizeof(pkg));
  snprintf(pkg.desc_short, sizeof(pkg.desc_short), "SUPER MARIO");
  snprintf(pkg.desc_long, sizeof(pkg.desc_long), "Super Mario World Save");
  snprintf(pkg.app_id, sizeof(pkg.app_id), "SMW-DC");
  pkg.data_len = (int)len;
  pkg.data = src;
  uint8_t *raw = NULL;
  int raw_len = 0;
  if (vmu_pkg_build(&pkg, &raw, &raw_len) != 0 || !raw || raw_len <= 0)
    return -1;
  FILE *f = fopen(DC_VMU_SAVE_PATH, "wb");
  if (!f) { free(raw); return -1; }
  size_t wrote = fwrite(raw, 1, (size_t)raw_len, f);
  int close_result = fclose(f);
  free(raw);
  if (wrote != (size_t)raw_len || close_result != 0) {
    fprintf(stderr, "Unable to finish writing VMU save\n");
    return -1;
  }
  return 0;
}


/* Cheats are deliberately stored separately from SMW.SRM. */
#define DC_VMU_CHEATS_PATH "/vmu/a1/SMW.CHT"

int dc_cheats_load(uint8_t *dst, size_t len) {
  if (!g_vmu_fs_ready) return -1;
  FILE *f = fopen(DC_VMU_CHEATS_PATH, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long file_len = ftell(f);
  if (file_len <= 0 || file_len > 65536 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
  uint8_t *raw = (uint8_t *)malloc((size_t)file_len);
  if (!raw) { fclose(f); return -1; }
  size_t got = fread(raw, 1, (size_t)file_len, f);
  fclose(f);
  vmu_pkg_t pkg;
  int ok = got == (size_t)file_len &&
           vmu_pkg_parse(raw, (size_t)file_len, &pkg) == 0 &&
           pkg.data_len == (int)len;
  if (ok) memcpy(dst, pkg.data, len);
  free(raw);
  return ok ? 0 : -1;
}

int dc_cheats_store(const uint8_t *src, size_t len) {
  if (!g_vmu_fs_ready || len > 32768) return -1;
  vmu_pkg_t pkg;
  memset(&pkg, 0, sizeof(pkg));
  snprintf(pkg.desc_short, sizeof(pkg.desc_short), "SMW CHEATS");
  snprintf(pkg.desc_long, sizeof(pkg.desc_long), "Super Mario World Cheats");
  snprintf(pkg.app_id, sizeof(pkg.app_id), "SMW-CHT");
  pkg.data_len = (int)len;
  pkg.data = src;
  uint8_t *raw = NULL;
  int raw_len = 0;
  if (vmu_pkg_build(&pkg, &raw, &raw_len) != 0 || !raw || raw_len <= 0) return -1;
  FILE *f = fopen(DC_VMU_CHEATS_PATH, "wb");
  if (!f) { free(raw); return -1; }
  size_t wrote = fwrite(raw, 1, (size_t)raw_len, f);
  int close_result = fclose(f);
  free(raw);
  return (wrote == (size_t)raw_len && close_result == 0) ? 0 : -1;
}

void dc_shutdown(void) {
  dc_audio_stop();
  if (g_vmu_fs_ready) { fs_vmu_shutdown(); g_vmu_fs_ready = 0; }
}

void dc_fatal(const char *msg) {
  dc_shutdown();
  vid_set_mode(dc_display_mode(), PM_RGB565);
  if (vid_mode && vram_s) {
    vid_clear(0, 0, 0);
    int pitch = (int)vid_mode->width;
    bfont_set_encoding(BFONT_CODE_ISO8859_1);
    bfont_set_foreground_color(0xFFFF);
    bfont_set_background_color(0x0000);
    bfont_draw_str(vram_s + 24 * pitch + 24, pitch, true, msg ? msg : "FATAL ERROR");
    for (int i = 0; i < 60 * 8; i++) vid_waitvbl();
  }
  exit(1);
}
