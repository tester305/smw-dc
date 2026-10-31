#include <stdint.h>
#include <stdio.h>

/* Mirrors the two 256-entry plane LUTs emitted by patch_smw.py. */
static uint16_t plane_lut[2][256];

static uint32_t decode_2bpp(uint32_t planes) {
  return plane_lut[0][planes & 0xff] |
         plane_lut[1][(planes >> 8) & 0xff];
}

static unsigned reference_pixel(uint32_t planes, unsigned i) {
  return ((planes >> i) & 1u) | ((planes >> (7u + i)) & 2u);
}

int main(void) {
  for (unsigned plane = 0; plane < 2; ++plane) {
    for (unsigned value = 0; value < 256; ++value) {
      uint16_t packed = 0;
      for (unsigned pixel = 0; pixel < 8; ++pixel) {
        if (value & (1u << pixel))
          packed |= (uint16_t)(1u << (pixel * 2u + plane));
      }
      plane_lut[plane][value] = packed;
    }
  }

  /* Exhaust every possible pair of 8-bit SNES bitplanes. */
  for (uint32_t planes = 0; planes < 65536u; ++planes) {
    uint32_t decoded = decode_2bpp(planes);
    for (unsigned i = 0; i < 8; ++i) {
      unsigned fast = (decoded >> (i * 2u)) & 3u;
      unsigned flipped = (decoded >> ((7u - i) * 2u)) & 3u;
      if (fast != reference_pixel(planes, i) ||
          flipped != reference_pixel(planes, 7u - i)) {
        fprintf(stderr, "2bpp LUT mismatch: planes=%04x pixel=%u\\n",
                (unsigned)planes, i);
        return 1;
      }
    }
  }
  puts("2bpp LUT matches the original decoder for all 65,536 plane pairs");
  return 0;
}
