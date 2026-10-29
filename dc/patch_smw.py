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
    if "PPU_objInterlace(ppu)" in sprite_fn or "PPU_objSize(ppu)" in sprite_fn:
        sys.exit("ppu.c: not all sprite register reads were hoisted - update patch_smw.py")
    ppu = ppu[:sprite_start] + sprite_fn + ppu[sprite_end:]
    write(ppu_path, ppu)
    print("patch_smw: hoisted sprite-size/interlace reads")
else:
    print("patch_smw: sprite-register hoisting already present")
