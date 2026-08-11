#!/usr/bin/env python3
"""Build and verify NorthstarOS's deterministic raw BIOS disk image.

The image has deliberately fixed boot extents.  This makes the boot contract
auditable and prevents a growing component from silently overwriting another.
All unwritten bytes and all slack inside reserved extents are guaranteed zero.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import tempfile
from typing import Dict, Iterable, List, Optional, Tuple


SECTOR_SIZE = 512
MIB = 1024 * 1024
DEFAULT_IMAGE_MIB = 64

FIXED_EXTENTS = {
    "stage1": (0, 1),
    "stage2": (1, 32),
    "kernel": (128, 4096 - 128),
    "initrd": (4096, 32768 - 4096),
}
FILESYSTEM_LBA = 32768


class ImageError(ValueError):
    pass


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(MIB), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sectors_for(size: int) -> int:
    return (size + SECTOR_SIZE - 1) // SECTOR_SIZE


def round_up(value: int, alignment: int) -> int:
    return ((value + alignment - 1) // alignment) * alignment


def validate_artifact(name: str, path: pathlib.Path, capacity_sectors: Optional[int]) -> Dict[str, object]:
    if not path.is_file():
        raise ImageError("{} artifact does not exist: {}".format(name, path))
    byte_count = path.stat().st_size
    if byte_count <= 0:
        raise ImageError("{} artifact is empty: {}".format(name, path))
    sectors = sectors_for(byte_count)
    if capacity_sectors is not None and sectors > capacity_sectors:
        raise ImageError(
            "{} requires {} sectors but its reserved extent contains {}".format(
                name, sectors, capacity_sectors
            )
        )
    if name == "stage1":
        if byte_count != SECTOR_SIZE:
            raise ImageError("stage1 must be exactly 512 bytes, got {}".format(byte_count))
        with path.open("rb") as stream:
            stream.seek(510)
            if stream.read(2) != b"\x55\xaa":
                raise ImageError("stage1 is missing the BIOS 0x55AA signature")
    return {
        "path": path,
        "bytes": byte_count,
        "sectors": sectors,
        "sha256": sha256_file(path),
    }


def copy_at(output, source_path: pathlib.Path, offset: int) -> None:
    output.seek(offset)
    with source_path.open("rb") as source:
        for chunk in iter(lambda: source.read(MIB), b""):
            output.write(chunk)


def zero_ranges(extents: Iterable[Tuple[int, int]], total_size: int) -> List[Tuple[int, int]]:
    ordered = sorted(extents)
    result: List[Tuple[int, int]] = []
    cursor = 0
    for start, end in ordered:
        if start < cursor or end < start or end > total_size:
            raise ImageError("image extents overlap or exceed image size")
        if start > cursor:
            result.append((cursor, start))
        cursor = end
    if cursor < total_size:
        result.append((cursor, total_size))
    return result


def atomic_json(path: pathlib.Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
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


def atomic_text(path: pathlib.Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix="." + path.name + ".", dir=str(path.parent))
    try:
        with os.fdopen(descriptor, "w", encoding="ascii", newline="\n") as stream:
            stream.write(value)
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


def load_expected_layout(path: Optional[pathlib.Path]) -> Optional[Dict[str, object]]:
    if path is None:
        return None
    try:
        with path.open("r", encoding="utf-8") as stream:
            layout = json.load(stream)
    except (OSError, json.JSONDecodeError) as error:
        raise ImageError("cannot read generated layout {}: {}".format(path, error))
    if layout.get("schema") != "northstar.boot-layout.v1":
        raise ImageError("{} is not a northstar.boot-layout.v1 document".format(path))
    return layout


def cross_check_layout(layout: Optional[Dict[str, object]], artifacts: Dict[str, Dict[str, object]]) -> None:
    if layout is None:
        return
    extents = layout.get("extents", {})
    for name in ("kernel", "initrd", "filesystem"):
        expected = extents.get(name)
        if not isinstance(expected, dict):
            raise ImageError("generated layout is missing the {} extent".format(name))
        actual = artifacts.get(name)
        expected_bytes = int(expected.get("bytes", 0))
        actual_bytes = 0 if actual is None else int(actual["bytes"])
        if expected_bytes != actual_bytes:
            raise ImageError(
                "{} changed after boot metadata generation (expected {} bytes, got {})".format(
                    name, expected_bytes, actual_bytes
                )
            )
        expected_hash = expected.get("sha256")
        actual_hash = None if actual is None else actual["sha256"]
        if expected_hash != actual_hash:
            raise ImageError("{} changed after boot metadata generation (SHA-256 mismatch)".format(name))


def build_image(arguments: argparse.Namespace) -> Dict[str, object]:
    inputs: Dict[str, pathlib.Path] = {
        "stage1": arguments.stage1,
        "stage2": arguments.stage2,
        "kernel": arguments.kernel,
    }
    if arguments.initrd is not None:
        inputs["initrd"] = arguments.initrd
    if arguments.filesystem is not None:
        inputs["filesystem"] = arguments.filesystem

    artifacts: Dict[str, Dict[str, object]] = {}
    for name, path in inputs.items():
        capacity = FIXED_EXTENTS[name][1] if name in FIXED_EXTENTS else None
        artifacts[name] = validate_artifact(name, path, capacity)
    cross_check_layout(load_expected_layout(arguments.layout), artifacts)

    minimum_size = DEFAULT_IMAGE_MIB * MIB
    requested_size = arguments.image_size_mib * MIB
    fs_end = FILESYSTEM_LBA * SECTOR_SIZE
    if "filesystem" in artifacts:
        fs_end += round_up(int(artifacts["filesystem"]["bytes"]), SECTOR_SIZE)
    image_size = round_up(max(minimum_size, requested_size, fs_end), MIB)

    placed: List[Tuple[str, int, int]] = []
    for name in ("stage1", "stage2", "kernel", "initrd"):
        if name not in artifacts:
            continue
        start = FIXED_EXTENTS[name][0] * SECTOR_SIZE
        end = start + int(artifacts[name]["bytes"])
        placed.append((name, start, end))
    if "filesystem" in artifacts:
        start = FILESYSTEM_LBA * SECTOR_SIZE
        placed.append(("filesystem", start, start + int(artifacts["filesystem"]["bytes"])))

    # Validate every occupied range before touching the destination.
    zero_ranges(((start, end) for _, start, end in placed), image_size)

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix="." + arguments.output.name + ".", dir=str(arguments.output.parent)
    )
    try:
        with os.fdopen(descriptor, "w+b", buffering=0) as output:
            output.truncate(image_size)  # POSIX guarantees the extension reads as zero.
            for name, start, _ in placed:
                copy_at(output, artifacts[name]["path"], start)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary_name, 0o644)
        os.replace(temporary_name, str(arguments.output))
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise

    manifest_artifacts = []
    for name in ("stage1", "stage2", "kernel", "initrd", "filesystem"):
        artifact = artifacts.get(name)
        if name == "filesystem":
            lba, capacity = FILESYSTEM_LBA, (image_size // SECTOR_SIZE) - FILESYSTEM_LBA
        else:
            lba, capacity = FIXED_EXTENTS[name]
        byte_count = 0 if artifact is None else int(artifact["bytes"])
        meaningful_sectors = 0 if artifact is None else int(artifact["sectors"])
        manifest_artifacts.append(
            {
                "name": name,
                "present": artifact is not None,
                "source": None if artifact is None else artifact["path"].name,
                "lba": lba,
                "offset": lba * SECTOR_SIZE,
                "bytes": byte_count,
                "sectors": meaningful_sectors,
                "capacity_sectors": capacity,
                "zero_padding_bytes": capacity * SECTOR_SIZE - byte_count,
                "sha256": None if artifact is None else artifact["sha256"],
            }
        )

    image_hash = sha256_file(arguments.output)
    source_epoch_text = os.environ.get("SOURCE_DATE_EPOCH", "0")
    try:
        source_epoch = int(source_epoch_text)
    except ValueError:
        raise ImageError("SOURCE_DATE_EPOCH must be an integer")
    manifest = {
        "schema": "northstar.raw-image.v1",
        "sector_size": SECTOR_SIZE,
        "source_date_epoch": source_epoch,
        "image": {
            "name": arguments.output.name,
            "bytes": image_size,
            "sectors": image_size // SECTOR_SIZE,
            "sha256": image_hash,
        },
        "artifacts": manifest_artifacts,
    }
    atomic_json(arguments.manifest, manifest)
    atomic_text(arguments.output.with_suffix(arguments.output.suffix + ".sha256"), "{}  {}\n".format(image_hash, arguments.output.name))
    return manifest


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage1", type=pathlib.Path, required=True)
    parser.add_argument("--stage2", type=pathlib.Path, required=True)
    parser.add_argument("--kernel", type=pathlib.Path, required=True)
    parser.add_argument("--initrd", type=pathlib.Path)
    parser.add_argument("--filesystem", type=pathlib.Path)
    parser.add_argument("--layout", type=pathlib.Path, help="generated boot_layout.json to cross-check")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--image-size-mib", type=int, default=DEFAULT_IMAGE_MIB)
    arguments = parser.parse_args()
    if arguments.image_size_mib <= 0:
        parser.error("--image-size-mib must be positive")
    return arguments


def main() -> int:
    arguments = parse_args()
    try:
        manifest = build_image(arguments)
    except (OSError, ImageError) as error:
        print("build_image: error: {}".format(error), file=__import__("sys").stderr)
        return 2
    print("{}  {}".format(manifest["image"]["sha256"], arguments.output))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
