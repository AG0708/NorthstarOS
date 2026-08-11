#!/usr/bin/env python3
"""Drive the interactive NorthstarOS serial console through a real PTY."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import re
import select
import shutil
import signal
import subprocess
import sys
import time
import tty
from typing import Dict, Optional, Sequence


ROOT = pathlib.Path(__file__).resolve().parents[1]


class InteractiveError(RuntimeError):
    pass


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path: pathlib.Path, value: object) -> None:
    temporary = path.with_name("." + path.name + ".tmp-{}".format(os.getpid()))
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(str(temporary), str(path))


def terminate(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=2.0)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()


def qemu_command(qemu: str, image: pathlib.Path) -> list[str]:
    return [
        qemu,
        "-machine",
        "pc-i440fx-7.2",
        "-accel",
        "tcg,thread=single",
        "-cpu",
        "qemu64",
        "-m",
        "256",
        "-smp",
        "1",
        "-display",
        "none",
        "-monitor",
        "none",
        "-serial",
        "pty",
        "-no-reboot",
        "-rtc",
        "base=2000-01-01T00:00:00,clock=vm",
        "-boot",
        "c",
        "-drive",
        "file={},format=raw,if=ide,index=0,media=disk,snapshot=off,cache=writeback".format(
            image.resolve()
        ),
        "-nic",
        "none",
    ]


def recorded_qemu_command(command: Sequence[str], image: pathlib.Path) -> list[str]:
    recorded = list(command)
    recorded[0] = pathlib.Path(recorded[0]).name
    absolute_image = str(image.resolve())
    recorded = [item.replace(absolute_image, image.name) for item in recorded]
    return recorded


def discover_pty(process: subprocess.Popen[bytes], log: bytearray,
                 deadline: float) -> pathlib.Path:
    if process.stdout is None:
        raise InteractiveError("QEMU diagnostic pipe is unavailable")
    pattern = re.compile(rb"char device redirected to (\S+)")
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise InteractiveError(
                "QEMU exited before publishing its serial PTY (rc={})".format(
                    process.returncode
                )
            )
        ready, _, _ = select.select([process.stdout], [], [], 0.1)
        if not ready:
            continue
        line = process.stdout.readline()
        if not line:
            continue
        log.extend(line)
        match = pattern.search(line)
        if match is not None:
            return pathlib.Path(match.group(1).decode("utf-8", errors="strict"))
    raise InteractiveError("QEMU did not publish a serial PTY before timeout")


def read_until(fd: int, transcript: bytearray, pattern: bytes,
               deadline: float, start: int = 0) -> int:
    while time.monotonic() < deadline:
        match = re.search(pattern, bytes(transcript[start:]))
        if match is not None:
            return start + match.end()
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if chunk:
            transcript.extend(chunk)
    raise InteractiveError(
        "serial output did not match {!r}; tail={!r}".format(
            pattern, bytes(transcript[-512:])
        )
    )


def send_line(fd: int, value: str) -> None:
    data = (value + "\n").encode("ascii")
    offset = 0
    while offset < len(data):
        try:
            written = os.write(fd, data[offset:])
        except BlockingIOError:
            select.select([], [fd], [], 0.1)
            continue
        if written <= 0:
            raise InteractiveError("serial PTY accepted no command bytes")
        offset += written


def run_session(arguments: argparse.Namespace,
                session: pathlib.Path) -> Dict[str, object]:
    image = session / "writable.img"
    shutil.copyfile(str(arguments.image), str(image))
    qemu_path = shutil.which(arguments.qemu)
    if qemu_path is None:
        raise InteractiveError("QEMU executable not found: {}".format(arguments.qemu))
    command = qemu_command(qemu_path, image)
    diagnostic = bytearray()
    transcript = bytearray()
    process = subprocess.Popen(
        command,
        cwd=str(ROOT),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    fd: Optional[int] = None
    error: Optional[str] = None
    markers: Dict[str, bool] = {
        "initial_prompt": False,
        "echo": False,
        "persistent_write": False,
        "persistent_read": False,
        "init_restart": False,
        "second_prompt": False,
    }
    started = time.monotonic()
    try:
        deadline = started + arguments.timeout
        pty_path = discover_pty(process, diagnostic, deadline)
        fd = os.open(str(pty_path), os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(fd)

        cursor = read_until(fd, transcript, rb"northstar\$ ", deadline)
        markers["initial_prompt"] = True

        send_line(fd, "echo INTERACTIVE_OK")
        cursor = read_until(fd, transcript, rb"\nINTERACTIVE_OK\r?\n", deadline,
                            cursor)
        cursor = read_until(fd, transcript, rb"northstar\$ ", deadline, cursor)
        markers["echo"] = True

        send_line(fd, "echo PERSIST_INTERACTIVE > /persist/interactive.txt")
        cursor = read_until(fd, transcript, rb"northstar\$ ", deadline, cursor)
        markers["persistent_write"] = True

        send_line(fd, "cat /persist/interactive.txt")
        cursor = read_until(fd, transcript, rb"\nPERSIST_INTERACTIVE\r?\n",
                            deadline, cursor)
        cursor = read_until(fd, transcript, rb"northstar\$ ", deadline, cursor)
        markers["persistent_read"] = True

        send_line(fd, "exit 0")
        cursor = read_until(
            fd, transcript,
            rb"init: shell exited with status 0; restarting\r?\n",
            deadline, cursor,
        )
        markers["init_restart"] = True
        cursor = read_until(fd, transcript, rb"NorthstarOS shell\.", deadline,
                            cursor)
        read_until(fd, transcript, rb"northstar\$ ", deadline, cursor)
        markers["second_prompt"] = True
    except (OSError, UnicodeError, InteractiveError) as failure:
        error = str(failure)
    finally:
        if fd is not None:
            os.close(fd)
        terminate(process)

    (session / "qemu.log").write_bytes(bytes(diagnostic))
    (session / "serial.log").write_bytes(bytes(transcript))
    write_json(session / "command.json", {
        "schema": "northstar.interactive-invocation.v1",
        "command": recorded_qemu_command(command, image),
        "image_sha256": sha256_file(arguments.image),
        "writable_image": image.name,
        "timeout_seconds": arguments.timeout,
    })
    passed = error is None and all(markers.values())
    result = {
        "schema": "northstar.interactive-result.v1",
        "passed": passed,
        "error": error,
        "elapsed_seconds": round(time.monotonic() - started, 6),
        "image_sha256": sha256_file(arguments.image),
        "writable_image_sha256": sha256_file(image),
        "markers": markers,
        "serial_log": "serial.log",
        "serial_sha256": sha256_file(session / "serial.log"),
        "qemu_log": "qemu.log",
        "qemu_returncode_after_controlled_termination": process.returncode,
        "network_enabled": False,
        "socket_ownership": "not exercised; interactive profile disables M5",
    }
    if arguments.canonical_image is not None:
        result["canonical_image_sha256"] = sha256_file(
            arguments.canonical_image
        )
    return result


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=pathlib.Path, required=True)
    parser.add_argument("--canonical-image", type=pathlib.Path)
    parser.add_argument("--artifacts-dir", type=pathlib.Path,
                        default=ROOT / "artifacts" / "interactive")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--session-id")
    arguments = parser.parse_args(argv)
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    return arguments


def main() -> int:
    arguments = parse_args()
    if not arguments.image.is_file():
        print("test_interactive: image does not exist: {}".format(arguments.image),
              file=sys.stderr)
        return 2
    if arguments.canonical_image is not None and not arguments.canonical_image.is_file():
        print(
            "test_interactive: canonical image does not exist: {}".format(
                arguments.canonical_image
            ),
            file=sys.stderr,
        )
        return 2
    if arguments.session_id is None:
        stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        session_id = "interactive-{}-{}".format(stamp, os.getpid())
    else:
        session_id = re.sub(r"[^A-Za-z0-9_.-]", "_", arguments.session_id)
    session = arguments.artifacts_dir / session_id
    session.mkdir(parents=True, exist_ok=False)
    try:
        result = run_session(arguments, session)
    except (OSError, InteractiveError) as error:
        result = {
            "schema": "northstar.interactive-result.v1",
            "passed": False,
            "error": str(error),
            "image_sha256": sha256_file(arguments.image),
        }
        if arguments.canonical_image is not None:
            result["canonical_image_sha256"] = sha256_file(
                arguments.canonical_image
            )
    write_json(session / "summary.json", result)
    print("{} interactive serial profile".format(
        "PASS" if result["passed"] else "FAIL"
    ))
    print("interactive evidence: {}".format(session))
    if not result["passed"]:
        print("test_interactive: {}".format(result.get("error")), file=sys.stderr)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
