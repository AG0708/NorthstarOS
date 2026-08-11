#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "/tmp/northstar-fs-tools.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

cc=cc
cflags="-std=c11 -Wall -Wextra -Werror -pedantic -I$repo/include"

$cc $cflags "$repo/tools/nsfs_host.c" "$repo/tools/mkfs_northstar.c" \
    -o "$work/mkfs.northstar"
$cc $cflags "$repo/tools/nsfs_host.c" "$repo/tools/fsck_northstar.c" \
    -o "$work/fsck.northstar"
$cc $cflags "$repo/tools/nsfs_host.c" "$repo/tools/nsfs_inspect.c" \
    -o "$work/nsfs_inspect"
$cc $cflags \
    "$repo/kernel/fs/block.c" \
    "$repo/kernel/fs/block_mem.c" \
    "$repo/kernel/fs/nsfs.c" \
    "$repo/tests/host/test_fs_nsfs_compat.c" \
    -o "$work/test_nsfs_compat"

mkdir -p "$work/source/nested/deeper"
printf 'NorthstarFS host-tool oracle\n' > "$work/source/readme.txt"
printf 'empty parent\n' > "$work/source/nested/deeper/tiny"
dd if=/dev/zero of="$work/source/nested/indirect.bin" bs=4096 count=15 \
    2>/dev/null
printf 'indirect-boundary-tail' |
    dd of="$work/source/nested/indirect.bin" bs=1 seek=61417 conv=notrunc \
       2>/dev/null
ln -s ../readme.txt "$work/source/nested/readme-link"
: > "$work/source/empty"

SOURCE_DATE_EPOCH=1700000000 "$work/mkfs.northstar" --quiet --size 4M \
    --source "$work/source" "$work/one.img"
SOURCE_DATE_EPOCH=1700000000 "$work/mkfs.northstar" --quiet --size 4M \
    --source "$work/source" "$work/two.img"
cmp "$work/one.img" "$work/two.img"

# Cross-check the two independent implementations in both directions: mount a
# host-formatted image with the kernel driver, mutate it across the indirect
# block boundary, then inspect and validate the resulting raw image with host
# code that does not call the kernel filesystem implementation.
"$work/test_nsfs_compat" "$work/one.img" "$work/kernel-mutated.img"
"$work/fsck.northstar" --verbose "$work/kernel-mutated.img"
"$work/nsfs_inspect" "$work/kernel-mutated.img" cat /kernel-dir/nested \
    > "$work/kernel-nested.readback"
printf 'cross-compatible' > "$work/kernel-nested.expected"
cmp "$work/kernel-nested.expected" "$work/kernel-nested.readback"
"$work/nsfs_inspect" --json "$work/kernel-mutated.img" \
    stat /kernel-written > "$work/kernel-written.json"
python3 - "$work/kernel-written.json" <<'PY'
import json, sys
stat = json.load(open(sys.argv[1], encoding="utf-8"))
assert stat["size"] == (12 + 3) * 4096 + 137, stat
assert stat["allocated_blocks"] == 17, stat
PY

"$work/fsck.northstar" --verbose "$work/one.img"
"$work/fsck.northstar" --json "$work/one.img" > "$work/fsck.json"
python3 - "$work/fsck.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as stream:
    result = json.load(stream)
assert result["valid"] is True, result
assert result["errors"] == 0, result
assert result["journal"]["state"] == "empty", result
assert result["allocated_inodes"] >= 8, result
PY

"$work/nsfs_inspect" "$work/one.img" cat /readme.txt > "$work/readback"
cmp "$work/source/readme.txt" "$work/readback"
"$work/nsfs_inspect" "$work/one.img" cat /nested/indirect.bin \
    > "$work/indirect.readback"
cmp "$work/source/nested/indirect.bin" "$work/indirect.readback"
"$work/nsfs_inspect" "$work/one.img" cat /nested/readme-link \
    > "$work/link.readback"
