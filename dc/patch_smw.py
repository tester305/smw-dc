#!/usr/bin/env python3
"""
Make the upstream snesrev/smw sources safe for the Dreamcast's SH-4.

The SH-4 raises an address error on a 16/32-bit access to an odd / misaligned
address. The game logic reads and writes its emulated RAM through casts such as
*(uint16 *)&g_ram[0x13] and WORD(x), which is fine on x86 but fatal here.
GCC emits byte-wise access for a type declared with aligned(1), so we point
those casts at such types (uint16u / uint32u).

Usage: patch_smw.py <path-to-smw-checkout>      (idempotent)
"""
import re, sys, pathlib


def read(path):
    with open(path, "r", newline="") as f:      # keep upstream line endings (CRLF)
        return f.read()


def write(path, text):
    with open(path, "w", newline="") as f:
        f.write(text)


root = pathlib.Path(sys.argv[1])
MARK = "/* DC-PATCH: unaligned-safe types */"

# --- 1. define the unaligned-safe types in types.h -------------------------
types_h = root / "src/types.h"
t = read(types_h)
eol = "\r\n" if "\r\n" in t else "\n"
if MARK not in t:
    anchor = "typedef uint32_t uint32;"
    if anchor not in t:
        sys.exit("types.h: anchor not found - upstream changed, update patch_smw.py")
    t = t.replace(anchor, anchor + eol + MARK + eol +
                  "typedef uint16_t __attribute__((aligned(1))) uint16u;" + eol +
                  "typedef uint32_t __attribute__((aligned(1))) uint32u;", 1)
    write(types_h, t)

# --- 2. retarget the casts in game-logic files -----------------------------
cast16 = re.compile(r"\((const )?uint16(_t)? ?\*\)")
cast32 = re.compile(r"\((const )?uint32(_t)? ?\*\)")
var16 = re.compile(r"\buint16 \*(\w+) = \((const )?uint16u \*\)")
var32 = re.compile(r"\buint32 \*(\w+) = \((const )?uint32u \*\)")

files = ["src/types.h", "src/common_rtl.h", "src/common_rtl.c", "src/common_cpu_infra.c",
         "src/lm.c", "src/util.c", "src/smw_cpu_infra.c", "src/smw_rtl.c",
         "src/smw_spc_player.c"] + \
        [f"src/smw_{n}.c" for n in ("00", "01", "02", "03", "04", "05", "07", "0c", "0d")]

changed = 0
for rel in files:
    path = root / rel
    if not path.exists():
        continue
    s = read(path)
    o = s
    s = cast16.sub(lambda m: f"({m.group(1) or ''}uint16u *)", s)
    s = cast32.sub(lambda m: f"({m.group(1) or ''}uint32u *)", s)
    # a pointer variable initialised from such a cast must keep the weak alignment
    s = var16.sub(lambda m: f"{'const ' if m.group(2) else ''}uint16u *{m.group(1)} = ({m.group(2) or ''}uint16u *)", s)
    s = var32.sub(lambda m: f"{'const ' if m.group(2) else ''}uint32u *{m.group(1)} = ({m.group(2) or ''}uint32u *)", s)
    if s != o:
        write(path, s)
        changed += 1
print(f"patch_smw: {changed} files updated")

# --- 3. avoid collision with KallistiOS's hardware dma_init ---------------
# All C/header files here belong to the upstream SNES emulator, so rename
# its symbol and every declaration/call without touching KallistiOS.
dma_symbol = re.compile(r"\bdma_init\b")
dma_changed = 0

for path in root.rglob("*"):
    if path.is_file() and path.suffix in {".c", ".h"}:
        s = read(path)
        patched = dma_symbol.sub("snes_dma_init", s)
        if patched != s:
            write(path, patched)
            dma_changed += 1

print(f"patch_smw: renamed SNES dma_init in {dma_changed} files")
# --- 4. cache palette-to-RGB conversion in the PPU scanline compositor ----
ppu_path = root / "src/snes/ppu.c"
ppu = read(ppu_path)
ppu_mark = "/* DC-PATCH: cached palette RGB */"

