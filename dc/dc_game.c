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
#include "dc_cheats.h"

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
    Die("ASSET FILE OPEN/READ FAILED");

  static const char kSig[] = { kAssets_Sig };
  if (length < 16 + 32 + 32 + 8 + kNumberOfAssets * 4 ||
      memcmp(data, kSig, 48) != 0 ||
      *(uint32 *)(data + 80) != kNumberOfAssets)
    Die("ASSET HEADER INVALID");

  uint32 offset = 88 + kNumberOfAssets * 4 + *(uint32 *)(data + 84);
  for (size_t i = 0; i < kNumberOfAssets; i++) {
    uint32 size = *(uint32 *)(data + 88 + i * 4);
    offset = (offset + 3) & ~3u;
    if ((uint64)offset + size > length)
      Die("ASSET DATA OUT OF BOUNDS");
    g_asset_sizes[i] = size;
    g_asset_ptrs[i] = data + offset;
    offset += size;
  }
}
/* ---- audio: the DSP renders 534 stereo frames per game frame at 32 kHz ---- */
enum { kAudioRate = 32000, kAudioBlock = 534 };
static int16 g_audio_block[kAudioBlock * 2];
static int g_audio_pos = kAudioBlock;
static volatile int g_menu_music;
static uint32_t g_menu_sample;
static uint32_t g_menu_phase;

static void AudioFill(int16_t *dst, int frames) {
  if (g_menu_music) {
    static const uint16_t notes[] = { 523, 659, 784, 659, 587, 698, 880, 698 };
    for (int i = 0; i < frames; ++i) {
      uint32_t note = (g_menu_sample / 4000u) & 7u;
      g_menu_phase += ((uint32_t)notes[note] << 16) / kAudioRate;
      int16_t sample = (g_menu_phase & 0x8000u) ? 1800 : -1800;
      dst[i * 2] = sample;
      dst[i * 2 + 1] = sample;
      ++g_menu_sample;
    }
    return;
  }
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
static uint16_t g_my_pixels[256 * 256] __attribute__((aligned(32))); /* full 256x256 PVR texture; visible game area is 256x224 */

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
  (void)frame;
  dc_menu_init();
  dc_menu_clear();
  if (info_page) {
    dc_menu_draw_str(100, "VMU SAVE INFORMATION", 0);
    dc_menu_draw_str(150, "SRAM: /vmu/a1/SMW.SRM", 0);
    dc_menu_draw_str(190, "CHEATS: SMW.CHT", 0);
    dc_menu_draw_str(280, "B: BACK", 0);
  } else {
    dc_menu_draw_str(90, "SUPER MARIO WORLD", 0);
    dc_menu_draw_str(130, "DREAMCAST EDITION", 0);
    dc_menu_draw_str(220, "START GAME", selection == 0);
    dc_menu_draw_str(260, "VMU SAVE INFO", selection == 1);
    dc_menu_draw_str(300, "CHEAT MANAGER", selection == 2);
    dc_menu_draw_str(370, "DPAD MOVE   A SELECT", 0);
    dc_menu_draw_str(410, "B BACK", 0);
  }
  dc_menu_present();
}

static void RunCheatManager(int *quit) {
  static const char hex[] = "0123456789ABCDEF";
  char code[9] = "00000000";
  int editing = 0, selected = 0, char_pos = 0;
  uint32_t previous = 0;
  unsigned frame = 0;
  while (!*quit) {
    int present = 0;
    uint32_t pad = dc_read_pad(0, quit, &present);
    uint32_t pressed = pad & ~previous;
    previous = pad;
    if (editing) {
      if (pressed & DC_PAD_LEFT) code[char_pos] = hex[(strchr(hex, code[char_pos]) - hex + 15) & 15];
      if (pressed & DC_PAD_RIGHT) code[char_pos] = hex[(strchr(hex, code[char_pos]) - hex + 1) & 15];
      if (pressed & DC_PAD_A) {
        if (char_pos < 7) ++char_pos;
        else {
          int result = dc_cheats_add(code);
          if (result == 0) editing = 0;
        }
      }
      if (pressed & DC_PAD_B) editing = 0;
      if (pressed & DC_PAD_START) {
        if (dc_cheats_add(code) == 0) editing = 0;
      }
      DrawMenuBackground(frame);
      MenuText(14, 34, 0x07E0, "ADD GAME GENIE CODE");
      char shown[12];
      snprintf(shown, sizeof(shown), "%.4s-%.4s", code, code + 4);
      MenuText(50, 94, 0xFFFF, shown);
      MenuText(50 + char_pos * 12 + (char_pos >= 4 ? 12 : 0), 122, 0x07E0, "^");
      MenuText(2, 158, 0xFFFF, "LEFT/RIGHT: HEX");
      MenuText(20, 182, 0xFFE0, "A NEXT  START SAVE");
      MenuText(88, 198, 0xFFFF, "B CANCEL");
    } else {
      int count = dc_cheats_count();
      if (pressed & DC_PAD_UP && count) selected = (selected + count - 1) % count;
      if (pressed & DC_PAD_DOWN && count) selected = (selected + 1) % count;
      if (pressed & DC_PAD_B) return;
      if (pressed & DC_PAD_Y && count < DC_CHEAT_MAX) {
        memset(code, '0', 8); code[8] = 0;
        char_pos = 0; editing = 1;
      }
      if (pressed & DC_PAD_A && count) dc_cheats_toggle(selected);
      if (pressed & DC_PAD_X && count) {
        dc_cheats_remove(selected);
        if (selected >= dc_cheats_count() && selected > 0) --selected;
      }
      DrawMenuBackground(frame);
      MenuText(48, 28, 0x07E0, "CHEAT MANAGER");
      if (!count) MenuText(30, 78, 0xFFFF, "NO SAVED CHEATS");
      for (int i = 0; i < count && i < 5; ++i) {
        const dc_cheat_t *ch = dc_cheats_get(i);
        char line[24];
        snprintf(line, sizeof(line), "%c %.4s-%.4s", ch->enabled ? '*' : '-', ch->code, ch->code + 4);
        MenuText(24, 62 + i * 24, i == selected ? 0xFFE0 : 0xFFFF, line);
      }
      MenuText(8, 174, 0x07E0, "Y ADD A TOGGLE X DEL");
      MenuText(90, 198, 0xFFFF, "B BACK");
    }
    dc_present(g_my_pixels);
    dc_audio_poll();
    dc_wait_vbl();
    ++frame;
  }
}

