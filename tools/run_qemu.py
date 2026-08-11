#!/usr/bin/env python3
"""Run a NorthstarOS image headlessly and archive machine-readable evidence."""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
from typing import Callable, Dict, List, Optional, Sequence


PASS_DEBUG_EXIT = 0x10
DISK_CACHE_MODE = "writeback"


@dataclasses.dataclass
class RunResult:
    passed: bool
    timed_out: bool
    returncode: Optional[int]
    debug_exit: Optional[int]
    elapsed_seconds: float
    serial_log: pathlib.Path
    stderr_log: pathlib.Path
    result_file: pathlib.Path
    missing_patterns: List[str]
    forbidden_matches: List[str]
    error: Optional[str] = None

    def to_json(self) -> Dict[str, object]:
        return {
            "schema": "northstar.qemu-result.v1",
            "passed": self.passed,
            "timed_out": self.timed_out,
            "returncode": self.returncode,
            "debug_exit": self.debug_exit,
            "elapsed_seconds": round(self.elapsed_seconds, 6),
            "serial_log": self.serial_log.name,
            "stderr_log": self.stderr_log.name,
            "missing_patterns": self.missing_patterns,
            "forbidden_matches": self.forbidden_matches,
            "error": self.error,
        }


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def utc_run_id(prefix: str) -> str:
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    safe_prefix = re.sub(r"[^A-Za-z0-9_.-]", "_", prefix).strip("._") or "qemu"
    return "{}-{}-{}".format(safe_prefix, stamp, os.getpid())


def decode_debug_exit(returncode: Optional[int]) -> Optional[int]:
    # isa-debug-exit exits QEMU with (guest_value << 1) | 1.  Negative values
    # represent host signals and even values cannot have come from the device.
    if returncode is None or returncode < 0 or (returncode & 1) == 0:
        return None
    return (returncode - 1) >> 1


def terminate_process_group(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=2.0)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def canonical_qemu_command(
    qemu: str,
    image: pathlib.Path,
    serial_log: pathlib.Path,
    memory_mib: int,
    cpus: int,
    writable: bool,
    extra_args: Sequence[str],
) -> List[str]:
    drive = (
        "file={},format=raw,if=ide,index=0,media=disk,snapshot={},cache={}"
    ).format(
        image.resolve(), "off" if writable else "on", DISK_CACHE_MODE
    )
    return [
        qemu,
        "-machine",
        "pc-i440fx-7.2",
        "-accel",
        "tcg,thread=single",
        "-cpu",
        "qemu64",
        "-m",
        str(memory_mib),
        "-smp",
        str(cpus),
        "-display",
        "none",
        "-monitor",
        "none",
        "-serial",
        "file:{}".format(serial_log.resolve()),
        "-no-reboot",
        "-rtc",
        "base=2000-01-01T00:00:00,clock=vm",
        "-boot",
        "c",
        "-drive",
        drive,
        "-device",
        "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-nic",
        "none",
    ] + list(extra_args)


def recorded_qemu_command(
    command: Sequence[str], image: pathlib.Path, serial_log: pathlib.Path
) -> List[str]:
    """Return a replayable command record without host-specific absolute paths."""

    recorded = list(command)
    recorded[0] = pathlib.Path(recorded[0]).name
    replacements = {
        str(image.resolve()): image.name,
        str(serial_log.resolve()): serial_log.name,
    }
    for index, value in enumerate(recorded):
        for absolute, display in replacements.items():
            value = value.replace(absolute, display)
        recorded[index] = value
    return recorded


def write_json(path: pathlib.Path, value: object) -> None:
    temporary = path.with_name("." + path.name + ".tmp-{}".format(os.getpid()))
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(str(temporary), str(path))