if ppu_mark not in ppu:
    ppu_eol = "\r\n" if "\r\n" in ppu else "\n"
    dst_anchor = "  uint32 *dst = (uint32*)&ppu->renderBuffer[(y - 1) * ppu->renderPitch], *dst_org = dst;"
    old_pixel = (
        "        uint32 color = ppu->cgram[ppu->bgBuffers[0].data[i] & 0xff];" + ppu_eol +
        "        dst[0] = ppu->brightnessMult[color & clip_color_mask] << 16 |" + ppu_eol +
        "          ppu->brightnessMult[(color >> 5) & clip_color_mask] << 8 |" + ppu_eol +
        "          ppu->brightnessMult[(color >> 10) & clip_color_mask];"
    )
    if dst_anchor not in ppu or old_pixel not in ppu:
        sys.exit("ppu.c: palette-cache anchors not found - upstream changed, update patch_smw.py")

    cache_code = ppu_mark + ppu_eol + (
        "  static Ppu *dc_palette_cache_ppu;" + ppu_eol +
        "  static uint16_t dc_palette_cache_cgram[256];" + ppu_eol +
        "  static uint8_t dc_palette_cache_brightness[sizeof(((Ppu *)0)->brightnessMult)];" + ppu_eol +
        "  static uint32_t dc_palette_rgb[2][256];" + ppu_eol +
        "  static bool dc_palette_cache_valid;" + ppu_eol +
        "  if (!dc_palette_cache_valid || dc_palette_cache_ppu != ppu ||" + ppu_eol +
        "      memcmp(dc_palette_cache_cgram, ppu->cgram, sizeof(ppu->cgram)) != 0 ||" + ppu_eol +
        "      memcmp(dc_palette_cache_brightness, ppu->brightnessMult, sizeof(ppu->brightnessMult)) != 0) {" + ppu_eol +
        "    for (uint32_t c = 0; c < 256; c++) {" + ppu_eol +
        "      uint32_t color = ppu->cgram[c];" + ppu_eol +
        "      dc_palette_rgb[0][c] = 0;  /* clip mask 0 makes all RGB channels zero */" + ppu_eol +
        "      dc_palette_rgb[1][c] =" + ppu_eol +
        "          (uint32_t)ppu->brightnessMult[color & 0x1f] << 16 |" + ppu_eol +
        "          (uint32_t)ppu->brightnessMult[(color >> 5) & 0x1f] << 8 |" + ppu_eol +
        "          (uint32_t)ppu->brightnessMult[(color >> 10) & 0x1f];" + ppu_eol +
        "    }" + ppu_eol +
        "    memcpy(dc_palette_cache_cgram, ppu->cgram, sizeof(ppu->cgram));" + ppu_eol +
        "    memcpy(dc_palette_cache_brightness, ppu->brightnessMult, sizeof(ppu->brightnessMult));" + ppu_eol +
        "    dc_palette_cache_ppu = ppu;" + ppu_eol +
        "    dc_palette_cache_valid = true;" + ppu_eol +
        "  }" + ppu_eol + ppu_eol
    )
    ppu = ppu.replace(dst_anchor, cache_code + dst_anchor, 1)
    # Replace the per-pixel palette conversion with a four-pixel unrolled loop.
    # The RGB lookup removes channel math; unrolling cuts loop/control overhead on SH-4.
    old_loop = (
        "      uint32 i = left;" + ppu_eol +
        "      do {" + ppu_eol +
        old_pixel + ppu_eol +
        "      } while (dst++, ++i < right);"
    )
    if old_loop not in ppu:
        sys.exit("ppu.c: fast pixel loop anchor not found - upstream changed, update patch_smw.py")

    fast_loop = ppu_eol.join([
        "      uint32 i = left;",
        "      uint32_t *rgb = dc_palette_rgb[clip_color_mask != 0];",
        "      if (i < right) {",
        "        while (i + 3 < right) {",
        "          dst[0] = rgb[ppu->bgBuffers[0].data[i + 0] & 0xff];",
        "          dst[1] = rgb[ppu->bgBuffers[0].data[i + 1] & 0xff];",
        "          dst[2] = rgb[ppu->bgBuffers[0].data[i + 2] & 0xff];",
        "          dst[3] = rgb[ppu->bgBuffers[0].data[i + 3] & 0xff];",
        "          dst += 4;",
        "          i += 4;",
        "        }",
        "        while (i < right) {",
        "          *dst++ = rgb[ppu->bgBuffers[0].data[i] & 0xff];",
        "          i++;",
        "        }",
        "      } else {",
        "        /* Preserve upstream's do-while behavior for an empty range. */",
        "        *dst++ = rgb[ppu->bgBuffers[0].data[i] & 0xff];",
        "      }",
        "    } else {",
    ])
    ppu = ppu.replace(old_loop + ppu_eol + "    } else {", fast_loop, 1)
    write(ppu_path, ppu)
    print("patch_smw: enabled cached palette RGB conversion + four-pixel loop")
else:
    print("patch_smw: cached palette RGB conversion already present")


# --- 5. invalidate the palette cache on CGRAM writes instead of memcmp per scanline ---
# CGRAM is changed through ppu_write(0x22), and whole-PPU reset/copy/save-load
# operations also invalidate it. This avoids scanning all 256 palette entries
# on every rendered line while retaining a full refresh whenever state changes.
ppu = read(ppu_path)
eol = "\r\n" if "\r\n" in ppu else "\n"
dirty_mark = "/* DC-PATCH: palette cache dirty flag */"
if dirty_mark not in ppu:
    global_anchor = "Ppu* ppu_init(void) {"
    if global_anchor not in ppu:
        sys.exit("ppu.c: ppu_init anchor not found - upstream changed, update patch_smw.py")
    ppu = ppu.replace(global_anchor,
        dirty_mark + eol + "static bool dc_palette_cache_dirty = true;" + eol + eol + global_anchor, 1)

    cache_compare = "memcmp(dc_palette_cache_cgram, ppu->cgram, sizeof(ppu->cgram)) != 0 ||"
    if cache_compare not in ppu:
        sys.exit("ppu.c: palette cache comparison anchor not found - update patch_smw.py")
    ppu = ppu.replace(cache_compare, "dc_palette_cache_dirty ||", 1)
    ppu = re.sub(r"^[ \t]*static uint16_t dc_palette_cache_cgram\[256\];\r?\n", "", ppu, count=1, flags=re.M)
    ppu = re.sub(r"^[ \t]*memcpy\(dc_palette_cache_cgram, ppu->cgram, sizeof\(ppu->cgram\)\);\r?\n", "", ppu, count=1, flags=re.M)
    cache_ready = "    dc_palette_cache_valid = true;"
    if cache_ready not in ppu:
        sys.exit("ppu.c: palette cache refresh anchor not found - update patch_smw.py")
    ppu = ppu.replace(cache_ready,
        "    dc_palette_cache_dirty = false;" + eol + cache_ready, 1)

    cgram_write = "        ppu->cgram[ppu->cgramPointer++] = (val << 8) | ppu->cgramBuffer;"
    if cgram_write not in ppu:
        sys.exit("ppu.c: CGRAM write anchor not found - update patch_smw.py")
    ppu = ppu.replace(cgram_write,
        cgram_write + eol + "        dc_palette_cache_dirty = true;", 1)

    # These operations can replace CGRAM without going through register 0x22.
    for anchor, label in [
        ("  memcpy(ppu, ppu_src, sizeof(*ppu));", "ppu_copy"),
        ("    memset(ppu, 0, sizeof(*ppu));", "ppu_reset"),
        ("  sli->func(sli, &ppu->cgram, PPU_SAVESTATE_MEM_SIZE);", "ppu_saveload"),
    ]:
        if anchor not in ppu:
            sys.exit("ppu.c: " + label + " invalidation anchor not found - update patch_smw.py")
        ppu = ppu.replace(anchor, anchor + eol + ("  " if label != "ppu_reset" else "    ") +
                          "dc_palette_cache_dirty = true;", 1)

    write(ppu_path, ppu)
    print("patch_smw: enabled write-invalidated palette cache")
else:
    print("patch_smw: write-invalidated palette cache already present")


