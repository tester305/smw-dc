/*
 * SNES Mario for the Sega Dreamcast.
 *
 * Game logic, PPU and DSP come from the snesrev/smw C reimplementation; this
 * file replaces its SDL front end (main.c / config.c / opengl.c) with a
 * KallistiOS one. It only talks to the hardware through dc_platform.h.
 *
 * Runs without the ROM: the game assets are read from /rd/smw_assets.dat,
 * which build.sh extracts from YOUR copy of the game.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "assets/smw_assets.h"
#include "snes/ppu.h"
#include "types.h"
#include "smw_rtl.h"
#include "common_cpu_infra.h"
#include "config.h"
#include "util.h"
#include "smw_spc_player.h"
#include "snes/snes.h"

#include "dc_platform.h"

/* ---- globals the SDL main.c used to define for the rest of the game ---- */
bool g_debug_flag;
bool g_want_dump_memmap_flags;
bool g_new_ppu = true;          /* optimised scanline renderer */
bool g_other_image = true;      /* show the reimplementation's picture, not the emulator's */
struct SpcPlayer *g_spc_player;
int g_got_mismatch_count;
Config g_config;

const uint8 *g_asset_ptrs[kNumberOfAssets];
uint32 g_asset_sizes[kNumberOfAssets];

/* The SNES games other than SMW are not part of this build. */
const RtlGameInfo kSmb1GameInfo = { 0 };
const RtlGameInfo kSmbllGameInfo = { 0 };

void NORETURN Die(const char *error) {
  fprintf(stderr, "Error: %s\n", error);
  dc_fatal(error);
  for (;;) {}
}

/* Audio is produced on the main thread (dc_audio_poll), so no locking is needed. */
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

MemBlk FindInAssetArray(int asset, int idx) {
  return FindIndexInMemblk((MemBlk) { g_asset_ptrs[asset], g_asset_sizes[asset] }, idx);
}

const uint8 *FindPtrInAsset(int asset, uint32 addr) {
  return FindAddrInMemblk((MemBlk) { g_asset_ptrs[asset], g_asset_sizes[asset] }, addr);
}

/* ---- assets ---- */
static void LoadAssets(void) {
  size_t length = 0;
  uint8 *data = dc_load_file("/rd/smw_assets.dat", &length);
  if (!data)
    Die("smw_assets.dat is missing from the romdisk");

  static const char kSig[] = { kAssets_Sig };
  if (length < 16 + 32 + 32 + 8 + kNumberOfAssets * 4 ||
      memcmp(data, kSig, 48) != 0 ||
      *(uint32 *)(data + 80) != kNumberOfAssets)
    Die("smw_assets.dat does not match this build (re-run extraction)");

  uint32 offset = 88 + kNumberOfAssets * 4 + *(uint32 *)(data + 84);
  for (size_t i = 0; i < kNumberOfAssets; i++) {
    uint32 size = *(uint32 *)(data + 88 + i * 4);
    offset = (offset + 3) & ~3u;
    if ((uint64)offset + size > length)
      Die("smw_assets.dat is corrupt");
    g_asset_sizes[i] = size;
    g_asset_ptrs[i] = data + offset;
    offset += size;
  }
}

/* ---- audio: the DSP renders 534 stereo frames per game frame at 32 kHz ---- */
enum { kAudioRate = 32000, kAudioBlock = 534 };
static int16 g_audio_block[kAudioBlock * 2];
static int g_audio_pos = kAudioBlock;

static void AudioFill(int16_t *dst, int frames) {
  while (frames > 0) {
    if (g_audio_pos >= kAudioBlock) {
      RtlRenderAudio(g_audio_block, kAudioBlock, 2);
      g_audio_pos = 0;
    }
    int n = kAudioBlock - g_audio_pos;
    if (n > frames) n = frames;
    memcpy(dst, g_audio_block + g_audio_pos * 2, (size_t)n * 4);
    dst += n * 2;
    g_audio_pos += n;
    frames -= n;
  }
}

/* ---- video ---- */
static uint8 g_pixels[256 * 4 * 240] __attribute__((aligned(32)));     /* emulator PPU (unused) */
static uint8 g_my_pixels[256 * 4 * 240] __attribute__((aligned(32)));  /* game PPU */
static uint16_t g_fb[256 * 256] __attribute__((aligned(32)));          /* RGB565 texture source */

