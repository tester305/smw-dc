/* KallistiOS platform layer: video (PVR texture quad), audio (AICA stream), input (maple). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <kos/init.h>
#include <arch/timer.h>
#include <dc/video.h>
#include <dc/pvr.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <dc/biosfont.h>

#include "dc_platform.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

#define TXR_W 256
#define TXR_H 256
#define TXR_BYTES (TXR_W * TXR_H * 2)
#define AUDIO_BUF_BYTES 16384
#define STICK_DEADZONE 40
#define TRIGGER_ON 110

/* 1 = sharp 2x (512x448 centred on 640x480); 0 = stretch to the whole screen. */
#define INTEGER_SCALE 1

static pvr_ptr_t g_txr;
static pvr_poly_hdr_t g_hdr;
static pvr_vertex_t g_vert[4];
static int g_pvr_on;

static snd_stream_hnd_t g_stream = -1;
static dc_audio_fill_fn g_fill;
static int16_t g_abuf[AUDIO_BUF_BYTES / 2] __attribute__((aligned(32)));

static void set_vertex(pvr_vertex_t *v, float x, float y, float u, float t, uint32_t flags) {
  v->flags = flags;
  v->x = x; v->y = y; v->z = 1.0f;
  v->u = u; v->v = t;
  v->argb = PVR_PACK_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
  v->oargb = 0;
}

int dc_init(void) {
  /* Reset video state (also covers coming back from the BIOS menu). */
  int cable = vid_check_cable();
  int dm = (cable == CT_VGA) ? DM_640x480_VGA : DM_640x480_NTSC_IL;
  vid_set_mode(dm, PM_RGB565);
  vid_empty();
  vid_set_mode(dm, PM_RGB565);
  vid_border_color(0, 0, 0);
  vid_clear(0, 0, 0);

  if (pvr_init_defaults() != 0)
    return -1;
  g_pvr_on = 1;

  g_txr = pvr_mem_malloc(TXR_BYTES);
  if (!g_txr)
    return -2;

  pvr_poly_cxt_t cxt;
  pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                   TXR_W, TXR_H, g_txr, INTEGER_SCALE ? PVR_FILTER_NEAREST : PVR_FILTER_BILINEAR);
  pvr_poly_compile(&g_hdr, &cxt);

  /* Only the top 224 of the 256 texture rows are used. */
  float x0 = INTEGER_SCALE ? 64.0f : 0.0f, x1 = INTEGER_SCALE ? 576.0f : 640.0f;
  float y0 = INTEGER_SCALE ? 16.0f : 0.0f, y1 = INTEGER_SCALE ? 464.0f : 480.0f;
  float vmax = 224.0f / 256.0f;
  set_vertex(&g_vert[0], x0, y0, 0.0f, 0.0f, PVR_CMD_VERTEX);
  set_vertex(&g_vert[1], x1, y0, 1.0f, 0.0f, PVR_CMD_VERTEX);
  set_vertex(&g_vert[2], x0, y1, 0.0f, vmax, PVR_CMD_VERTEX);
  set_vertex(&g_vert[3], x1, y1, 1.0f, vmax, PVR_CMD_VERTEX_EOL);
  return 0;
}

void dc_present(const uint16_t *img) {
  pvr_wait_render_done();
  pvr_txr_load(img, g_txr, TXR_W * 224 * 2);
  pvr_wait_ready();
  pvr_scene_begin();
  pvr_list_begin(PVR_LIST_OP_POLY);
  pvr_prim(&g_hdr, sizeof(g_hdr));
  pvr_prim(&g_vert[0], sizeof(pvr_vertex_t));
  pvr_prim(&g_vert[1], sizeof(pvr_vertex_t));
  pvr_prim(&g_vert[2], sizeof(pvr_vertex_t));
  pvr_prim(&g_vert[3], sizeof(pvr_vertex_t));
  pvr_list_finish();
  pvr_scene_finish();
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

void dc_shutdown(void) {
  dc_audio_stop();
  if (g_pvr_on) {
    if (g_txr) pvr_mem_free(g_txr);
    pvr_shutdown();
    g_pvr_on = 0;
  }
}

void dc_fatal(const char *msg) {
  dc_shutdown();
  int cable = vid_check_cable();
  vid_set_mode(cable == CT_VGA ? DM_640x480_VGA : DM_640x480_NTSC_IL, PM_RGB565);
  vid_clear(0, 0, 0);
  int pitch = (vid_mode && vid_mode->width) ? (int)vid_mode->width : 640;
  bfont_set_encoding(BFONT_CODE_ISO8859_1);
  bfont_set_foreground_color(0xFFFF);
  bfont_set_background_color(0x0000);
  bfont_draw_str(vram_s + 24 * pitch + 24, pitch, true, msg);
  for (int i = 0; i < 60 * 8; i++) vid_waitvbl();
  exit(1);
}
