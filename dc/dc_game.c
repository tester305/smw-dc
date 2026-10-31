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
#include <dc/biosfont.h>

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
static uint8 g_pixels[256 * 4 * 240] __attribute__((aligned(32))); /* emulator PPU (unused) */
static uint16_t g_my_pixels[256 * 240] __attribute__((aligned(32))); /* direct RGB565 PVR source */

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
              g_my_pixels[(y + row * 2 + dy) * 256 + x + col * 2 + dx] = color;
  }
}

/* ---- lightweight Dreamcast front-end menu ---- */
static void MenuText(int x, int y, uint16_t color, const char *text) {
  bfont_draw_str_ex(&g_my_pixels[y * 256 + x], 256, color, 0x0000, 16, true, text);
}

static void DrawMenuBackground(unsigned frame) {
  for (int y = 0; y < 240; ++y) {
    for (int x = 0; x < 256; ++x) {
      unsigned v = (unsigned)(x * 13 + y * 7 + frame * 3);
      uint16_t color = ((v % 47u) == 0u && ((x + frame) % 5u) == 0u)
                         ? 0x0208 : 0x0000;
      g_my_pixels[y * 256 + x] = color;
    }
  }
  /* Subtle moving green code columns. */
  for (int col = 0; col < 16; ++col) {
    int x = (col * 17 + (int)(frame * (col % 3 + 1))) & 255;
    for (int y = 0; y < 240; y += 9) {
      int yy = (y + (int)(frame * 2 + col * 11)) % 240;
      g_my_pixels[yy * 256 + x] = 0x03E0;
    }
  }
}

static void DrawMainMenu(int selection, unsigned frame, int info_page) {
  DrawMenuBackground(frame);
  MenuText(48, 34, 0x07E0, "SUPER MARIO WORLD");
  MenuText(66, 58, 0xFFFF, "DREAMCAST EDITION");
  if (info_page) {
    MenuText(36, 104, 0x07E0, "VMU SAVE INFORMATION");
    MenuText(24, 132, 0xFFFF, "SRAM SAVE: /vmu/a1/SMW.SRM");
    MenuText(24, 154, 0xFFFF, "CHEATS USE A SEPARATE FILE");
    MenuText(48, 202, 0xFFE0, "B: BACK");
  } else {
    static const char *items[] = {
      "START GAME", "VMU SAVE INFO", "CHEAT MANAGER"
    };
    for (int i = 0; i < 3; ++i) {
      uint16_t color = (i == selection) ? 0xFFE0 : 0xFFFF;
      if (i == selection) MenuText(45, 105 + i * 25, 0x07E0, ">");
      MenuText(62, 105 + i * 25, color, items[i]);
    }
    MenuText(28, 202, 0x07E0, "DPAD: MOVE   A: SELECT");
    MenuText(78, 218, 0xFFFF, "B: BACK");
  }
}

static int RunMainMenu(int *quit) {
  int selection = 0, info_page = 0;
  uint32_t previous = 0;
  unsigned frame = 0;
  bfont_set_encoding(BFONT_CODE_ISO8859_1);
  while (!*quit) {
    int present = 0;
    uint32_t pad = dc_read_pad(0, quit, &present);
    uint32_t pressed = pad & ~previous;
    previous = pad;
    if (pressed & DC_PAD_UP) selection = (selection + 2) % 3;
    if (pressed & DC_PAD_DOWN) selection = (selection + 1) % 3;
    if (pressed & DC_PAD_B) {
      if (info_page) info_page = 0;
      else return 0;
    }
    if (pressed & DC_PAD_A) {
      if (info_page) info_page = 0;
      else if (selection == 0) return 1;
      else if (selection == 1) info_page = 1;
      else info_page = 2;
    }
    if (info_page == 2) {
      DrawMenuBackground(frame);
      MenuText(48, 34, 0x07E0, "CHEAT MANAGER");
      MenuText(22, 104, 0xFFFF, "CHEAT EDITOR IS NOT READY");
      MenuText(22, 128, 0xFFFF, "GAME GENIE SUPPORT COMES NEXT");
      MenuText(48, 202, 0xFFE0, "B: BACK");
      if (pressed & DC_PAD_B) info_page = 0;
    } else {
      DrawMainMenu(selection, frame, info_page);
    }
    dc_present(g_my_pixels);
    dc_wait_vbl();
    ++frame;
  }
  return 0;
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
  PpuBeginDrawing(g_my_ppu, (uint8_t *)g_my_pixels, 256 * sizeof(g_my_pixels[0]), 0);

  RtlReadSram();

  if (dc_audio_start(AudioFill, kAudioRate) != 0)
    Die("Audio initialisation failed");

  int quit = 0;\n  if (!RunMainMenu(&quit) || quit) { dc_shutdown(); return 0; }\n\n  const uint64_t kFrameUs = 16667;
  int show_perf = 0, skip_now = 0;
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
      uint64_t t2 = dc_time_us();

      /* Keep game/CPU time separate from PPU rendering (direct RGB565). */
      draw_us = (uint32_t)(t2 - t1);
      if (show_perf) {
        DrawNumber(4, 4, (int)(logic_us / 100), 0xFFFF);
        DrawNumber(4, 18, (int)(draw_us / 100), 0x07FF);
        DrawNumber(4, 32, (int)skipped, 0xFFE0);
      }
      dc_present(g_my_pixels);
      uint64_t t3 = dc_time_us();
      /* Include presentation/upload overhead in the frame budget too. */
      skip_now = (t3 - t0) > (kFrameUs - 1200);
    } else {
      dc_wait_vbl();
      skipped++;
      skip_now = 0;                      /* always draw after a skipped frame */
    }
  }

  dc_shutdown();
  return 0;
}
