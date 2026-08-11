#!/usr/bin/env python3
"""Generate the boot-time image layout consumed by NASM and C code.

The boot chain intentionally uses fixed, non-overlapping slots.  Only the
number of meaningful sectors in the kernel/initrd varies between builds; all
unused bytes in a slot are zero-filled by ``build_image.py``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import tempfile
from typing import Dict, Optional


SECTOR_SIZE = 512
STAGE1_LBA = 0
STAGE2_LBA = 1
STAGE2_SECTORS = 32
KERNEL_LBA = 128
INITRD_LBA = 4096
FILESYSTEM_LBA = 32768
KERNEL_LOAD_ADDR = 0x00200000
INITRD_LOAD_ADDR = 0x00400000
KERNEL_VIRT_ADDR = 0xFFFFFFFF80000000
KERNEL_ENTRY = KERNEL_VIRT_ADDR


def sectors_for(size: int) -> int:
    return (size + SECTOR_SIZE - 1) // SECTOR_SIZE


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def optional_artifact(path: Optional[pathlib.Path], name: str) -> Dict[str, object]:
    if path is None:
        return {"name": name, "present": False, "bytes": 0, "sectors": 0, "sha256": None}
    if not path.is_file():
        raise ValueError("{} artifact does not exist: {}".format(name, path))
    size = path.stat().st_size
    return {
        "name": name,
        "present": True,
        "bytes": size,
        "sectors": sectors_for(size),
        "sha256": sha256_file(path),
    }


def build_layout(
    kernel: pathlib.Path,
    initrd: Optional[pathlib.Path],
    filesystem: Optional[pathlib.Path],
    kernel_entry: int,
    kernel_load_addr: int,
    initrd_load_addr: int,
    kernel_virt_addr: int,
    kernel_memory_bytes: Optional[int],
) -> Dict[str, object]:
    if not kernel.is_file():
        raise ValueError("kernel artifact does not exist: {}".format(kernel))
    kernel_size = kernel.stat().st_size
    if kernel_size <= 0:
        raise ValueError("kernel artifact is empty")
    kernel_sectors = sectors_for(kernel_size)
    kernel_capacity = INITRD_LBA - KERNEL_LBA
    if kernel_sectors > kernel_capacity:
        raise ValueError(
            "kernel needs {} sectors, exceeding its {}-sector slot".format(
                kernel_sectors, kernel_capacity
            )
        )

    initrd_info = optional_artifact(initrd, "initrd")
    initrd_capacity = FILESYSTEM_LBA - INITRD_LBA
    if int(initrd_info["sectors"]) > initrd_capacity:
        raise ValueError(
            "initrd needs {} sectors, exceeding its {}-sector slot".format(
                initrd_info["sectors"], initrd_capacity
            )
        )
    fs_info = optional_artifact(filesystem, "filesystem")

    if kernel_memory_bytes is None:
        kernel_memory_bytes = kernel_size
    if kernel_memory_bytes < kernel_size:
        raise ValueError("kernel memory extent cannot be smaller than its file extent")
    if initrd_load_addr & 0xFFF:
        raise ValueError("initrd load address must be page aligned")
    if int(initrd_info["bytes"]) != 0:
        kernel_end = kernel_load_addr + kernel_memory_bytes
        initrd_end = initrd_load_addr + int(initrd_info["bytes"])
        if kernel_end > initrd_load_addr or initrd_end > 0x40000000:
            raise ValueError("kernel/initrd physical extents overlap or exceed the bootstrap direct map")

    return {
        "schema": "northstar.boot-layout.v1",
        "sector_size": SECTOR_SIZE,
        "addresses": {
            "kernel_load": kernel_load_addr,
            "initrd_load": initrd_load_addr,
            "kernel_virtual": kernel_virt_addr,
            "kernel_entry": kernel_entry,
        },
        "extents": {
            "stage1": {"lba": STAGE1_LBA, "sectors": 1, "capacity_sectors": 1},
            "stage2": {
                "lba": STAGE2_LBA,
                "sectors": STAGE2_SECTORS,
                "capacity_sectors": STAGE2_SECTORS,
            },
            "kernel": {
                "lba": KERNEL_LBA,
                "bytes": kernel_size,
                "memory_bytes": kernel_memory_bytes,
                "sectors": kernel_sectors,
                "capacity_sectors": kernel_capacity,
                "sha256": sha256_file(kernel),
            },
            "initrd": dict(initrd_info, lba=INITRD_LBA, capacity_sectors=initrd_capacity),
            "filesystem": dict(fs_info, lba=FILESYSTEM_LBA),
        },
    }


def atomic_write(path: pathlib.Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix="." + path.name + ".", dir=str(path.parent))
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary_name, 0o644)
        os.replace(temporary_name, str(path))
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def nasm_text(layout: Dict[str, object]) -> str:
    extents = layout["extents"]
    addresses = layout["addresses"]
    kernel = extents["kernel"]
    initrd = extents["initrd"]
    filesystem = extents["filesystem"]
    values = [
        ("BOOT_SECTOR_SIZE", layout["sector_size"]),
        ("STAGE2_LBA", extents["stage2"]["lba"]),
        ("STAGE2_SECTORS", extents["stage2"]["sectors"]),
        ("KERNEL_LBA", kernel["lba"]),
        ("KERNEL_SECTORS", kernel["sectors"]),
        ("KERNEL_BYTES", kernel["bytes"]),
        ("KERNEL_MEMORY_BYTES", kernel["memory_bytes"]),
        ("KERNEL_LOAD_ADDR", addresses["kernel_load"]),
        ("KERNEL_VIRT_ADDR", addresses["kernel_virtual"]),
        ("KERNEL_ENTRY", addresses["kernel_entry"]),
        ("INITRD_LBA", initrd["lba"]),
        ("INITRD_SECTORS", initrd["sectors"]),
        ("INITRD_BYTES", initrd["bytes"]),
        ("INITRD_LOAD_ADDR", addresses["initrd_load"]),
        ("FS_LBA", filesystem["lba"]),
        ("FS_SECTORS", filesystem["sectors"]),
    ]
    lines = ["; Generated by tools/gen_image_layout.py. Do not edit."]
    for name, value in values:
        if name.endswith("ADDR") or name in ("KERNEL_ENTRY", "KERNEL_VIRT_ADDR"):
            rendered = "0x{:x}".format(int(value))
        else:
            rendered = str(int(value))
        lines.append("%define {:<24} {}".format(name, rendered))
    return "\n".join(lines) + "\n"


def c_header_text(layout: Dict[str, object]) -> str:
    extents = layout["extents"]
    addresses = layout["addresses"]
    kernel = extents["kernel"]
    initrd = extents["initrd"]
    filesystem = extents["filesystem"]
    values = [
        ("NORTHSTAR_BOOT_SECTOR_SIZE", layout["sector_size"]),
        ("NORTHSTAR_STAGE2_LBA", extents["stage2"]["lba"]),
        ("NORTHSTAR_STAGE2_SECTORS", extents["stage2"]["sectors"]),
        ("NORTHSTAR_KERNEL_LBA", kernel["lba"]),
        ("NORTHSTAR_KERNEL_SECTORS", kernel["sectors"]),
        ("NORTHSTAR_KERNEL_BYTES", kernel["bytes"]),
        ("NORTHSTAR_KERNEL_MEMORY_BYTES", kernel["memory_bytes"]),
        ("NORTHSTAR_KERNEL_LOAD_ADDR", addresses["kernel_load"]),
        ("NORTHSTAR_KERNEL_VIRT_ADDR", addresses["kernel_virtual"]),
        ("NORTHSTAR_KERNEL_ENTRY", addresses["kernel_entry"]),
        ("NORTHSTAR_INITRD_LBA", initrd["lba"]),
        ("NORTHSTAR_INITRD_SECTORS", initrd["sectors"]),
        ("NORTHSTAR_INITRD_BYTES", initrd["bytes"]),
        ("NORTHSTAR_INITRD_LOAD_ADDR", addresses["initrd_load"]),
        ("NORTHSTAR_FS_LBA", filesystem["lba"]),
        ("NORTHSTAR_FS_SECTORS", filesystem["sectors"]),
    ]
    lines = [
        "/* Generated by tools/gen_image_layout.py. Do not edit. */",
        "#ifndef NORTHSTAR_GENERATED_BOOT_LAYOUT_H",
        "#define NORTHSTAR_GENERATED_BOOT_LAYOUT_H",
        "",
        "#include <stdint.h>",
        "",
    ]
    for name, value in values:
        lines.append("#define {:<38} UINT64_C(0x{:x})".format(name, int(value)))
    lines.extend(["", "#endif", ""])
    return "\n".join(lines)


def parse_int(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected an integer, got {!r}".format(value)) from error


def elf_layout(path: pathlib.Path, nm: str) -> Dict[str, int]:
    if not path.is_file():
        raise ValueError("kernel ELF does not exist: {}".format(path))
    try:
        completed = subprocess.run(
            [nm, "-P", "--defined-only", str(path)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=15,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ValueError("cannot inspect kernel ELF with {}: {}".format(nm, error))
    if completed.returncode != 0:
        diagnostic = completed.stderr.decode("utf-8", errors="replace").strip()
        raise ValueError("{} could not inspect kernel ELF: {}".format(nm, diagnostic))
    symbols: Dict[str, int] = {}
    for raw_line in completed.stdout.decode("utf-8", errors="replace").splitlines():
        fields = raw_line.split()
        if len(fields) >= 3:
            try:
                symbols[fields[0]] = int(fields[2], 16)
            except ValueError:
                continue
    required = ("_start", "__kernel_start", "__kernel_phys_start", "__kernel_phys_end")
    missing = [name for name in required if name not in symbols]
    if missing:
        raise ValueError("kernel ELF is missing required symbols: {}".format(missing))
    if symbols["__kernel_phys_end"] <= symbols["__kernel_phys_start"]:
        raise ValueError("kernel ELF physical extent is empty or reversed")
    return {
        "kernel_entry": symbols["_start"],
        "kernel_virtual": symbols["__kernel_start"],
        "kernel_load": symbols["__kernel_phys_start"],
        "kernel_memory_bytes": symbols["__kernel_phys_end"] - symbols["__kernel_phys_start"],
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=pathlib.Path, required=True)
    parser.add_argument("--kernel-elf", type=pathlib.Path, help="derive addresses and BSS extent from linker symbols")
    parser.add_argument("--nm", default="x86_64-elf-nm", help="nm executable used with --kernel-elf")
    parser.add_argument("--initrd", type=pathlib.Path)
    parser.add_argument("--filesystem", type=pathlib.Path)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--kernel-entry", type=parse_int, default=KERNEL_ENTRY)
    parser.add_argument("--kernel-load-addr", type=parse_int, default=KERNEL_LOAD_ADDR)
    parser.add_argument("--initrd-load-addr", type=parse_int, default=INITRD_LOAD_ADDR)
    parser.add_argument("--kernel-virt-addr", type=parse_int, default=KERNEL_VIRT_ADDR)
    parser.add_argument(
        "--kernel-memory-bytes",
        type=parse_int,
        help="in-memory kernel extent including BSS (defaults to kernel file size)",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    try:
        if arguments.kernel_elf is not None:
            derived = elf_layout(arguments.kernel_elf, arguments.nm)
            arguments.kernel_entry = derived["kernel_entry"]
            arguments.kernel_load_addr = derived["kernel_load"]
            arguments.kernel_virt_addr = derived["kernel_virtual"]
            arguments.kernel_memory_bytes = derived["kernel_memory_bytes"]
        layout = build_layout(
            arguments.kernel,
            arguments.initrd,
            arguments.filesystem,
            arguments.kernel_entry,
            arguments.kernel_load_addr,
            arguments.initrd_load_addr,
            arguments.kernel_virt_addr,
            arguments.kernel_memory_bytes,
        )
        output_dir = arguments.output_dir
        canonical_json = (json.dumps(layout, indent=2, sort_keys=True) + "\n").encode("utf-8")
        atomic_write(output_dir / "boot_layout.json", canonical_json)
        atomic_write(output_dir / "boot_layout.inc", nasm_text(layout).encode("ascii"))
        atomic_write(output_dir / "boot_layout.h", c_header_text(layout).encode("ascii"))
    except (OSError, ValueError) as error:
        print("gen_image_layout: error: {}".format(error), file=__import__("sys").stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
