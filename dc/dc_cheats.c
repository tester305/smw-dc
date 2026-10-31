#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include "dc_cheats.h"
#include "dc_platform.h"

static dc_cheat_t g_cheats[DC_CHEAT_MAX];
static int g_count;

int dc_cheats_decode_code(const char *input, uint32_t *address, uint8_t *value) {
  static const char encoded[] = "df4709156bc8a23e";
  static const char plain[] = "0123456789abcdef";
  char code[9];
  uint32_t raw = 0;
  int n = 0;
  for (; *input; ++input) {
    if (*input == '-') continue;
    if (n >= 8) return 0;
    char ch = (char)tolower((unsigned char)*input);
    const char *p = strchr(encoded, ch);
    if (!p) return 0;
    code[n++] = plain[p - encoded];
  }
  if (n != 8) return 0;
  code[8] = 0;
  for (int i = 0; i < 8; ++i) {
    const char *p = strchr(plain, code[i]);
    raw = (raw << 4) | (uint32_t)(p - plain);
  }
#define BIT(src, dst) (((raw & (src)) != 0u) << (dst))
  *address = BIT(0x002000,23) | BIT(0x001000,22) | BIT(0x000800,21) |
    BIT(0x000400,20) | BIT(0x000020,19) | BIT(0x000010,18) |
    BIT(0x000008,17) | BIT(0x000004,16) | BIT(0x800000,15) |
    BIT(0x400000,14) | BIT(0x200000,13) | BIT(0x100000,12) |
    BIT(0x000002,11) | BIT(0x000001,10) | BIT(0x008000,9) |
    BIT(0x004000,8) | BIT(0x080000,7) | BIT(0x040000,6) |
    BIT(0x020000,5) | BIT(0x010000,4) | BIT(0x000200,3) |
    BIT(0x000100,2) | BIT(0x000080,1) | BIT(0x000040,0);
#undef BIT
  *value = (uint8_t)(raw >> 24);
  return 1;
}

void dc_cheats_init(void) {
  memset(g_cheats, 0, sizeof(g_cheats));
  g_count = 0;
  if (dc_cheats_load((uint8_t *)g_cheats, sizeof(g_cheats)) == 0) {
    for (int i = 0; i < DC_CHEAT_MAX; ++i) {
      if (g_cheats[i].code[0] && g_cheats[i].code[8] == 0 &&
          dc_cheats_decode_code(g_cheats[i].code, &g_cheats[i].address, &g_cheats[i].value)) {
        if (g_cheats[i].enabled > 1) g_cheats[i].enabled = 0;
        g_count = i + 1;
      } else {
        memset(&g_cheats[i], 0, sizeof(g_cheats[i]));
      }
    }
  }
}

static int save(void) {
  return dc_cheats_store((const uint8_t *)g_cheats, sizeof(g_cheats));
}
int dc_cheats_count(void) { return g_count; }
const dc_cheat_t *dc_cheats_get(int i) {
  return (i >= 0 && i < g_count) ? &g_cheats[i] : NULL;
}
int dc_cheats_add(const char *code) {
  if (g_count >= DC_CHEAT_MAX) return -1;
  uint32_t address; uint8_t value;
  if (!dc_cheats_decode_code(code, &address, &value)) return -2;
  dc_cheat_t *c = &g_cheats[g_count];
  memset(c, 0, sizeof(*c));
  int n = 0;
  for (; *code && n < 8; ++code) if (*code != '-') c->code[n++] = (char)toupper((unsigned char)*code);
  c->address = address; c->value = value; c->enabled = 1;
  ++g_count;
  if (save() != 0) { --g_count; memset(c, 0, sizeof(*c)); return -3; }
  return 0;
}
void dc_cheats_toggle(int i) {
  if (i < 0 || i >= g_count) return;
  g_cheats[i].enabled = !g_cheats[i].enabled;
  if (save() != 0) g_cheats[i].enabled = !g_cheats[i].enabled;
}
void dc_cheats_remove(int i) {
  if (i < 0 || i >= g_count) return;
  dc_cheat_t old[DC_CHEAT_MAX];
  memcpy(old, g_cheats, sizeof(old));
  for (int n = i; n < g_count - 1; ++n) g_cheats[n] = g_cheats[n + 1];
  memset(&g_cheats[--g_count], 0, sizeof(g_cheats[0]));
  if (save() != 0) { memcpy(g_cheats, old, sizeof(old)); ++g_count; }
}
uint8_t dc_cheats_read(uint32_t address, uint8_t original) {
  for (int i = 0; i < g_count; ++i)
    if (g_cheats[i].enabled && (g_cheats[i].address & 0x7fffffu) == (address & 0x7fffffu))
      return g_cheats[i].value;
  return original;
}