# --- 6. hoist invariant OAM registers out of the per-sprite scan loop ------
# These register fields cannot change while ppu_evaluateSprites() is running.
# Read them once per scanline instead of re-extracting them for each of 128 OAM
# entries. This preserves the existing sprite ordering and limits.
ppu = read(ppu_path)
sprite_eol = "\r\n" if "\r\n" in ppu else "\n"
sprite_start = ppu.find("static bool ppu_evaluateSprites(Ppu* ppu, int line) {")
sprite_end = ppu.find("static uint16_t ppu_getVramRemap(Ppu* ppu) {", sprite_start)
if sprite_start < 0 or sprite_end < 0:
    sys.exit("ppu.c: sprite evaluator anchors not found - update patch_smw.py")
sprite_fn = ppu[sprite_start:sprite_end]
sprite_mark = "/* DC-PATCH: hoisted sprite registers */"
if sprite_mark not in sprite_fn:
    sprite_anchor = "  uint8_t index = PPU_objPriority(ppu) ? (ppu->oamaddl & 0xfe) : 0;"
    if sprite_anchor not in sprite_fn:
        sys.exit("ppu.c: sprite loop anchor not found - update patch_smw.py")
    sprite_fn = sprite_fn.replace(
        sprite_anchor,
        "  " + sprite_mark + sprite_eol +
        "  const uint8_t dc_obj_size = PPU_objSize(ppu);" + sprite_eol +
        "  const bool dc_obj_interlace = PPU_objInterlace(ppu);" + sprite_eol +
        sprite_anchor, 1)
    sprite_fn = sprite_fn.replace("spriteSizes[PPU_objSize(ppu)]",
                                  "spriteSizes[dc_obj_size]", 1)
    sprite_fn = sprite_fn.replace("PPU_objInterlace(ppu) ? spriteSize / 2 : spriteSize",
                                  "dc_obj_interlace ? spriteSize / 2 : spriteSize", 1)
    sprite_fn = sprite_fn.replace("if(PPU_objInterlace(ppu)) row =",
                                  "if(dc_obj_interlace) row =", 1)
    if sprite_fn.count("PPU_objInterlace(ppu)") != 1 or sprite_fn.count("PPU_objSize(ppu)") != 1:
        sys.exit("ppu.c: not all sprite register reads were hoisted - update patch_smw.py")
    ppu = ppu[:sprite_start] + sprite_fn + ppu[sprite_end:]
    write(ppu_path, ppu)
    print("patch_smw: hoisted sprite-size/interlace reads")
else:
    print("patch_smw: sprite-register hoisting already present")


