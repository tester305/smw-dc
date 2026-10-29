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
