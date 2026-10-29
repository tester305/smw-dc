#!/bin/sh
# 1) fetch the snesrev/smw C reimplementation at a pinned commit
# 2) patch it for the SH-4 (unaligned-access safety)
# 3) extract the game assets from YOUR ROM dump (smw.sfc, USA, 512 KB, no header)
# 4) put smw_assets.dat in the romdisk and build smw_dc.elf if a KallistiOS environment is loaded
set -e
cd "$(dirname "$0")"
ROM="${1:-smw.sfc}"
SMW_COMMIT=eae20c65c58930c8b62c76188d259579ad4130f1

[ -f "$ROM" ] || { echo "ROM not found: $ROM"; echo "usage: ./build.sh path/to/smw.sfc"; exit 1; }
command -v python3 >/dev/null || { echo "python3 is required"; exit 1; }

if [ ! -d smw/.git ]; then
    rm -rf smw
    git init -q smw
    git -C smw remote add origin https://github.com/snesrev/smw.git
fi
git -C smw fetch -q --depth 1 origin "$SMW_COMMIT"
git -C smw checkout -q -f "$SMW_COMMIT"
git -C smw clean -fdq -e smw_assets.dat

python3 dc/patch_smw.py smw

cp "$ROM" smw/smw.sfc
rm -f smw/smw_assets.dat
( cd smw && python3 assets/restool.py )
[ -s smw/smw_assets.dat ] || { echo "asset extraction failed (is this the USA smw.sfc, unheadered?)"; exit 1; }
rm -f smw/smw.sfc

mkdir -p romdisk
cp smw/smw_assets.dat romdisk/smw_assets.dat
echo "romdisk/smw_assets.dat: $(wc -c < romdisk/smw_assets.dat) bytes"

if [ -n "$KOS_BASE" ]; then
    make clean
    make
    echo "Built smw_dc.elf"
else
    echo "KOS_BASE is not set - source your KallistiOS environ.sh, then run:  make"
fi