# --- 7. use a compact LUT to decode 4bpp planar pixels ----------------------
# A tile row stores the four bitplanes in four bytes. Expand each byte into
# eight 4-bit pixels once, rather than repeating four bit extractions for
# every pixel in the inner background/sprite loops.
ppu = read(ppu_path)
planar_mark = "/* DC-PATCH: SH-4 4bpp pixel LUT */"
if planar_mark not in ppu:
    planar_code = [
        planar_mark,
        "static uint32_t dc_planar_pixel_lut[4][256];",
        "static bool dc_planar_pixel_lut_ready;",
        "",
        "static void dc_init_planar_pixel_lut(void) {",
        "  if (dc_planar_pixel_lut_ready) return;",
        "  for (unsigned plane = 0; plane < 4; plane++) {",
        "    for (unsigned value = 0; value < 256; value++) {",
        "      uint32_t packed = 0;",
        "      for (unsigned pixel = 0; pixel < 8; pixel++)",
        "        if (value & (1u << pixel))",
        "          packed |= 1u << (pixel * 4 + plane);",
        "      dc_planar_pixel_lut[plane][value] = packed;",
        "    }",
        "  }",
        "  dc_planar_pixel_lut_ready = true;",
        "}",
        "",
        "static inline uint32_t dc_decode_4bpp(uint32_t planes) {",
        "  return dc_planar_pixel_lut[0][planes & 0xff] |",
        "         dc_planar_pixel_lut[1][(planes >> 8) & 0xff] |",
        "         dc_planar_pixel_lut[2][(planes >> 16) & 0xff] |",
        "         dc_planar_pixel_lut[3][planes >> 24];",
        "}",
        "",
        "Ppu* ppu_init(void) {",
    ]
    helper = eol.join(planar_code)
    init_anchor = "Ppu* ppu_init(void) {" 
    if ppu.count(init_anchor) != 1:
        sys.exit("ppu.c: ppu_init anchor not found or ambiguous")
    ppu = ppu.replace(init_anchor, helper, 1)
    alloc_anchor = "Ppu* ppu_init(void) {" + eol + "  Ppu* ppu = malloc(sizeof(Ppu));"
    if alloc_anchor not in ppu:
        sys.exit("ppu.c: ppu_init allocation anchor not found")
    ppu = ppu.replace(alloc_anchor,
        "Ppu* ppu_init(void) {" + eol +
        "  dc_init_planar_pixel_lut();" + eol +
        "  Ppu* ppu = malloc(sizeof(Ppu));", 1)

    # Accelerate the full, aligned 8-pixel tile path. Keep the clipped-edge
    # path untouched because it shifts the source bitplanes after each pixel.
    bg_start = ppu.find("static void PpuDrawBackground_4bpp(Ppu *ppu, uint y, bool sub, uint layer, PpuZbufType zhi, PpuZbufType zlo) {")
    bg_end = ppu.find("// Draw a whole line of a 2bpp background layer", bg_start)
    if bg_start < 0 or bg_end < 0:
        sys.exit("ppu.c: 4bpp background function anchors not found")
    bg_fn = ppu[bg_start:bg_end]
    macro_anchor = "#define READ_BITS(ta, tile)"
    fast_macros = eol.join([
        "#define DO_PIXEL_FAST(i) do { \\",
        "  pixel = (dc_pixels >> ((i) * 4)) & 0xf; \\",
        "  if (pixel && z > dstz[i]) dstz[i] = z + pixel; } while (0)",
        "#define DO_PIXEL_FAST_HFLIP(i) do { \\",
        "  pixel = (dc_pixels >> ((7 - (i)) * 4)) & 0xf; \\",
        "  if (pixel && z > dstz[i]) dstz[i] = z + pixel; } while (0)",
        macro_anchor
    ])
    if bg_fn.count(macro_anchor) != 1:
        sys.exit("ppu.c: 4bpp macro insertion anchor not found")
    bg_fn = bg_fn.replace(macro_anchor, fast_macros, 1)

    old_middle = eol.join([
        "      uint32 bits = READ_BITS(ta, tile & 0x3ff);",
        "      if (bits) {",
        "        z += ((tile & 0x1c00) >> kPaletteShift);",
        "        if (tile & 0x4000) {",
        "          DO_PIXEL(0); DO_PIXEL(1); DO_PIXEL(2); DO_PIXEL(3);",
        "          DO_PIXEL(4); DO_PIXEL(5); DO_PIXEL(6); DO_PIXEL(7);",
        "        } else {",
        "          DO_PIXEL_HFLIP(0); DO_PIXEL_HFLIP(1); DO_PIXEL_HFLIP(2); DO_PIXEL_HFLIP(3);",
        "          DO_PIXEL_HFLIP(4); DO_PIXEL_HFLIP(5); DO_PIXEL_HFLIP(6); DO_PIXEL_HFLIP(7);",
        "        }",
        "      }"
    ])
    new_middle = eol.join([
        "      uint32 bits = READ_BITS(ta, tile & 0x3ff);",
        "      uint32_t dc_pixels = dc_decode_4bpp(bits);",
        "      if (bits) {",
        "        z += ((tile & 0x1c00) >> kPaletteShift);",
        "        if (tile & 0x4000) {",
        "          DO_PIXEL_FAST(0); DO_PIXEL_FAST(1); DO_PIXEL_FAST(2); DO_PIXEL_FAST(3);",
        "          DO_PIXEL_FAST(4); DO_PIXEL_FAST(5); DO_PIXEL_FAST(6); DO_PIXEL_FAST(7);",
        "        } else {",
        "          DO_PIXEL_FAST_HFLIP(0); DO_PIXEL_FAST_HFLIP(1); DO_PIXEL_FAST_HFLIP(2); DO_PIXEL_FAST_HFLIP(3);",
        "          DO_PIXEL_FAST_HFLIP(4); DO_PIXEL_FAST_HFLIP(5); DO_PIXEL_FAST_HFLIP(6); DO_PIXEL_FAST_HFLIP(7);",
        "        }",
        "      }"
    ])
    if bg_fn.count(old_middle) != 1:
        sys.exit("ppu.c: full 4bpp tile loop anchor not found or ambiguous")
    bg_fn = bg_fn.replace(old_middle, new_middle, 1)
    bg_fn = bg_fn.replace("#undef DO_PIXEL_HFLIP",
        "#undef DO_PIXEL_HFLIP" + eol +
        "#undef DO_PIXEL_FAST" + eol +
        "#undef DO_PIXEL_FAST_HFLIP", 1)
    ppu = ppu[:bg_start] + bg_fn + ppu[bg_end:]

    # Sprite rows use exactly the same four-plane pixel encoding.
    sprite_start = ppu.find("static bool ppu_evaluateSprites(Ppu* ppu, int line) {")
    sprite_end = ppu.find("static uint16_t ppu_getVramRemap(Ppu* ppu) {", sprite_start)
    if sprite_start < 0 or sprite_end < 0:
        sys.exit("ppu.c: sprite evaluator anchors not found for LUT")
    sprite_fn = ppu[sprite_start:sprite_end]
    plane_anchor = "            uint32 plane = addr[0] | addr[8] << 16;"
    if sprite_fn.count(plane_anchor) != 1:
        sys.exit("ppu.c: sprite plane anchor not found or ambiguous")
    sprite_fn = sprite_fn.replace(plane_anchor,
        plane_anchor + eol + "            uint32_t dc_pixels = dc_decode_4bpp(plane);", 1)
    pixel_anchor = "              int pixel = (bits >> 0) & 1 | (bits >> 7) & 2 | (bits >> 14) & 4 | (bits >> 21) & 8;"
    if sprite_fn.count(pixel_anchor) != 1:
        sys.exit("ppu.c: sprite pixel decode anchor not found or ambiguous")
    sprite_fn = sprite_fn.replace(pixel_anchor,
        "              int pixel = (dc_pixels >> (shift * 4)) & 0xf;", 1)
    ppu = ppu[:sprite_start] + sprite_fn + ppu[sprite_end:]
    write(ppu_path, ppu)
    print("patch_smw: enabled SH-4 4bpp planar pixel LUT")
else:
    print("patch_smw: 4bpp planar pixel LUT already present")

