#!/usr/bin/env python3
"""Hard-cut NorthstarFS at each durable journal boundary and verify recovery."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import re
import shutil
import signal
import struct
import subprocess
import sys
import time
from typing import Dict, Sequence


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests" / "host"))

from run_integration import run_filesystem_probe, write_json  # noqa: E402
from run_qemu import (  # noqa: E402
    canonical_qemu_command,
    recorded_qemu_command,
    run_qemu,
    sha256_file,
)
from test_support import FilesystemProbe  # noqa: E402


CONTROL_MAGIC = b"NSCUT01\0"
CONTROL_BYTES = 512
FS_OFFSET = 16 * 1024 * 1024
TAIL_RESERVE = 4 * 1024 * 1024
PAYLOAD_BYTES = 61577
PAYLOAD_SHA256 = "29fe680bb728551e867fff0011dcfcdc098cf55bc74dbf53e42887ca6a43306e"

CUTPOINTS = (
    (1, "redo-durable", "absent"),
    (2, "commit-durable", "present"),
    (3, "home-durable", "present"),
    (4, "journal-cleared", "present"),
)


class MatrixError(RuntimeError):
    pass


def write_control(image: pathlib.Path, checkpoint: int) -> None:
    if checkpoint < 0 or checkpoint > len(CUTPOINTS):
        raise ValueError("checkpoint is outside the control ABI")
    if checkpoint == 0:
        data = bytes(CONTROL_BYTES)
    else:
        data = struct.pack(
            "<8sII496s",
            CONTROL_MAGIC,
            checkpoint,
            (~checkpoint) & 0xFFFFFFFF,
            b"",
        )
    with image.open("r+b") as stream:
        stream.seek(-CONTROL_BYTES, os.SEEK_END)
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def hard_cut_at_marker(
    *,
    qemu: str,
    image: pathlib.Path,
    case_root: pathlib.Path,
    marker: str,
    timeout_seconds: float,
) -> Dict[str, object]:
    run_root = case_root / "power-cut"
    run_root.mkdir(parents=True, exist_ok=False)
    serial_log = run_root / "serial.log"
    stderr_log = run_root / "qemu.stderr.log"
    serial_log.touch()
    qemu_path = shutil.which(qemu) if os.path.sep not in qemu else qemu
    if qemu_path is None:
        raise MatrixError("QEMU executable not found: {}".format(qemu))
    command = canonical_qemu_command(
        str(qemu_path), image, serial_log, 256, 1, True, ()
    )
    write_json(
        run_root / "command.json",
        {
            "schema": "northstar.journal-cut-command.v1",
            "command": recorded_qemu_command(command, image, serial_log),
            "marker": marker,
            "image_sha256_before_boot": sha256_file(image),
        },
    )
    started = time.monotonic()
    with stderr_log.open("wb") as stderr:
        process = subprocess.Popen(
            command,
            cwd=str(ROOT),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=stderr,
            start_new_session=True,
        )
        deadline = started + timeout_seconds
        observed = False
        while time.monotonic() < deadline:
            transcript = serial_log.read_text(encoding="utf-8", errors="replace")
            if marker in transcript:
                observed = True
                break
            if process.poll() is not None:
                break
            time.sleep(0.01)
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
        returncode = process.wait()
    transcript = serial_log.read_text(encoding="utf-8", errors="replace")
    marker_count = transcript.count(marker)
    passed = (
        observed
        and marker_count == 1
        and returncode == -signal.SIGKILL
        and "NS:RUN:COMPLETE" not in transcript[transcript.find(marker) :]
    )
    result = {
        "schema": "northstar.journal-cut-result.v1",
        "passed": passed,
        "marker": marker,
        "marker_count": marker_count,
        "hard_kill_signal": signal.SIGKILL,
        "returncode": returncode,
        "elapsed_seconds": round(time.monotonic() - started, 6),
        "serial_log": str(serial_log.relative_to(case_root)),
        "stderr_log": str(stderr_log.relative_to(case_root)),
        "serial_sha256": sha256_file(serial_log),
        "image_sha256_after_cut": sha256_file(image),
    }
    write_json(run_root / "result.json", result)
    return result


def inspect_mutation(
    *,
    case_root: pathlib.Path,
    image: pathlib.Path,
    expected: str,
    region_bytes: int,
) -> Dict[str, object]:
    inspector = case_root / "host-filesystem-oracle" / "nsfs_inspect"
    command = [
        inspector.resolve().relative_to(ROOT.resolve()).as_posix(),
        "--json",
        "--offset",
        str(FS_OFFSET),
        "--size",
        str(region_bytes),
        image.resolve().relative_to(ROOT.resolve()).as_posix(),
        "stat",
        "/journal-cut",
    ]
    completed = subprocess.run(
        command,
        cwd=str(ROOT),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
        check=False,
    )
    (case_root / "mutation-stat.stdout.log").write_bytes(completed.stdout)
    (case_root / "mutation-stat.stderr.log").write_bytes(completed.stderr)
    recorded_command = [
        pathlib.Path(command[0]).name,
        *[
            item.replace(str(image.resolve()), image.name)
            for item in command[1:]
        ],
    ]
    record: Dict[str, object] = {
        "expected": expected,
        "argv": recorded_command,
        "returncode": completed.returncode,
        "passed": False,
    }
    if expected == "absent":
        record["passed"] = completed.returncode == 2 and not completed.stdout
        return record
    if completed.returncode != 0:
        return record
    try:
        stat = json.loads(completed.stdout.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError):
        return record
    record["stat"] = stat
    record["passed"] = stat.get("size") == 0 and stat.get("type") == "regular"
    return record


def run_case(
    *,
    source_image: pathlib.Path,
    session_root: pathlib.Path,
    checkpoint: int,
    name: str,
    expected: str,
    qemu: str,
    timeout_seconds: float,
) -> Dict[str, object]:
    case_root = session_root / ("{:02d}-{}".format(checkpoint, name))
    case_root.mkdir(parents=True, exist_ok=False)
    image = case_root / "writable.img"
    shutil.copyfile(str(source_image), str(image))
    initialization = run_qemu(
        image=image,
        artifacts_dir=case_root,
        run_id="initialize",
        timeout_seconds=timeout_seconds,
        expected_debug_exit=0x10,
        required_patterns=(r"^NS:NSFS:PHASE write-complete$",),
        forbidden_patterns=(r"(?i)panic", r"^# NS_GATE G4 FAIL$"),
        qemu=qemu,
        memory_mib=256,
        writable=True,
    )
    if not initialization.passed:
        raise MatrixError("{}: baseline initialization failed".format(name))

    write_control(image, checkpoint)
    cut = hard_cut_at_marker(
        qemu=qemu,
        image=image,
        case_root=case_root,
        marker="NS:NSFS:CUT phase={}".format(name),
        timeout_seconds=timeout_seconds,
    )
    if not cut["passed"]:
        raise MatrixError("{}: hard-cut marker was not reached exactly".format(name))

    write_control(image, 0)
    recovery = run_qemu(
        image=image,
        artifacts_dir=case_root,
        run_id="recovery",
        timeout_seconds=timeout_seconds,
        expected_debug_exit=0x10,
        required_patterns=(
            r"^NS:NSFS:RECOVERY mutation={}$".format(expected),
            r"^NS:GATE:G4:PASS image_sha256={}$".format(PAYLOAD_SHA256),
            r"^NS:RUN:COMPLETE$",
        ),
        forbidden_patterns=(r"(?i)panic", r"^# NS_GATE G4 FAIL$"),
        qemu=qemu,
        memory_mib=256,
        writable=True,
    )
    if not recovery.passed:
        raise MatrixError("{}: kernel journal recovery failed".format(name))

    probe = FilesystemProbe(
        offset_bytes=FS_OFFSET,
        tail_reserve_bytes=TAIL_RESERVE,
        path="/evidence/nested/payload.bin",
        expected_bytes=PAYLOAD_BYTES,
        expected_sha256=PAYLOAD_SHA256,
    )
    filesystem = run_filesystem_probe(probe, image, case_root)
    if not filesystem.get("passed"):
        raise MatrixError(
            "{}: independent filesystem oracle failed: {}".format(
                name, filesystem.get("error", "unknown error")
            )
        )
    mutation = inspect_mutation(
        case_root=case_root,
        image=image,
        expected=expected,
        region_bytes=int(filesystem["region_bytes"]),
    )
    if not mutation["passed"]:
        raise MatrixError("{}: recovered transaction state is wrong".format(name))
    result = {
        "schema": "northstar.journal-matrix-case.v1",
        "checkpoint": checkpoint,
        "name": name,
        "expected_mutation": expected,
        "passed": True,
        "initialization_result": str(
            initialization.result_file.relative_to(case_root)
        ),
        "cut": cut,
        "recovery_result": str(recovery.result_file.relative_to(case_root)),
        "filesystem_probe": filesystem,
        "mutation_probe": mutation,
        "final_image_sha256": sha256_file(image),
    }
    write_json(case_root / "case-result.json", result)
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=pathlib.Path, required=True)
    parser.add_argument(
        "--artifacts-dir", type=pathlib.Path,
        default=ROOT / "artifacts" / "journal-matrix"
    )
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--session-id")
    arguments = parser.parse_args()
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    return arguments


def main() -> int:
    arguments = parse_args()
    if not arguments.image.is_file():
        print("run_journal_matrix: image does not exist: {}".format(arguments.image),
              file=sys.stderr)
        return 2
    if arguments.session_id is None:
        timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        session_id = "journal-matrix-{}-{}".format(timestamp, os.getpid())
    else:
        session_id = re.sub(r"[^A-Za-z0-9_.-]", "_", arguments.session_id)
    session_root = arguments.artifacts_dir / session_id
    session_root.mkdir(parents=True, exist_ok=False)
    cases = []
    error = None
    try:
        for checkpoint, name, expected in CUTPOINTS:
            print("RUN  {}".format(name), flush=True)
            case = run_case(
                source_image=arguments.image,
                session_root=session_root,
                checkpoint=checkpoint,
                name=name,
                expected=expected,
                qemu=arguments.qemu,
                timeout_seconds=arguments.timeout,
            )
            cases.append(case)
            print("PASS {}".format(name), flush=True)
    except (MatrixError, OSError, subprocess.SubprocessError) as failure:
        error = str(failure)
        print("FAIL {}".format(error), file=sys.stderr, flush=True)
    summary = {
        "schema": "northstar.journal-matrix.v1",
        "passed": error is None and len(cases) == len(CUTPOINTS),
        "error": error,
        "crash_model": {
            "qemu_cache": "writeback",
            "cut": "SIGKILL after a guest ATA FLUSH durability checkpoint",
            "assumptions": [
                "completed guest FLUSH requests have reached host durable storage",
                "unflushed QEMU writeback-cache contents may be lost",
                "sector writes are atomic; torn sectors and physical-media failure are out of scope",
            ],
        },
        "source_image": (
            arguments.image.resolve().relative_to(ROOT.resolve()).as_posix()
            if ROOT.resolve() in arguments.image.resolve().parents
            else arguments.image.name
        ),
        "image_sha256": sha256_file(arguments.image),
        "source_image_sha256": sha256_file(arguments.image),
        "cases": cases,
    }
    write_json(session_root / "summary.json", summary)
    print("journal matrix evidence: {}".format(session_root))
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
