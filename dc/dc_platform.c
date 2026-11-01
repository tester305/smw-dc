/* KallistiOS platform layer: video (PVR texture quad), audio (AICA stream), input (maple). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <kos/init.h>
#include <arch/timer.h>
#include <dc/video.h>
#include <dc/pvr/pvr_regs.h>
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

static int g_vmu_fs_ready;

static snd_stream_hnd_t g_stream = -1;
static dc_audio_fill_fn g_fill;
static int16_t g_abuf[AUDIO_BUF_BYTES / 2] __attribute__((aligned(32)));

/* Progressive 320x240 (the hardware pixel/line doubles it to 640x480 on
   output) is the standard KallistiOS homebrew mode and the only display path
   that every Dreamcast emulator renders reliably. The interlaced 640x480
   direct-framebuffer modes previously used here are the least supported path
   and black-screen in several emulators. */
static int dc_display_mode(void) {
  return vid_check_cable() == CT_VGA ? DM_320x240_VGA : DM_320x240_NTSC;
}

int dc_init(void) {
  int mode = dc_display_mode();

  vid_set_mode(mode, PM_RGB565);
  vid_border_color(0, 0, 0);
  vid_clear(0, 0, 0);
  dc_menu_init();
  for (int i = 0; i < 8; ++i) dc_wait_vbl();

  if (!vid_mode || !vram_s || vid_mode->width < 256 || vid_mode->height < 224)
    return -1;

  /* KallistiOS already mounts the VMU filesystem during kernel init
     (vmu_fs_init is a default-on weak init flag). Calling fs_vmu_init() here
     a second time re-inserts fs_vmu's static nmmgr_handler_t into the kernel
     name-handler list, so the list node points at itself and nmmgr_lookup()
     spins forever on the first fopen("/vmu/...") — dc_cheats_init() does that
     before the menu is drawn, which is the black screen. smb-dc never calls
     fs_vmu_init() itself, which is why its menu works. */
  g_vmu_fs_ready = 1;
  return 0;
}

/* Software-scale the 256x224 RGB565 image directly into the video framebuffer.
   This deliberately avoids the PVR path so the front-end is visible on boot. */
void dc_present(const uint16_t *img) {
  if (!img || !vid_mode || !vram_s) return;

  const int screen_w = (int)vid_mode->width;
  const int screen_h = (int)vid_mode->height;
  int scale = screen_w / 256;
  if (scale > screen_h / 224) scale = screen_h / 224;
  if (scale < 1) scale = 1;
  const int draw_w = 256 * scale;
  const int draw_h = 224 * scale;
  const int left = (screen_w - draw_w) / 2;
  const int top = (screen_h - draw_h) / 2;
  uint16_t *fb = (uint16_t *)vram_s;

  for (int y = 0; y < draw_h; ++y) {
    const uint16_t *src = img + (y / scale) * 256;
    uint16_t *dst = fb + (top + y) * screen_w + left;
    for (int x = 0; x < draw_w; ++x)
      dst[x] = src[x / scale];
  }
}

/* BIOS-framebuffer menu helpers (same call sequence as the proven SMB Dreamcast front end).
 *
 * The BIOS ROM font is a fixed 12x24 cell, which is only 26 columns on the 320x240 display.
 * Each glyph is therefore drawn by bfont_draw() into a scratch cell and area-averaged down to
 * MENU_GLYPH_W x MENU_GLYPH_H (8x16 = 40 columns x 15 rows) while it is copied to the screen.
 * Change the two sizes below to resize the menu text. */
#define MENU_GLYPH_W 8
#define MENU_GLYPH_H 16
#define MENU_SUBSAMPLES 4   /* MENU_SUBSAMPLES^2 samples per output pixel */

static int dc_menu_pitch(void) {
  return (vid_mode && vid_mode->width) ? (int)vid_mode->width : 320;
}
void dc_menu_init(void) {
  bfont_set_encoding(BFONT_CODE_ISO8859_1);
  bfont_set_foreground_color(0xFFFF);
  bfont_set_background_color(0x0000);
}
void dc_menu_clear(void) { vid_clear(0, 0, 0); }