# --- 8. direct RGB565 output from the PPU compositor ------------------------
# Step 4 has already made the no-math renderer use a cached palette table.
# Store that table as RGB565 and make both composite paths write 16-bit output.
ppu = read(ppu_path)
rgb565_mark = "/* DC-PATCH: direct RGB565 output */"
if rgb565_mark not in ppu:
    whole_start = ppu.find("static NOINLINE void PpuDrawWholeLine(Ppu *ppu, uint y) {")
    whole_end = ppu.find("static bool ppu_evaluateSprites(Ppu* ppu, int line) {", whole_start)
    if whole_start < 0 or whole_end < 0:
        sys.exit("ppu.c: whole-line renderer anchors not found")
    whole_fn = ppu[whole_start:whole_end]
    old = "static uint32_t dc_palette_rgb[2][256];"
    if whole_fn.count(old) != 1:
        sys.exit("ppu.c: palette cache declaration not found")
    whole_fn = whole_fn.replace(old, "static uint16_t dc_palette_rgb[2][256];", 1)
    old = eol.join([
        "      dc_palette_rgb[0][c] = 0;  /* clip mask 0 makes all RGB channels zero */",
        "      dc_palette_rgb[1][c] =",
        "          (uint32_t)ppu->brightnessMult[color & 0x1f] << 16 |",
        "          (uint32_t)ppu->brightnessMult[(color >> 5) & 0x1f] << 8 |",
        "          (uint32_t)ppu->brightnessMult[(color >> 10) & 0x1f];"
    ])
    new = eol.join([
        "      dc_palette_rgb[0][c] = 0;",
        "      uint32_t r8 = ppu->brightnessMult[color & 0x1f];",
        "      uint32_t g8 = ppu->brightnessMult[(color >> 5) & 0x1f];",
        "      uint32_t b8 = ppu->brightnessMult[(color >> 10) & 0x1f];",
        "      dc_palette_rgb[1][c] = (uint16_t)(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));"
    ])
    if whole_fn.count(old) != 1:
        sys.exit("ppu.c: palette cache value anchor not found")
    whole_fn = whole_fn.replace(old, new, 1)
    old = "uint32_t *rgb = dc_palette_rgb[clip_color_mask != 0];"
    if whole_fn.count(old) != 1:
        sys.exit("ppu.c: palette cache pointer anchor not found")
    whole_fn = whole_fn.replace(old, "uint16_t *rgb = dc_palette_rgb[clip_color_mask != 0];", 1)
    old = "size_t n = sizeof(uint32) * (256 + ppu->extraLeftRight * 2);"
    if whole_fn.count(old) != 1:
        sys.exit("ppu.c: forced-blank output size anchor not found")
    whole_fn = whole_fn.replace(old, "size_t n = sizeof(uint16_t) * (256 + ppu->extraLeftRight * 2);", 1)
    old = "uint32 *dst = (uint32*)&ppu->renderBuffer[(y - 1) * ppu->renderPitch], *dst_org = dst;"
    if whole_fn.count(old) != 1:
        sys.exit("ppu.c: render destination anchor not found")
    whole_fn = whole_fn.replace(old, "uint16_t *dst = (uint16_t*)&ppu->renderBuffer[(y - 1) * ppu->renderPitch], *dst_org = dst;", 1)
    old = "        dst[0] = color_map[b] | color_map[g] << 8 | color_map[r] << 16;"
    new = eol.join([
        "        uint32_t r8 = color_map[r];",
        "        uint32_t g8 = color_map[g];",
        "        uint32_t b8 = color_map[b];",
        "        dst[0] = (uint16_t)(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));"
    ])
    if whole_fn.count(old) != 1:
        sys.exit("ppu.c: color-math output anchor not found")
    whole_fn = whole_fn.replace(old, new, 1)
    ppu = ppu[:whole_start] + whole_fn + ppu[whole_end:]
    ppu = ppu.replace("static NOINLINE void PpuDrawWholeLine(Ppu *ppu, uint y) {",
        rgb565_mark + eol + "static NOINLINE void PpuDrawWholeLine(Ppu *ppu, uint y) {", 1)
    write(ppu_path, ppu)
    print("patch_smw: enabled direct RGB565 PPU output")
else:
    print("patch_smw: direct RGB565 PPU output already present")


# --- 9. skip empty 4bpp tile rows before decoding or visiting pixels --------
# Most transparent tile rows need no palette-plane expansion or per-pixel
# depth checks. Keep the original non-empty path byte-for-byte equivalent.
ppu = read(ppu_path)
ppu_eol = "\r\n" if "\r\n" in ppu else "\n"
empty_mark = "/* DC-PATCH: skip empty 4bpp rows */"
if empty_mark not in ppu:
    bg_old = ppu_eol.join([
        "      uint32 bits = READ_BITS(ta, tile & 0x3ff);",
        "      uint32_t dc_pixels = dc_decode_4bpp(bits);",
        "      if (bits) {",
        "        z += ((tile & 0x1c00) >> kPaletteShift);"
    ])
    bg_new = ppu_eol.join([
        "      uint32 bits = READ_BITS(ta, tile & 0x3ff);",
        "      if (bits) {",
        "        uint32_t dc_pixels = dc_decode_4bpp(bits);",
        "        z += ((tile & 0x1c00) >> kPaletteShift);"
    ])
    if ppu.count(bg_old) != 1:
        sys.exit("ppu.c: 4bpp empty-row background anchor not found or ambiguous")
    ppu = ppu.replace(bg_old, bg_new, 1)

    sprite_start = ppu.find("static bool ppu_evaluateSprites(Ppu* ppu, int line) {")
    sprite_end = ppu.find("static uint16_t ppu_getVramRemap(Ppu* ppu) {", sprite_start)
    if sprite_start < 0 or sprite_end < 0:
        sys.exit("ppu.c: sprite evaluator anchors not found for empty-row optimization")
    sprite_fn = ppu[sprite_start:sprite_end]
    sprite_old = ppu_eol.join([
        "            uint32 plane = addr[0] | addr[8] << 16;",
        "            uint32_t dc_pixels = dc_decode_4bpp(plane);",
        "            // go over each pixel",
        "            int px_left = IntMax(-(col + x + kPpuExtraLeftRight), 0);"
    ])
    sprite_new = ppu_eol.join([
        "            uint32 plane = addr[0] | addr[8] << 16;",
        "            if (plane) {",
        "              uint32_t dc_pixels = dc_decode_4bpp(plane);",
        "              // go over each pixel",
        "              int px_left = IntMax(-(col + x + kPpuExtraLeftRight), 0);"
    ])
    if sprite_fn.count(sprite_old) != 1:
        sys.exit("ppu.c: sprite empty-row start anchor not found or ambiguous")
    sprite_fn = sprite_fn.replace(sprite_old, sprite_new, 1)
    sprite_loop_end = ppu_eol.join([
        "            }",
        "",
        "          }"
    ])
    sprite_loop_new_end = ppu_eol.join([
        "            }",
        "            }",
        "",
        "          }"
    ])
    if sprite_fn.count(sprite_loop_end) != 1:
        sys.exit("ppu.c: sprite empty-row end anchor not found or ambiguous")
    sprite_fn = sprite_fn.replace(sprite_loop_end, sprite_loop_new_end, 1)
    ppu = ppu[:sprite_start] + sprite_fn + ppu[sprite_end:]
    ppu = ppu.replace("/* DC-PATCH: direct RGB565 output */",
        empty_mark + eol + "/* DC-PATCH: direct RGB565 output */", 1)
    write(ppu_path, ppu)
    print("patch_smw: skips empty 4bpp background and sprite rows")
else:
    print("patch_smw: empty 4bpp row skipping already present")