printf '../readme.txt' > "$work/link.expected"
cmp "$work/link.expected" "$work/link.readback"
"$work/nsfs_inspect" --json "$work/one.img" stat /nested/indirect.bin \
    > "$work/stat.json"
"$work/nsfs_inspect" --json "$work/one.img" ls /nested > "$work/ls.json"
python3 - "$work/stat.json" "$work/ls.json" <<'PY'
import json, sys
stat = json.load(open(sys.argv[1], encoding="utf-8"))
listing = json.load(open(sys.argv[2], encoding="utf-8"))
assert stat["size"] == 15 * 4096
assert stat["allocated_blocks"] == 16
assert {entry["name"] for entry in listing["entries"]} == {
    "deeper", "indirect.bin", "readme-link"
}
PY

python3 - "$work/container.img" <<'PY'
import sys
path = sys.argv[1]
data = bytearray(6 * 1024 * 1024)
data[:16] = b"PREFIX-PRESERVED"
data[-16:] = b"SUFFIX-PRESERVED"
open(path, "wb").write(data)
PY
"$work/mkfs.northstar" --quiet --offset 1M --size 4M \
    "$work/container.img"
"$work/fsck.northstar" --offset 1M --size 4M "$work/container.img"
"$work/nsfs_inspect" --offset 1M --size 4M "$work/container.img" stat /
python3 - "$work/container.img" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
assert data[:16] == b"PREFIX-PRESERVED"
assert data[-16:] == b"SUFFIX-PRESERVED"
PY

if "$work/mkfs.northstar" --quiet --size 4M "$work/one.img" \
        >"$work/refuse.out" 2>"$work/refuse.err"; then
    echo "mkfs unexpectedly overwrote a nonzero image" >&2
    exit 1
fi

cp "$work/one.img" "$work/bad-inode.img"
python3 - "$work/bad-inode.img" <<'PY'
import struct, sys
path = sys.argv[1]
with open(path, "r+b") as image:
    superblock = image.read(256)
    inode_table = struct.unpack_from("<Q", superblock, 104)[0]
    image.seek(inode_table * 4096 + 128 + 120)
    byte = image.read(1)
    image.seek(-1, 1)
    image.write(bytes([byte[0] ^ 0x80]))
PY
if "$work/fsck.northstar" "$work/bad-inode.img" >/dev/null 2>&1; then
    echo "fsck missed inode corruption" >&2
    exit 1
fi
echo "ok - independent fsck rejects inode corruption"

cp "$work/one.img" "$work/bad-bitmap.img"
python3 - "$work/bad-bitmap.img" <<'PY'
import struct, sys
path = sys.argv[1]
with open(path, "r+b") as image:
    superblock = image.read(256)
    inode_table = struct.unpack_from("<Q", superblock, 104)[0]
    block_bitmap = struct.unpack_from("<Q", superblock, 88)[0]
    image.seek(inode_table * 4096 + 128 + 64)
    root_block = struct.unpack("<I", image.read(4))[0]
    image.seek(block_bitmap * 4096 + root_block // 8)
    byte = image.read(1)[0]
    image.seek(-1, 1)
    image.write(bytes([byte & ~(1 << (root_block % 8))]))
PY
if "$work/fsck.northstar" "$work/bad-bitmap.img" >/dev/null 2>&1; then
    echo "fsck missed bitmap/reference corruption" >&2
    exit 1
fi
echo "ok - independent fsck rejects bitmap-reference corruption"

cp "$work/one.img" "$work/bad-super.img"
python3 - "$work/bad-super.img" <<'PY'
import sys
path = sys.argv[1]
with open(path, "r+b") as image:
    for base in (0, 4096):
        image.seek(base + 184)
        byte = image.read(1)[0]
        image.seek(-1, 1)
        image.write(bytes([byte ^ 1]))
PY
if "$work/fsck.northstar" "$work/bad-super.img" >/dev/null 2>&1; then
    echo "fsck missed dual-superblock corruption" >&2
    exit 1
fi
echo "ok - independent fsck rejects dual-superblock corruption"

echo "NorthstarFS host tools: PASS"