static void ConvertFrame(void) {
  const uint32_t *src = (const uint32_t *)g_my_pixels;
  uint16_t *dst = g_fb;
  for (int y = 0; y < 224; y++) {
    for (int x = 0; x < 256; x += 4) {
      uint32_t p0 = src[x + 0];
      uint32_t p1 = src[x + 1];
      uint32_t p2 = src[x + 2];
      uint32_t p3 = src[x + 3];

      dst[x + 0] = (uint16_t)(((p0 >> 8) & 0xF800) | ((p0 >> 5) & 0x07E0) | ((p0 >> 3) & 0x001F));
      dst[x + 1] = (uint16_t)(((p1 >> 8) & 0xF800) | ((p1 >> 5) & 0x07E0) | ((p1 >> 3) & 0x001F));
      dst[x + 2] = (uint16_t)(((p2 >> 8) & 0xF800) | ((p2 >> 5) & 0x07E0) | ((p2 >> 3) & 0x001F));
      dst[x + 3] = (uint16_t)(((p3 >> 8) & 0xF800) | ((p3 >> 5) & 0x07E0) | ((p3 >> 3) & 0x001F));
    }
    src += 256;
    dst += 256;
  }
}

/* Tiny 3x5 digit font for the timing overlay (A+B+X+Y toggles it). */
static void DrawNumber(int x, int y, int value, uint16_t color) {
  static const uint16_t kDigits[10] = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF
  };
  char buf[12];
  int n = snprintf(buf, sizeof(buf), "%d", value);
  for (int i = 0; i < n; i++, x += 8) {
    uint16_t g = kDigits[buf[i] - '0'];
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 3; col++)
        if (g & (1 << (14 - row * 3 - col)))
          for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 2; dx++)
              g_fb[(y + row * 2 + dy) * 256 + x + col * 2 + dx] = color;
  }
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;

  if (dc_init() != 0)
    dc_fatal("Video / PVR initialisation failed");

  /* What smw.ini used to provide. */
  g_config.new_renderer = true;
  g_config.enable_audio = true;
  g_config.audio_freq = kAudioRate;
  g_config.audio_channels = 2;
  g_config.audio_samples = 512;

  LoadAssets();

  if (!SnesInit(NULL, 0))          /* no ROM: pure C reimplementation */
    Die("SnesInit failed");

  g_spc_player = SmwSpcPlayer_Create();
  g_spc_player->initialize(g_spc_player);

  PpuBeginDrawing(g_snes->ppu, g_pixels, 256 * 4, 0);
  PpuBeginDrawing(g_my_ppu, g_my_pixels, 256 * 4, 0);

  RtlReadSram();

  if (dc_audio_start(AudioFill, kAudioRate) != 0)
    Die("Audio initialisation failed");

  const uint64_t kFrameUs = 16667;
  int quit = 0, show_perf = 0, skip_now = 0;
  uint32_t logic_us = 0, draw_us = 0, skipped = 0;

  while (!quit) {
    uint64_t t0 = dc_time_us();

    int p1 = 0, p2 = 0;
    uint32_t pad1 = dc_read_pad(0, &quit, &p1);
    uint32_t pad2 = dc_read_pad(1, NULL, &p2);
    uint32_t inputs = pad1 | (pad2 << 12) | ((uint32_t)((p1 ? 1 : 0) | (p2 ? 2 : 0)) << 30);

    if (dc_perf_toggle_pressed(0))
      show_perf = !show_perf;

    RtlRunFrame(inputs);
    dc_audio_poll();
    uint64_t t1 = dc_time_us();
    logic_us = (uint32_t)(t1 - t0);

    if (!skip_now) {
      g_rtl_game_info->draw_ppu_frame();
      uint64_t t_ppu = dc_time_us();

      ConvertFrame();
      uint64_t t2 = dc_time_us();

      logic_us = (uint32_t)(t_ppu - t1);
      draw_us = (uint32_t)(t2 - t_ppu);
      if (show_perf) {
        DrawNumber(4, 4, (int)(logic_us / 100), 0xFFFF);
        DrawNumber(4, 18, (int)(draw_us / 100), 0x07FF);
        DrawNumber(4, 32, (int)skipped, 0xFFE0);
      }
      dc_present(g_fb);
      /* Too slow to make the next vblank? drop the next picture, keep game speed. */
      skip_now = (t2 - t0) > (kFrameUs - 1200);
    } else {
      dc_wait_vbl();
      skipped++;
      skip_now = 0;                      /* always draw after a skipped frame */
    }
  }

  dc_shutdown();
  return 0;
}