# --- 10. memoize the per-pixel colour-math path ------------------------------
# SMW leaves colour math enabled on nearly every scanline (typically "add subscreen"
# with only the backdrop layer enabled, BG2 on the subscreen), so the generic
# per-pixel math loop - ~50 SH-4 instructions per pixel - runs for most of the frame.
# A math pixel's result is a pure function of (main palette index, sub palette index)
# plus per-line state (fixed colour, half/subtract/add-subscreen flags, clip mask,
# palette + brightness).  Memoize it in a table tagged with a generation number that
# changes whenever any of that state changes; pixels whose layer has math disabled
# use the cached palette table directly.  Output is bit-identical to upstream.
ppu = read(ppu_path)
math_eol = "\r\n" if "\r\n" in ppu else "\n"
math_mark = "/* DC-PATCH: memoized colour math */"
if math_mark not in ppu:
    def M(text):
        return text.replace("\n", math_eol)

    whole_start = ppu.find("static NOINLINE void PpuDrawWholeLine(Ppu *ppu, uint y) {")
    whole_end = ppu.find("static bool ppu_evaluateSprites(Ppu* ppu, int line) {", whole_start)
    if whole_start < 0 or whole_end < 0:
        sys.exit("ppu.c: whole-line renderer anchors not found for colour-math memo")
    whole_fn = ppu[whole_start:whole_end]

    # generation counter bumped whenever the cached palette tables are rebuilt
    old = "  static bool dc_palette_cache_valid;"
    if whole_fn.count(old) != 1:
        sys.exit("ppu.c: palette cache valid-flag anchor not found for colour-math memo")
    whole_fn = whole_fn.replace(old, old + math_eol + "  static uint32_t dc_palette_gen;", 1)
    old = "    dc_palette_cache_valid = true;"
    if whole_fn.count(old) != 1:
        sys.exit("ppu.c: palette cache refresh anchor not found for colour-math memo")
    whole_fn = whole_fn.replace(old, "    dc_palette_gen++;" + math_eol + old, 1)

    # replace the whole generic per-pixel loop
    blk_start = whole_fn.find("      uint8 *half_color_map = ")
    end_marker = "      } while (dst++, ++i < right);"
    blk_end = whole_fn.find(end_marker, blk_start)
    if blk_start < 0 or blk_end < 0:
        sys.exit("ppu.c: colour-math loop anchors not found - upstream/patch changed, update patch_smw.py")
    blk_end += len(end_marker)
    if "ppu->cgram[ppu->bgBuffers[0].data[i] & 0xff]" not in whole_fn[blk_start:blk_end]:
        sys.exit("ppu.c: colour-math loop has an unexpected shape - update patch_smw.py")

    new_block = M(r'''      /* DC-PATCH: memoized colour math */
      static uint32_t dc_math_memo[65536];   /* (main idx << 8 | sub idx) -> gen << 16 | rgb565 */
      static uint32_t dc_math_state, dc_math_pal_gen, dc_math_gen;
      uint8 *half_color_map = PPU_halfColor(ppu) ? ppu->brightnessMultHalf : ppu->brightnessMult;
      // Store this in locals
      math_enabled_cur |= PPU_addSubscreen(ppu) << 8 | PPU_subtractColor(ppu) << 9;
      {
        uint32_t state = fixed_color | (uint32_t)(PPU_halfColor(ppu) != 0) << 16 |
                         (uint32_t)(clip_color_mask != 0) << 17 | (math_enabled_cur & 0x300) << 10;
        if (state != dc_math_state || dc_palette_gen != dc_math_pal_gen) {
          dc_math_state = state;
          dc_math_pal_gen = dc_palette_gen;
          if ((dc_math_gen = (dc_math_gen + 1) & 0xffff) == 0) {  // generation wrapped: drop stale tags
            memset(dc_math_memo, 0, sizeof(dc_math_memo));
            dc_math_gen = 1;
          }
        }
      }
      const PpuZbufType *dc_main = ppu->bgBuffers[0].data;
      const PpuZbufType *dc_sub = ppu->bgBuffers[1].data;
      const uint16_t *dc_rgb = dc_palette_rgb[clip_color_mask != 0];
      const uint32_t dc_gen = dc_math_gen;
      const bool dc_addsub = (math_enabled_cur & 0x100) != 0;
      // Need to check for each pixel whether to use math or not based on the main screen layer.
      uint32 i = left;
      do {
        uint32 m = dc_main[i];
        uint32 idx = m & 0xff;
        if (!(math_enabled_cur & (1 << ((m >> 8) & 0xf)))) {
          dst[0] = dc_rgb[idx];  // no math on this layer: same as the cached palette path
        } else {
          uint32 sub = dc_addsub ? (dc_sub[i] & 0xff) : 0;
          uint32 e = dc_math_memo[idx << 8 | sub];
          if ((e >> 16) != dc_gen) {
            uint32 color = ppu->cgram[idx], color2;
            uint32 r = color & clip_color_mask;
            uint32 g = (color >> 5) & clip_color_mask;
            uint32 b = (color >> 10) & clip_color_mask;
            uint8 *color_map = ppu->brightnessMult;
            if (dc_addsub) {  // addSubscreen ?
              if (sub != 0)
                color2 = ppu->cgram[sub], color_map = half_color_map;
              else  // Don't halve if PPU_addSubscreen(ppu) && backdrop
                color2 = fixed_color;
            } else {
              color2 = fixed_color, color_map = half_color_map;
            }
            uint32 r2 = (color2 & 0x1f), g2 = ((color2 >> 5) & 0x1f), b2 = ((color2 >> 10) & 0x1f);
            if (math_enabled_cur & 0x200) {  // subtractColor?
              r = (r >= r2) ? r - r2 : 0;
              g = (g >= g2) ? g - g2 : 0;
              b = (b >= b2) ? b - b2 : 0;
            } else {
              r += r2;
              g += g2;
              b += b2;
            }
            uint32_t r8 = color_map[r];
            uint32_t g8 = color_map[g];
            uint32_t b8 = color_map[b];
            e = dc_gen << 16 | (uint16_t)(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
            dc_math_memo[idx << 8 | sub] = e;
          }
          dst[0] = (uint16_t)e;
        }
      } while (dst++, ++i < right);''')
    whole_fn = whole_fn[:blk_start] + new_block + whole_fn[blk_end:]
    ppu = ppu[:whole_start] + whole_fn + ppu[whole_end:]
    write(ppu_path, ppu)
    print("patch_smw: memoized colour-math compositor path")