static int RunMainMenu(int *quit) {
  /* SMB-DC style edge-triggered menu: draw first, then poll controls and
     debounce directional input so a held button doesn't race through items. */
  int selection = 0, info_page = 0;
  uint32_t previous = 0;
  int debounce = 0;
  unsigned frame = 0;

  dc_menu_init();
  g_menu_music = 1;
  DrawMainMenu(selection, frame, info_page);

  while (!*quit) {
    int present = 0;
    uint32_t pad = dc_read_pad(0, quit, &present);
    uint32_t pressed = pad & ~previous;
    previous = pad;

    if (debounce > 0) {
      --debounce;
    } else {
      if (pressed & DC_PAD_UP) {
        selection = (selection + 2) % 3;
        debounce = 12;
      } else if (pressed & DC_PAD_DOWN) {
        selection = (selection + 1) % 3;
        debounce = 12;
      }

      if (pressed & DC_PAD_B) {
        if (info_page) info_page = 0;
        else {
          g_menu_music = 0;
          g_audio_pos = kAudioBlock;
          return 0;
        }
      }

      if (pressed & DC_PAD_A) {
        if (info_page) info_page = 0;
        else if (selection == 0) {
          g_menu_music = 0;
          g_audio_pos = kAudioBlock;
          return 1;
        } else if (selection == 1) {
          info_page = 1;
        } else {
          RunCheatManager(quit);
          previous = 0;
        }
        debounce = 12;
      }
    }

    DrawMainMenu(selection, frame, info_page);
    dc_audio_poll();
    dc_wait_vbl();
    ++frame;
  }

  g_menu_music = 0;
  g_audio_pos = kAudioBlock;
  return 0;
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;

  if (dc_init() != 0)
    dc_fatal("VIDEO INIT FAILED");

  /* What smw.ini used to provide. */
  g_config.new_renderer = true;
  g_config.enable_audio = true;
  g_config.audio_freq = kAudioRate;
  g_config.audio_channels = 2;
  g_config.audio_samples = 512;

  /* Match smb-dc: draw and run the BIOS-framebuffer menu before audio/PVR/game init. */
  dc_cheats_init();
  int quit = 0;
  if (!RunMainMenu(&quit) || quit) {
    dc_shutdown();
    return 0;
  }

  /* Start audio only after the user chooses Start, as in smb-dc. */
  g_menu_music = 0;
  if (dc_audio_start(AudioFill, kAudioRate) != 0)
    Die("AUDIO INIT FAILED");

  LoadAssets();

  if (!SnesInit(NULL, 0))          /* no ROM: pure C reimplementation */
    Die("SNES INIT FAILED");

  g_spc_player = SmwSpcPlayer_Create();
  if (!g_spc_player) Die("SPC PLAYER CREATE FAILED");
  g_spc_player->initialize(g_spc_player);

  PpuBeginDrawing(g_snes->ppu, g_pixels, 256 * 4, 0);
  PpuBeginDrawing(g_my_ppu, (uint8_t *)g_my_pixels, 256 * sizeof(g_my_pixels[0]), 0);

  RtlReadSram();

  const uint64_t kFrameUs = 16667;
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
