#!/usr/bin/env python3
"""Compile and run every registered NorthstarOS hosted test deterministically."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import time
from typing import Dict, List, Optional, Sequence, Tuple


ROOT = pathlib.Path(__file__).resolve().parents[1]

# A closed dependency map is deliberate: adding a test without wiring its real
# implementation sources is a hard error rather than a silently unexecuted
# file or a test that accidentally uses a mock implementation.
C_TEST_SOURCES: Dict[str, Tuple[str, ...]] = {
    "test_sha256.c": ("kernel/core/sha256.c",),
    "test_fs_block.c": (
        "kernel/fs/block.c",
        "kernel/fs/block_mem.c",
        "kernel/fs/block_slice.c",
    ),
    "test_fs_block_ata_pio.c": (
        "kernel/fs/block.c",
        "kernel/drivers/ata.c",
    ),
    "test_fs_nsfs.c": (
        "kernel/fs/block.c",
        "kernel/fs/block_mem.c",
        "kernel/fs/nsfs.c",
    ),
    "test_fs_nsfs_compat.c": (
        "kernel/fs/block.c",
        "kernel/fs/block_mem.c",
        "kernel/fs/nsfs.c",
    ),
    "test_fs_nsfs_vfs.c": (
        "kernel/fs/block.c",
        "kernel/fs/block_mem.c",
        "kernel/fs/nsfs.c",
        "kernel/fs/vfs.c",
        "kernel/fs/nsfs_vfs.c",
    ),
    "test_fs_vfs.c": (
        "kernel/fs/vfs.c",
        "kernel/fs/initramfs.c",
    ),
    "test_net_base.c": (
        "kernel/net/net_checksum.c",
        "kernel/net/net_device.c",
        "kernel/net/net_ethernet.c",
        "kernel/net/net_arp.c",
        "kernel/net/net_ipv4.c",
        "kernel/net/net_icmp.c",
    ),
    "test_net_rtl8139.c": ("kernel/drivers/rtl8139.c",),
    "test_net_services.c": (
        "kernel/net/net_dhcp.c",
        "kernel/net/net_dns.c",
    ),
    "test_net_socket_backend.c": (
        "kernel/net/net_checksum.c",
        "kernel/net/net_udp.c",
        "kernel/net/net_tcp.c",
        "kernel/net/socket_api.c",
        "kernel/net/socket_net_backend.c",
    ),
    "test_net_stack.c": (
        "kernel/drivers/rtl8139.c",
        "kernel/net/net_checksum.c",
        "kernel/net/net_device.c",
        "kernel/net/net_ethernet.c",
        "kernel/net/net_arp.c",
        "kernel/net/net_ipv4.c",
        "kernel/net/net_icmp.c",
        "kernel/net/net_udp.c",
        "kernel/net/net_tcp.c",
        "kernel/net/net_dhcp.c",
        "kernel/net/net_dns.c",
        "kernel/net/socket_api.c",
        "kernel/net/socket_net_backend.c",
        "kernel/net/net_stack.c",
    ),
    "test_net_tcp.c": (
        "kernel/net/net_checksum.c",
        "kernel/net/net_tcp.c",
    ),
    "test_net_udp_socket.c": (
        "kernel/net/net_checksum.c",
        "kernel/net/net_udp.c",
        "kernel/net/socket_api.c",
    ),
    "test_proc_elf.c": ("kernel/proc/elf_loader.c",),
    "test_proc_process.c": ("kernel/proc/process.c",),
    "test_proc_scheduler.c": ("kernel/proc/scheduler.c",),
    "test_proc_syscall.c": (
        "kernel/proc/syscall.c",
        "kernel/proc/usercopy.c",
        "kernel/net/socket_api.c",
    ),
    "test_proc_usercopy.c": ("kernel/proc/usercopy.c",),
    "test_mm_pmm.c": ("kernel/mm/pmm.c",),
    "test_mm_vmm.c": ("kernel/mm/pmm.c", "kernel/mm/vmm.c"),
    "test_mm_heap.c": (
        "kernel/mm/heap.c",
        "kernel/mm/pmm.c",
        "kernel/mm/vmm.c",
    ),
}

C_TEST_FLAGS: Dict[str, Tuple[str, ...]] = {
    "test_mm_heap.c": ("-pthread",),
}

# This binary takes two filesystem-image paths and is executed end-to-end by
# test_fs_tools.sh after the independent formatter/checker have been built.
# Compile it in every C mode here, then let the script supply its fixtures.
C_COMPILE_ONLY = {"test_fs_nsfs_compat.c"}

BASE_FLAGS = (
    "-std=c11",
    "-O2",
    "-g3",
    "-Iinclude",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wformat=2",
    "-Wshadow",
    "-Wstrict-prototypes",
    "-Wmissing-prototypes",
    "-Wundef",
    "-Wvla",
    "-fno-common",
)

MODE_FLAGS = {
    "strict": (),
    "ubsan": (
        "-O1",
        "-fsanitize=undefined",
        "-fno-sanitize-recover=undefined",
        "-fno-omit-frame-pointer",
    ),
    "asan": (
        "-O1",
        "-fsanitize=address",
        "-fno-omit-frame-pointer",
    ),
}


class HostTestError(RuntimeError):
    pass


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_name(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]", "_", value)


def recorded_argv(argv: Sequence[str]) -> List[str]:
    recorded: List[str] = []
    root = ROOT.resolve()
    for index, item in enumerate(argv):
        candidate = pathlib.Path(item)
        if candidate.is_absolute():
            try:
                item = candidate.resolve().relative_to(root).as_posix()
            except ValueError:
                item = candidate.name
        elif index == 0 and os.path.sep in item:
            item = candidate.name
        recorded.append(item)
    return recorded


def discovered_tests() -> Tuple[List[pathlib.Path], List[pathlib.Path]]:
    root = ROOT / "tests" / "host"
    c_tests = sorted(root.glob("test_*.c"))
    script_tests = sorted(
        path
        for pattern in ("test_*.py", "test_*.sh")
        for path in root.glob(pattern)
        if path.name != "test_support.py"
    )
    return c_tests, script_tests


def validate_registry(c_tests: Sequence[pathlib.Path]) -> None:
    discovered_names = {path.name for path in c_tests}
    unmapped = sorted(discovered_names - set(C_TEST_SOURCES))
    if unmapped:
        raise HostTestError(
            "unmapped C host tests (add their production dependencies to C_TEST_SOURCES): {}".format(
                unmapped
            )
        )
    missing_sources: List[str] = []
    for test in c_tests:
        for source in C_TEST_SOURCES[test.name]:
            if not (ROOT / source).is_file():
                missing_sources.append("{} -> {}".format(test.name, source))
    if missing_sources:
        raise HostTestError("registered production sources are missing: {}".format(missing_sources))


def run_bounded(
    argv: Sequence[str],
    *,
    cwd: pathlib.Path,
    timeout: float,
    environment: Optional[Dict[str, str]] = None,
) -> Dict[str, object]:
    started = time.monotonic()
    try:
        completed = subprocess.run(
            list(argv),
            cwd=str(cwd),
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
        returncode: Optional[int] = completed.returncode
        output = completed.stdout
        timed_out = False
        error = None
    except subprocess.TimeoutExpired as failure:
        returncode = None
        output = failure.stdout or b""
        timed_out = True
        error = "timeout after {} seconds".format(timeout)
    except OSError as failure:
        returncode = None
        output = str(failure).encode("utf-8", errors="replace")
        timed_out = False
        error = str(failure)
    return {
        "argv": list(argv),
        "returncode": returncode,
        "timed_out": timed_out,
        "elapsed_seconds": round(time.monotonic() - started, 6),
        "output": output,
        "error": error,
    }


def environment_for_mode(mode: str) -> Dict[str, str]:
    environment = dict(os.environ)
    environment.update(
        {
            "LANG": "C",
            "LC_ALL": "C",
            "TZ": "UTC",
            "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:abort_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        }
    )
    return environment


def write_log(path: pathlib.Path, output: bytes) -> None:
    path.write_bytes(output)


def run_c_test(
    test: pathlib.Path,
    mode: str,
    *,
    compiler: str,
    session: pathlib.Path,
    compile_timeout: float,
    run_timeout: float,
) -> Dict[str, object]:
    test_id = test.stem
    mode_dir = session / mode
    mode_dir.mkdir(parents=True, exist_ok=True)
    binary = mode_dir / test_id
    compile_log = mode_dir / (test_id + ".compile.log")
    run_log = mode_dir / (test_id + ".run.log")
    sources = [str(test.relative_to(ROOT))] + list(C_TEST_SOURCES[test.name])
    command = (
        [compiler]
        + list(BASE_FLAGS)
        + list(MODE_FLAGS[mode])
        + list(C_TEST_FLAGS.get(test.name, ()))
        + sources
        + ["-o", str(binary)]
    )
    compile_result = run_bounded(
        command,
        cwd=ROOT,
        timeout=compile_timeout,
        environment=environment_for_mode(mode),
    )
    write_log(compile_log, compile_result.pop("output"))
    compile_result["argv"] = recorded_argv(compile_result["argv"])
    record: Dict[str, object] = {
        "id": test_id,
        "kind": "c",
        "mode": mode,
        "sources": sources,
        "compile": dict(compile_result, log=str(compile_log.relative_to(session))),
        "run": None,
        "passed": False,
    }
    if compile_result["returncode"] != 0 or compile_result["timed_out"]:
        return record
    if test.name in C_COMPILE_ONLY:
        record["run"] = {
            "delegated_to": "tests/host/test_fs_tools.sh",
            "reason": "requires independently formatted input and output image paths",
        }
        record["passed"] = True
        return record
    run_result = run_bounded(
        [str(binary)],
        cwd=ROOT,
        timeout=run_timeout,
        environment=environment_for_mode(mode),
    )
    write_log(run_log, run_result.pop("output"))
    run_result["argv"] = recorded_argv(run_result["argv"])
    record["run"] = dict(run_result, log=str(run_log.relative_to(session)))
    record["passed"] = run_result["returncode"] == 0 and not run_result["timed_out"]
    return record


def run_script_test(
    test: pathlib.Path,
    *,
    session: pathlib.Path,
    timeout: float,
) -> Dict[str, object]:
    script_dir = session / "scripts"
    script_dir.mkdir(parents=True, exist_ok=True)
    log = script_dir / (test.stem + ".run.log")
    if test.suffix == ".py":
        command = [sys.executable, str(test.relative_to(ROOT))]
    elif test.suffix == ".sh":
        command = ["/bin/sh", str(test.relative_to(ROOT))]
    else:
        raise HostTestError("unsupported host script type: {}".format(test))
    result = run_bounded(command, cwd=ROOT, timeout=timeout, environment=environment_for_mode("strict"))
    write_log(log, result.pop("output"))
    result["argv"] = recorded_argv(result["argv"])
    passed = result["returncode"] == 0 and not result["timed_out"]
    return {
        "id": test.stem,
        "kind": test.suffix.lstrip("."),
        "mode": "script",
        "sources": [str(test.relative_to(ROOT))],
        "compile": None,
        "run": dict(result, log=str(log.relative_to(session))),
        "passed": passed,
    }


def emit_tap(path: pathlib.Path, records: Sequence[Dict[str, object]]) -> None:
    lines = ["TAP version 13", "1..{}".format(len(records))]
    for index, record in enumerate(records, start=1):
        status = "ok" if record["passed"] else "not ok"
        lines.append("{} {} - {} [{}]".format(status, index, record["id"], record["mode"]))
        if not record["passed"]:
            lines.append("  ---")
            lines.append("  kind: host-test-failure")
            lines.append("  mode: {}".format(record["mode"]))
            lines.append("  ...")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("HOST_CC", "cc"))
    parser.add_argument("--mode", action="append", choices=sorted(MODE_FLAGS), default=[])
    parser.add_argument("--artifacts-dir", type=pathlib.Path, default=ROOT / "artifacts" / "host")
    parser.add_argument("--session-id")
    parser.add_argument("--compile-timeout", type=float, default=60.0)
    parser.add_argument("--run-timeout", type=float, default=30.0)
    parser.add_argument("--keep-going", action="store_true")
    parser.add_argument("--list", action="store_true")
    arguments = parser.parse_args()
    if not arguments.mode:
        arguments.mode = ["strict", "ubsan"]
    if arguments.compile_timeout <= 0 or arguments.run_timeout <= 0:
        parser.error("timeouts must be positive")
    return arguments


def main() -> int:
    arguments = parse_args()
    c_tests, script_tests = discovered_tests()
    try:
        validate_registry(c_tests)
    except HostTestError as error:
        print("run_host_tests: error: {}".format(error), file=sys.stderr)
        return 2
    if arguments.list:
        for test in c_tests:
            print("{}: {}".format(test.name, " ".join(C_TEST_SOURCES[test.name])))
        for test in script_tests:
            print("{}: script".format(test.name))
        return 0
    compiler = shutil.which(arguments.cc) if os.path.sep not in arguments.cc else arguments.cc
    if compiler is None or not pathlib.Path(compiler).is_file():
        print("run_host_tests: error: compiler not found: {}".format(arguments.cc), file=sys.stderr)
        return 2
    session_id = arguments.session_id
    if session_id is None:
        timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        session_id = "host-{}-{}".format(timestamp, os.getpid())
    session = arguments.artifacts_dir / safe_name(session_id)
    try:
        session.mkdir(parents=True, exist_ok=False)
        version_result = run_bounded([compiler, "--version"], cwd=ROOT, timeout=10)
        version_output = version_result.pop("output")
        write_log(session / "compiler-version.log", version_output)
        records: List[Dict[str, object]] = []
        stop = False
        for mode in arguments.mode:
            for test in c_tests:
                print("BUILD+RUN {} [{}]".format(test.stem, mode), flush=True)
                record = run_c_test(
                    test,
                    mode,
                    compiler=compiler,
                    session=session,
                    compile_timeout=arguments.compile_timeout,
                    run_timeout=arguments.run_timeout,
                )
                records.append(record)
                if not record["passed"] and not arguments.keep_going:
                    stop = True
                    break
            if stop:
                break
        if not stop:
            for test in script_tests:
                print("RUN {} [script]".format(test.stem), flush=True)
                record = run_script_test(test, session=session, timeout=arguments.run_timeout)
                records.append(record)
                if not record["passed"] and not arguments.keep_going:
                    break
        expected_count = len(c_tests) * len(arguments.mode) + len(script_tests)
        passed = len(records) == expected_count and all(bool(record["passed"]) for record in records)
        summary = {
            "schema": "northstar.host-test-summary.v1",
            "passed": passed,
            "compiler": pathlib.Path(compiler).name,
            "compiler_version_sha256": hashlib.sha256(version_output).hexdigest(),
            "modes": arguments.mode,
            "expected_test_executions": expected_count,
            "completed_test_executions": len(records),
            "records": records,
        }
        summary_path = session / "summary.json"
        summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        emit_tap(session / "summary.tap", records)
    except (OSError, HostTestError) as error:
        print("run_host_tests: error: {}".format(error), file=sys.stderr)
        return 2
    print("host-test evidence: {}".format(session))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