else:
    print("patch_smw: colour-math memo already present")


# --- 11. direct CGRAM writers must invalidate the palette cache ---------------
# Section 5 only hooks CGRAM writes made through ppu_write().  The game's C code uploads
# palettes straight into g_ppu->cgram (RtlUpdatePalette) and snapshot loading memcpy()s it,
# so those writes must mark the cache dirty too or an animated palette could go stale.
ppu = read(ppu_path)
dirty_decl_old = "static bool dc_palette_cache_dirty = true;"
dirty_decl_new = "bool dc_palette_cache_dirty = true;  /* DC-PATCH: shared with common_rtl.c / common_cpu_infra.c */"
if dirty_decl_old in ppu:
    ppu = ppu.replace(dirty_decl_old, dirty_decl_new, 1)
    write(ppu_path, ppu)

rtl_path = root / "src/common_rtl.c"
rtl = read(rtl_path)
rtl_eol = "\r\n" if "\r\n" in rtl else "\n"
if "dc_palette_cache_dirty" not in rtl:
    old = "void RtlUpdatePalette(const uint16 *src, int dst, int n) {" + rtl_eol
    if old not in rtl:
        sys.exit("common_rtl.c: RtlUpdatePalette anchor not found - update patch_smw.py")
    rtl = rtl.replace(old,
        "extern bool dc_palette_cache_dirty;  /* DC-PATCH: palette cache invalidation */" + rtl_eol +
        old + "  dc_palette_cache_dirty = true;" + rtl_eol, 1)
    write(rtl_path, rtl)
    print("patch_smw: RtlUpdatePalette invalidates the palette cache")

cpu_path = root / "src/common_cpu_infra.c"
cpu = read(cpu_path)
cpu_eol = "\r\n" if "\r\n" in cpu else "\n"
if "dc_palette_cache_dirty" not in cpu:
    old = "  memcpy(g_ppu->cgram, s->cgram, sizeof(uint16) * 0x100);" + cpu_eol
    if cpu.count(old) != 1:
        sys.exit("common_cpu_infra.c: snapshot cgram load anchor not found - update patch_smw.py")
    cpu = cpu.replace(old,
        old + "  { extern bool dc_palette_cache_dirty; dc_palette_cache_dirty = true; }  /* DC-PATCH */" + cpu_eol, 1)
    write(cpu_path, cpu)
    print("patch_smw: snapshot load invalidates the palette cache")


# --- 12. sprite scan: reject sprites that are not on this line before any other work ----
# ppu_evaluateSprites() walks all 128 OAM entries on every one of the 224 scanlines.  Most
# entries are parked off-screen, so check the Y range against the tallest possible sprite
# first and skip the size/high-OAM lookups for them.  Identical result: a sprite whose row is
# >= the largest size can never satisfy row < spriteHeight for either size.  The OAM base
# pointer is hoisted and the index step moved into the for-header so the rejected path is a
# handful of SH-4 instructions.
ppu = read(ppu_path)
spr_eol = "\r\n" if "\r\n" in ppu else "\n"
spr_mark = "/* DC-PATCH: sprite early-out */"
if spr_mark not in ppu:
    def S(text):
        return text.replace("\n", spr_eol)
    old = S("  const bool dc_obj_interlace = PPU_objInterlace(ppu);\n")
    if ppu.count(old) != 1:
        sys.exit("ppu.c: hoisted sprite register anchor not found for sprite early-out - update patch_smw.py")
    ppu = ppu.replace(old, old + S(
        "  " + spr_mark + "\n"
        "  const uint16 *dc_oam = ppu->oam;\n"
        "  const int dc_obj_max_h = (dc_obj_interlace ? spriteSizes[dc_obj_size][1] / 2 : spriteSizes[dc_obj_size][1]);\n"), 1)
    old = S("  for(int i = 0; i < 128; i++) {\n    uint8_t y = ppu->oam[index] >> 8;\n"
            "    // check if the sprite is on this line and get the sprite size\n"
            "    uint8_t row = line - y;\n    int spriteSize = ")
    if ppu.count(old) != 1:
        sys.exit("ppu.c: sprite scan loop anchor not found for sprite early-out - update patch_smw.py")
    ppu = ppu.replace(old, S(
        "  for(int i = 0; i < 128; i++, index += 2) {\n    uint8_t y = dc_oam[index] >> 8;\n"
        "    // check if the sprite is on this line and get the sprite size\n"
        "    uint8_t row = line - y;\n    if (row >= dc_obj_max_h) continue;\n    int spriteSize = "), 1)
    old = S("      }\n    }\n    index += 2;\n  }\n  return tilesFound != 0;")
    if ppu.count(old) != 1:
        sys.exit("ppu.c: sprite scan loop tail anchor not found for sprite early-out - update patch_smw.py")
    ppu = ppu.replace(old, S("      }\n    }\n  }\n  return tilesFound != 0;"), 1)
    write(ppu_path, ppu)
    print("patch_smw: sprite scan early-out")
else:
    print("patch_smw: sprite early-out already present")