def run_qemu(
    *,
    image: pathlib.Path,
    artifacts_dir: pathlib.Path,
    run_id: str,
    timeout_seconds: float,
    expected_debug_exit: int = PASS_DEBUG_EXIT,
    required_patterns: Sequence[str] = (),
    forbidden_patterns: Sequence[str] = (),
    qemu: str = "qemu-system-x86_64",
    memory_mib: int = 128,
    cpus: int = 1,
    writable: bool = False,
    extra_args: Sequence[str] = (),
    ready_pattern: Optional[str] = None,
    ready_action: Optional[Callable[[], None]] = None,
) -> RunResult:
    if not image.is_file():
        raise ValueError("image does not exist: {}".format(image))
    if timeout_seconds <= 0:
        raise ValueError("timeout must be positive")
    if memory_mib < 16:
        raise ValueError("QEMU memory must be at least 16 MiB")
    if cpus < 1:
        raise ValueError("QEMU CPU count must be positive")
    qemu_path = shutil.which(qemu) if os.path.sep not in qemu else qemu
    if not qemu_path or not pathlib.Path(qemu_path).is_file():
        raise ValueError("QEMU executable not found: {}".format(qemu))

    run_dir = artifacts_dir / run_id
    run_dir.mkdir(parents=True, exist_ok=False)
    serial_log = run_dir / "serial.log"
    stderr_log = run_dir / "qemu.stderr.log"
    result_file = run_dir / "result.json"
    command_file = run_dir / "command.json"
    serial_log.touch()
    command = canonical_qemu_command(
        str(qemu_path), image, serial_log, memory_mib, cpus, writable, extra_args
    )
    invocation = {
        "schema": "northstar.qemu-invocation.v1",
        "run_id": run_id,
        "image": image.name,
        "image_sha256": sha256_file(image),
        "timeout_seconds": timeout_seconds,
        "expected_debug_exit": expected_debug_exit,
        "required_patterns": list(required_patterns),
        "forbidden_patterns": list(forbidden_patterns),
        "writable": writable,
        "command": recorded_qemu_command(command, image, serial_log),
    }
    write_json(command_file, invocation)

    started = time.monotonic()
    timed_out = False
    action_error: Optional[str] = None
    ready_action_ran = False
    returncode: Optional[int] = None
    try:
        with stderr_log.open("wb") as stderr_stream:
            process = subprocess.Popen(
                command,
                stdin=subprocess.DEVNULL,
                stdout=stderr_stream,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            try:
                while True:
                    returncode = process.poll()
                    if returncode is not None:
                        break
                    elapsed = time.monotonic() - started
                    if elapsed >= timeout_seconds:
                        timed_out = True
                        terminate_process_group(process)
                        returncode = process.returncode
                        break
                    if ready_pattern is not None and ready_action is not None and not ready_action_ran:
                        serial_text = serial_log.read_text(encoding="utf-8", errors="replace")
                        if re.search(ready_pattern, serial_text, flags=re.MULTILINE):
                            ready_action_ran = True
                            try:
                                ready_action()
                            except Exception as error:  # test callback failure is evidence, not a traceback.
                                action_error = "ready action failed: {}".format(error)
                                terminate_process_group(process)
                                returncode = process.returncode
                                break
                    time.sleep(0.025)
            finally:
                terminate_process_group(process)
    except OSError as error:
        action_error = "failed to launch QEMU: {}".format(error)

    elapsed_seconds = time.monotonic() - started
    serial_text = serial_log.read_text(encoding="utf-8", errors="replace")
    missing_patterns = [
        pattern for pattern in required_patterns if re.search(pattern, serial_text, flags=re.MULTILINE) is None
    ]
    forbidden_matches = [
        pattern for pattern in forbidden_patterns if re.search(pattern, serial_text, flags=re.MULTILINE) is not None
    ]
    decoded = decode_debug_exit(returncode)
    passed = (
        not timed_out
        and action_error is None
        and decoded == expected_debug_exit
        and not missing_patterns
        and not forbidden_matches
    )
    result = RunResult(
        passed=passed,
        timed_out=timed_out,
        returncode=returncode,
        debug_exit=decoded,
        elapsed_seconds=elapsed_seconds,
        serial_log=serial_log,
        stderr_log=stderr_log,
        result_file=result_file,
        missing_patterns=missing_patterns,
        forbidden_matches=forbidden_matches,
        error=action_error,
    )
    result_payload = result.to_json()
    result_payload.update(
        {
            "run_id": run_id,
            "image_sha256": invocation["image_sha256"],
            "serial_sha256": sha256_file(serial_log),
            "stderr_sha256": sha256_file(stderr_log),
            "ready_action_ran": ready_action_ran,
        }
    )
    write_json(result_file, result_payload)
    return result


def parse_integer(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected integer, got {!r}".format(value)) from error


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=pathlib.Path, required=True)
    parser.add_argument("--artifacts-dir", type=pathlib.Path, default=pathlib.Path("artifacts/qemu"))
    parser.add_argument("--run-id")
    parser.add_argument("--name", default="boot")
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--expected-debug-exit", type=parse_integer, default=PASS_DEBUG_EXIT)
    parser.add_argument("--require", action="append", default=[], help="required serial regex")
    parser.add_argument("--forbid", action="append", default=[], help="forbidden serial regex")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--memory", type=int, default=128, metavar="MIB")
    parser.add_argument("--cpus", type=int, default=1)
    parser.add_argument("--writable", action="store_true")
    parser.add_argument(
        "--qemu-arg",
        action="append",
        default=[],
        help="one extra QEMU argument (use --qemu-arg=-device for leading dashes)",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    run_id = arguments.run_id or utc_run_id(arguments.name)
    try:
        result = run_qemu(
            image=arguments.image,
            artifacts_dir=arguments.artifacts_dir,
            run_id=run_id,
            timeout_seconds=arguments.timeout,
            expected_debug_exit=arguments.expected_debug_exit,
            required_patterns=arguments.require,
            forbidden_patterns=arguments.forbid,
            qemu=arguments.qemu,
            memory_mib=arguments.memory,
            cpus=arguments.cpus,
            writable=arguments.writable,
            extra_args=arguments.qemu_arg,
        )
    except (OSError, ValueError, re.error) as error:
        print("run_qemu: error: {}".format(error), file=sys.stderr)
        return 2
    status = "PASS" if result.passed else "FAIL"
    print(
        "{} {}: debug_exit={} timeout={} serial={}".format(
            status, run_id, result.debug_exit, result.timed_out, result.serial_log
        )
    )
    if result.missing_patterns:
        print("missing serial patterns: {}".format(result.missing_patterns), file=sys.stderr)
    if result.forbidden_matches:
        print("forbidden serial patterns matched: {}".format(result.forbidden_matches), file=sys.stderr)
    if result.error:
        print(result.error, file=sys.stderr)
    return 0 if result.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
