#ifndef DC_CHEATS_H
#define DC_CHEATS_H
#include <stdint.h>
#define DC_CHEAT_MAX 8
typedef struct dc_cheat {
  char code[9];
  uint32_t address;
  uint8_t value;
  uint8_t enabled;
} dc_cheat_t;
void dc_cheats_init(void);
int dc_cheats_count(void);
const dc_cheat_t *dc_cheats_get(int index);
int dc_cheats_add(const char *code);
void dc_cheats_toggle(int index);
void dc_cheats_remove(int index);
uint8_t dc_cheats_read(uint32_t address, uint8_t original);
int dc_cheats_decode_code(const char *code, uint32_t *address, uint8_t *value);
#endif