# --- 13. LUT-decode aligned 2bpp background tiles --------------------------
# Mode-1 BG3 uses 2bpp tiles. Decode the two bitplanes once per tile row,
# then use packed 2-bit pixels in the full-tile loop instead of repeating
# bit extraction for each of the eight pixels. Clipped/mosaic paths stay intact.
ppu = read(ppu_path)
two_bpp_eol = "\\r\\n" if "\\r\\n" in ppu else "\\n"
two_bpp_mark = "/* DC-PATCH: SH-4 2bpp pixel LUT */"
if two_bpp_mark not in ppu:
    def T(text):
        return text.replace("\\n", two_bpp_eol)

    init_end = "  dc_planar_pixel_lut_ready = true;"
    if ppu.count(init_end) != 1:
        sys.exit("ppu.c: 4bpp LUT init completion anchor not found")
    ppu = ppu.replace(init_end, T(
        "  /* DC-PATCH: initialize the two-plane lookup table too. */\n"
        "  for (unsigned plane = 0; plane < 2; plane++) {\n"
        "    for (unsigned value = 0; value < 256; value++) {\n"
        "      uint16_t packed = 0;\n"
        "      for (unsigned pixel = 0; pixel < 8; pixel++)\n"
        "        if (value & (1u << pixel))\n"
        "          packed |= (uint16_t)(1u << (pixel * 2 + plane));\n"
        "      dc_planar_2bpp_lut[plane][value] = packed;\n"
        "    }\n"
        "  }\n" + init_end), 1)

    # Declare the table beside the existing 4bpp LUT.
    lut_anchor = "static uint32_t dc_planar_pixel_lut[4][256];"
    if ppu.count(lut_anchor) != 1:
        sys.exit("ppu.c: 4bpp LUT declaration anchor not found")
    ppu = ppu.replace(lut_anchor, lut_anchor + T(
        "\nstatic uint16_t dc_planar_2bpp_lut[2][256];"), 1)

    # Add the decoder beside dc_decode_4bpp().
    decode_anchor = T(
        "static inline uint32_t dc_decode_4bpp(uint32_t planes) {\n"
        "  return dc_planar_pixel_lut[0][planes & 0xff] |\n"
        "         dc_planar_pixel_lut[1][(planes >> 8) & 0xff] |\n"
        "         dc_planar_pixel_lut[2][(planes >> 16) & 0xff] |\n"
        "         dc_planar_pixel_lut[3][planes >> 24];\n"
        "}")
    if ppu.count(decode_anchor) != 1:
        sys.exit("ppu.c: 4bpp decoder anchor not found")
    ppu = ppu.replace(decode_anchor, decode_anchor + T(
        "\n\nstatic inline uint32_t dc_decode_2bpp(uint32_t planes) {\n"
        "  return dc_planar_2bpp_lut[0][planes & 0xff] |\n"
        "         dc_planar_2bpp_lut[1][(planes >> 8) & 0xff];\n"
        "}"), 1)

    bg_start = ppu.find("static void PpuDrawBackground_2bpp(Ppu *ppu, uint y, bool sub, uint layer, PpuZbufType zhi, PpuZbufType zlo) {")
    bg_end = ppu.find("// Draw a whole line of a 4bpp background layer into bgBuffers, with mosaic applied", bg_start)
    if bg_start < 0 or bg_end < 0:
        sys.exit("ppu.c: 2bpp background function anchors not found")
    bg_fn = ppu[bg_start:bg_end]

    macro_anchor = "#define READ_BITS(ta, tile)"
    fast_macros = T(
        "#define DO_PIXEL_FAST(i) do { \\\\n"
        "  pixel = (dc_pixels >> ((i) * 2)) & 3; \\\\n"
        "  if (pixel && z > dstz[i]) dstz[i] = z + pixel; } while (0)\n"
        "#define DO_PIXEL_FAST_HFLIP(i) do { \\\\n"
        "  pixel = (dc_pixels >> ((7 - (i)) * 2)) & 3; \\\\n"
        "  if (pixel && z > dstz[i]) dstz[i] = z + pixel; } while (0)\n" + macro_anchor)
    if bg_fn.count(macro_anchor) != 1:
        sys.exit("ppu.c: 2bpp READ_BITS macro anchor not found")
    bg_fn = bg_fn.replace(macro_anchor, fast_macros, 1)

    old_middle = T(
        "      uint32 bits = READ_BITS(ta, tile & 0x3ff);\n"
        "      if (bits) {\n"
        "        z += ((tile & 0x1c00) >> kPaletteShift);\n"
        "        if (tile & 0x4000) {\n"
        "          DO_PIXEL(0); DO_PIXEL(1); DO_PIXEL(2); DO_PIXEL(3);\n"
        "          DO_PIXEL(4); DO_PIXEL(5); DO_PIXEL(6); DO_PIXEL(7);\n"
        "        } else {\n"
        "          DO_PIXEL_HFLIP(0); DO_PIXEL_HFLIP(1); DO_PIXEL_HFLIP(2); DO_PIXEL_HFLIP(3);\n"
        "          DO_PIXEL_HFLIP(4); DO_PIXEL_HFLIP(5); DO_PIXEL_HFLIP(6); DO_PIXEL_HFLIP(7);\n"
        "        }\n"
        "      }")
    new_middle = T(
        "      uint32 bits = READ_BITS(ta, tile & 0x3ff);\n"
        "      if (bits) {\n"
        "        uint32_t dc_pixels = dc_decode_2bpp(bits);\n"
        "        z += ((tile & 0x1c00) >> kPaletteShift);\n"
        "        if (tile & 0x4000) {\n"
        "          DO_PIXEL_FAST(0); DO_PIXEL_FAST(1); DO_PIXEL_FAST(2); DO_PIXEL_FAST(3);\n"
        "          DO_PIXEL_FAST(4); DO_PIXEL_FAST(5); DO_PIXEL_FAST(6); DO_PIXEL_FAST(7);\n"
        "        } else {\n"
        "          DO_PIXEL_FAST_HFLIP(0); DO_PIXEL_FAST_HFLIP(1); DO_PIXEL_FAST_HFLIP(2); DO_PIXEL_FAST_HFLIP(3);\n"
        "          DO_PIXEL_FAST_HFLIP(4); DO_PIXEL_FAST_HFLIP(5); DO_PIXEL_FAST_HFLIP(6); DO_PIXEL_FAST_HFLIP(7);\n"
        "        }\n"
        "      }")
    if bg_fn.count(old_middle) != 1:
        sys.exit("ppu.c: 2bpp full-tile loop anchor not found or ambiguous")
    bg_fn = bg_fn.replace(old_middle, new_middle, 1)
    bg_fn = bg_fn.replace("#undef DO_PIXEL_HFLIP", "#undef DO_PIXEL_HFLIP" + T(
        "\n#undef DO_PIXEL_FAST\n#undef DO_PIXEL_FAST_HFLIP"), 1)
    ppu = ppu[:bg_start] + bg_fn + ppu[bg_end:]
    ppu = ppu.replace("/* DC-PATCH: memoized colour math */",
        two_bpp_mark + two_bpp_eol + "/* DC-PATCH: memoized colour math */", 1)
    write(ppu_path, ppu)
    print("patch_smw: enabled SH-4 2bpp background pixel LUT")
else:
    print("patch_smw: 2bpp planar pixel LUT already present")
