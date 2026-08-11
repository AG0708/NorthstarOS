#!/usr/bin/env python3
"""Build a deterministic SVR4 newc initramfs containing user ELF programs."""

from __future__ import annotations

import argparse
import os
import pathlib
import stat
from dataclasses import dataclass
from typing import Iterable


@dataclass(frozen=True)
class Member:
    name: str
    mode: int
    data: bytes


def pad4(output: bytearray) -> None:
    output.extend(b"\0" * ((-len(output)) & 3))


def append_member(output: bytearray, member: Member, inode: int, mtime: int) -> None:
    encoded_name = member.name.encode("utf-8") + b"\0"
    fields = (
        inode,
        member.mode,
        0,
        0,
        2 if stat.S_ISDIR(member.mode) else 1,
        mtime,
        len(member.data),
        0,
        0,
        0,
        0,
        len(encoded_name),
        0,
    )
    header = "070701" + "".join(f"{field & 0xFFFFFFFF:08x}" for field in fields)
    if len(header) != 110:
        raise AssertionError("newc header width changed")
    output.extend(header.encode("ascii"))
    output.extend(encoded_name)
    pad4(output)
    output.extend(member.data)
    pad4(output)


def parent_directories(names: Iterable[str]) -> set[str]:
    result: set[str] = set()
    for name in names:
        path = pathlib.PurePosixPath(name)
        for parent in path.parents:
            text = str(parent)
            if text not in (".", ""):
                result.add(text)
    return result


def parse_file(value: str) -> tuple[str, pathlib.Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("--file requires GUEST=HOST")
    guest, host = value.split("=", 1)
    guest = guest.strip("/")
    if not guest or guest == "." or ".." in pathlib.PurePosixPath(guest).parts:
        raise argparse.ArgumentTypeError(f"unsafe guest path: {guest!r}")
    return guest, pathlib.Path(host)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--file", action="append", type=parse_file, default=[])
    arguments = parser.parse_args()
    if not arguments.file:
        parser.error("at least one --file is required")

    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    if not 0 <= epoch <= 0xFFFFFFFF:
        parser.error("SOURCE_DATE_EPOCH must fit in an unsigned 32-bit field")
    files: dict[str, pathlib.Path] = {}
    for guest, host in arguments.file:
        if guest in files:
            parser.error(f"duplicate archive member: {guest}")
        if not host.is_file():
            parser.error(f"input is not a regular file: {host}")
        files[guest] = host

    members = [Member(".", stat.S_IFDIR | 0o755, b"")]
    members.extend(
        Member(name, stat.S_IFDIR | 0o755, b"")
        for name in sorted(parent_directories(files))
    )
    members.extend(
        Member(name, stat.S_IFREG | 0o755, files[name].read_bytes())
        for name in sorted(files)
    )
    output = bytearray()
    for inode, member in enumerate(members, start=1):
        append_member(output, member, inode, epoch)
    append_member(output, Member("TRAILER!!!", 0, b""), len(members) + 1, epoch)
    output.extend(b"\0" * ((512 - (len(output) % 512)) % 512))

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = arguments.output.with_suffix(arguments.output.suffix + ".tmp")
    temporary.write_bytes(output)
    os.replace(temporary, arguments.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