static void dc_menu_glyph(uint16_t *dst, int pitch, unsigned char ch) {
  static uint16_t cell[BFONT_THIN_WIDTH * BFONT_HEIGHT];
  memset(cell, 0, sizeof(cell));
  bfont_draw(cell, BFONT_THIN_WIDTH, true, ch);          /* 12x24, lit pixel = foreground */
  for (int ty = 0; ty < MENU_GLYPH_H; ++ty) {
    for (int tx = 0; tx < MENU_GLYPH_W; ++tx) {
      int lit = 0;
      for (int j = 0; j < MENU_SUBSAMPLES; ++j) {
        int sy = ((ty * MENU_SUBSAMPLES + j) * BFONT_HEIGHT) / (MENU_GLYPH_H * MENU_SUBSAMPLES);
        for (int i = 0; i < MENU_SUBSAMPLES; ++i) {
          int sx = ((tx * MENU_SUBSAMPLES + i) * BFONT_THIN_WIDTH) / (MENU_GLYPH_W * MENU_SUBSAMPLES);
          if (cell[sy * BFONT_THIN_WIDTH + sx]) ++lit;
        }
      }
      unsigned v = (unsigned)lit * 255u / (MENU_SUBSAMPLES * MENU_SUBSAMPLES);
      dst[ty * pitch + tx] = (uint16_t)(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
    }
  }
}

void dc_menu_draw_str(int y, const char *str, int highlight) {
  const int pitch = dc_menu_pitch();
  const int height = (vid_mode && vid_mode->height) ? (int)vid_mode->height : 240;
  int len = (int)strlen(str);
  const int max_len = (pitch - 2 * MENU_GLYPH_W) / MENU_GLYPH_W;   /* keep an 8 px margin */
  if (len > max_len) len = max_len;
  if (y < 0 || y + MENU_GLYPH_H > height) return;

  int x = (pitch - len * MENU_GLYPH_W) / 2;
  if (x < MENU_GLYPH_W) x = MENU_GLYPH_W;
  uint16_t *row = (uint16_t *)vram_s + y * pitch;
  if (highlight && x >= 2 * MENU_GLYPH_W)
    dc_menu_glyph(row + x - 2 * MENU_GLYPH_W, pitch, '>');
  for (int i = 0; i < len; ++i)
    dc_menu_glyph(row + x + i * MENU_GLYPH_W, pitch, (unsigned char)str[i]);
}
void dc_menu_present(void) {
  /* Single framebuffer: the text is already on screen, just pace to the beam. */
  dc_wait_vbl();
}

/* KOS's vid_waitvbl() busy-polls the SPG_STATUS register and spins FOREVER on
   emulators that never update it: dc_init() would hang before drawing a
   single pixel, leaving a permanent black screen with no feedback. Poll the
   register ourselves with a hard deadline and fall back to frame pacing when
   the register is dead. On real hardware and faithful emulators this behaves
   exactly like vid_waitvbl(). */
void dc_wait_vbl(void) {
  uint64_t t0 = timer_us_gettime64();
  uint32_t line = PVR_GET(PVR_SYNC_STATUS) & 0x1ff;

  if (line == 0) {
    /* Beam is at the top of the frame; wait for it to leave (bounded). */
    while ((PVR_GET(PVR_SYNC_STATUS) & 0x1ff) == 0)
      if (timer_us_gettime64() - t0 > 25000) return;
  }
  /* Wait for the scanline counter to wrap back to 0 = vblank start. */
  while ((PVR_GET(PVR_SYNC_STATUS) & 0x1ff) != 0)
    if (timer_us_gettime64() - t0 > 25000) return;
}

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
  /* Do NOT call fs_vmu_shutdown() here: KOS shuts the VMU filesystem down
     itself after main() returns (arch_auto_shutdown). Shutting it down twice
     would mirror the double-init problem. */
  g_vmu_fs_ready = 0;
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
    for (int i = 0; i < 60 * 8; i++) dc_wait_vbl();
  }
  exit(1);
}
