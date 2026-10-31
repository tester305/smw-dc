#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "dc_cheats.h"

/* The unit test does not need a VMU. */
int dc_cheats_load(uint8_t *dst, size_t size) { (void)dst; (void)size; return -1; }
int dc_cheats_store(const uint8_t *src, size_t size) { (void)src; (void)size; return 0; }

int main(void) {
  uint32_t address = 0;
  uint8_t value = 0;
  assert(dc_cheats_decode_code("C2AC-346F", &address, &value));
  assert(address == 0x80C7AA);
  assert(value == 0xAD);
  assert(dc_cheats_decode_code("c2ac346f", &address, &value));
  assert(address == 0x80C7AA && value == 0xAD);
  assert(!dc_cheats_decode_code("C2AC-346G", &address, &value));
  assert(!dc_cheats_decode_code("1234567", &address, &value));
  puts("SNES Game Genie decoder tests passed");
  return 0;
}
