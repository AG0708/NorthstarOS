#!/usr/bin/env python3
"""Run the bounded NorthstarOS release gate and generate its evidence manifest."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import re
import select
import signal
import subprocess
import sys
import time
from typing import Dict, Optional, Sequence


ROOT = pathlib.Path(__file__).resolve().parents[1]


class ReleaseGateError(RuntimeError):
    pass


def require_clean_git_repository(expected_commit: Optional[str] = None) -> str:
    """Return HEAD only for a clean, standalone repository rooted at ROOT."""

    def git(*arguments: str) -> str:
        try:
            completed = subprocess.run(
                ["git", "-C", str(ROOT)] + list(arguments),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=10,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise ReleaseGateError("cannot inspect Git state: {}".format(error))
        if completed.returncode != 0:
            diagnostic = completed.stderr.decode("utf-8", errors="replace").strip()
            raise ReleaseGateError(
                "Git inspection failed for {}: {}".format(
                    " ".join(arguments), diagnostic or "exit {}".format(completed.returncode)
                )
            )
        return completed.stdout.decode("utf-8", errors="replace").rstrip("\n")

    top_level = pathlib.Path(git("rev-parse", "--show-toplevel")).resolve()
    if top_level != ROOT.resolve():
        raise ReleaseGateError(
            "release source must be the root of its own Git repository"
        )
    commit = git("rev-parse", "--verify", "HEAD")
    if not re.fullmatch(r"[0-9a-f]{40,64}", commit):
        raise ReleaseGateError("Git HEAD is not a full object identifier")
    status = git("status", "--porcelain=v1", "--untracked-files=all")
    if status:
        raise ReleaseGateError(
            "release source has {} uncommitted path(s)".format(len(status.splitlines()))
        )
    if expected_commit is not None and commit != expected_commit:
        raise ReleaseGateError(
            "Git HEAD changed during the release gate ({} -> {})".format(
                expected_commit, commit
            )
        )
    return commit


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: pathlib.Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name("." + path.name + ".tmp-{}".format(os.getpid()))
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(str(temporary), str(path))


def recorded_argv(argv: Sequence[str]) -> list[str]:
    recorded = []
    root = ROOT.resolve()
    for item in argv:
        candidate = pathlib.Path(item)
        if candidate.is_absolute():
            try:
                item = candidate.resolve().relative_to(root).as_posix()
            except ValueError:
                item = candidate.name
        recorded.append(item)
    return recorded


def terminate(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=3.0)
        return
    except (ProcessLookupError, subprocess.TimeoutExpired):
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def run_step(
    name: str,
    argv: Sequence[str],
    *,
    release_root: pathlib.Path,
    timeout_seconds: float,
) -> Dict[str, object]:
    print("\n==> {}".format(name), flush=True)
    log_path = release_root / (name + ".log")
    started = time.monotonic()
    environment = dict(os.environ)
    environment.update(
        {
            "LANG": "C",
            "LC_ALL": "C",
            "PYTHONHASHSEED": "0",
            "SOURCE_DATE_EPOCH": environment.get("SOURCE_DATE_EPOCH", "1700000000"),
            "TZ": "UTC",
        }
    )
    timed_out = False
    with log_path.open("wb") as log:
        process = subprocess.Popen(
            list(argv),
            cwd=str(ROOT),
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        assert process.stdout is not None
        deadline = started + timeout_seconds
        while True:
            if time.monotonic() >= deadline:
                timed_out = True
                terminate(process)
                break
            ready, _, _ = select.select([process.stdout], [], [], 0.1)
            if ready:
                chunk = os.read(process.stdout.fileno(), 4096)
            else:
                chunk = b""
            if chunk:
                log.write(chunk)
                log.flush()
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
                continue
            if process.poll() is not None:
                break
        if process.poll() is None:
            terminate(process)
        remainder = process.stdout.read()
        if remainder:
            log.write(remainder)
            sys.stdout.buffer.write(remainder)
            sys.stdout.buffer.flush()
        returncode = process.returncode
    result = {
        "name": name,
        "passed": returncode == 0 and not timed_out,
        "returncode": returncode,
        "timed_out": timed_out,
        "elapsed_seconds": round(time.monotonic() - started, 6),
        "command": recorded_argv(argv),
        "log": log_path.name,
        "log_sha256": sha256_file(log_path),
    }
    if not result["passed"]:
        raise ReleaseGateError(
            "{} failed (returncode={}, timed_out={})".format(
                name, returncode, timed_out
            )
        )
    return result


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=pathlib.Path, required=True)
    parser.add_argument("--interactive-image", type=pathlib.Path, required=True)
    parser.add_argument(
        "--artifacts-dir",
        type=pathlib.Path,
        default=ROOT / "artifacts" / "release",
    )
    parser.add_argument("--cross", default="x86_64-elf-")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--session-id")
    return parser.parse_args(argv)


def main() -> int:
    arguments = parse_args()
    try:
        source_commit = require_clean_git_repository()
        release_version = (ROOT / "VERSION").read_text(encoding="ascii").strip()
        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?", release_version):
            raise ReleaseGateError("VERSION is not a valid release version")
    except (OSError, ReleaseGateError) as error:
        print("run_release_gate: {}".format(error), file=sys.stderr)
        return 2
    if not arguments.image.is_file() or not arguments.interactive_image.is_file():
        print("run_release_gate: both profile images must exist", file=sys.stderr)
        return 2
    if arguments.session_id is None:
        stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        session_id = "release-{}-{}".format(stamp, os.getpid())
    else:
        session_id = re.sub(r"[^A-Za-z0-9_.-]", "_", arguments.session_id)
    release_root = arguments.artifacts_dir / session_id
    try:
        release_root.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        print("run_release_gate: cannot create evidence directory: {}".format(error), file=sys.stderr)
        return 2

    python = sys.executable
    os.environ["CROSS"] = arguments.cross
    image_hash = sha256_file(arguments.image)
    interactive_hash = sha256_file(arguments.interactive_image)
    steps = []
    error = None
    started = time.monotonic()
    try:
        steps.append(
            run_step(
                "python-host-tests",
                [python, "-m", "unittest", "discover", "-s", "tests/host", "-p", "test_*.py"],
                release_root=release_root,
                timeout_seconds=300,
            )
        )
        steps.append(
            run_step(
                "native-host-tests",
                [
                    python,
                    "tools/run_host_tests.py",
                    "--artifacts-dir",
                    str(release_root / "host"),
                    "--session-id",
                    "host",
                    "--keep-going",
                ],
                release_root=release_root,
                timeout_seconds=900,
            )
        )
        host_summary = release_root / "host" / "host" / "summary.json"

        steps.append(
            run_step(
                "canonical-integration",
                [
                    python,
                    "tools/run_integration.py",
                    "--image",
                    str(arguments.image),
                    "--milestone",
                    "M5",
                    "--artifacts-dir",
                    str(release_root / "integration"),
                    "--session-id",
                    "canonical",
                    "--qemu",
                    arguments.qemu,
                ],
                release_root=release_root,
                timeout_seconds=900,
            )
        )
        integration_summary = release_root / "integration" / "canonical" / "summary.json"

        adversarial = release_root / "adversarial.json"
        steps.append(
            run_step(
                "adversarial-coverage",
                [
                    python,
                    "tools/check_adversarial_results.py",
                    "--image",
                    str(arguments.image),
                    "--host-summary",
                    str(host_summary),
                    "--integration-summary",
                    str(integration_summary),
                    "--output",
                    str(adversarial),
                ],
                release_root=release_root,
                timeout_seconds=60,
            )
        )

        steps.append(
            run_step(
                "journal-matrix",
                [
                    python,
                    "tools/run_journal_matrix.py",
                    "--image",
                    str(arguments.image),
                    "--artifacts-dir",
                    str(release_root / "journal"),
                    "--session-id",
                    "matrix",
                    "--qemu",
                    arguments.qemu,
                ],
                release_root=release_root,
                timeout_seconds=900,
            )
        )
        journal_summary = release_root / "journal" / "matrix" / "summary.json"

        steps.append(
            run_step(
                "interactive-pty",
                [
                    python,
                    "tools/test_interactive.py",
                    "--image",
                    str(arguments.interactive_image),
                    "--canonical-image",
                    str(arguments.image),
                    "--artifacts-dir",
                    str(release_root / "interactive"),
                    "--session-id",
                    "pty",
                    "--qemu",
                    arguments.qemu,
                ],
                release_root=release_root,
                timeout_seconds=180,
            )
        )
        interactive_summary = release_root / "interactive" / "pty" / "summary.json"

        steps.append(
            run_step(
                "reproducibility",
                [
                    python,
                    "tools/check_reproducible.py",
                    "--source",
                    ".",
                    "--command",
                    "make all CROSS={}".format(arguments.cross),
                    "--source-date-epoch",
                    os.environ.get("SOURCE_DATE_EPOCH", "1700000000"),
                    "--artifacts-dir",
                    str(release_root / "reproducibility"),
                ],
                release_root=release_root,
                timeout_seconds=900,
            )
        )
        reproducibility_summary = release_root / "reproducibility" / "report.json"

        steps.append(
            run_step(
                "cold-boot-repetition",
                [
                    python,
                    "tools/run_integration.py",
                    "--image",
                    str(arguments.image),
                    "--milestone",
                    "M5",
                    "--repeat",
                    "100",
                    "--artifacts-dir",
                    str(release_root / "repetition"),
                    "--session-id",
                    "cold-boots",
                    "--qemu",
                    arguments.qemu,
                    "--keep-going",
                ],
                release_root=release_root,
                timeout_seconds=7200,
            )
        )
        repetition_summary = release_root / "repetition" / "cold-boots" / "summary.json"

        require_clean_git_repository(source_commit)
        sbom = release_root / "NorthstarOS.spdx.json"
        steps.append(
            run_step(
                "sbom",
                [
                    python,
                    "tools/gen_sbom.py",
                    "--source",
                    ".",
                    "--output",
                    str(sbom),
                    "--version",
                    release_version,
                    "--require-clean",
                ],
                release_root=release_root,
                timeout_seconds=120,
            )
        )

        evidence = release_root / "evidence.json"
        evidence_command = [
            python,
            "tools/gen_release_evidence.py",
            "--source",
            ".",
            "--image",
            str(arguments.image),
            "--image-manifest",
            "build/image-layout.json",
            "--boot-layout",
            "build/generated/boot_layout.json",
            "--interactive-image",
            str(arguments.interactive_image),
            "--sbom",
            str(sbom),
            "--host-result",
            str(host_summary),
            "--evidence",
            str(release_root),
            "--output",
            str(evidence),
            "--strict-tools",
            "--require-release-gates",
            "--require-clean",
        ]
        for result in (
            integration_summary,
            repetition_summary,
            journal_summary,
            interactive_summary,
            reproducibility_summary,
            adversarial,
        ):
            evidence_command.extend(("--test-result", str(result)))
        steps.append(
            run_step(
                "release-evidence",
                evidence_command,
                release_root=release_root,
                timeout_seconds=120,
            )
        )
        require_clean_git_repository(source_commit)
    except (OSError, ReleaseGateError) as failure:
        error = str(failure)
        print("run_release_gate: FAIL: {}".format(error), file=sys.stderr)

    summary = {
        "schema": "northstar.release-gate.v1",
        "passed": error is None,
        "error": error,
        "canonical_image_sha256": image_hash,
        "interactive_image_sha256": interactive_hash,
        "release_version": release_version,
        "source_commit": source_commit,
        "repeat_requested": 100,
        "elapsed_seconds": round(time.monotonic() - started, 6),
        "steps": steps,
    }
    evidence_path = release_root / "evidence.json"
    if evidence_path.is_file():
        summary["evidence_manifest"] = evidence_path.name
        summary["evidence_manifest_sha256"] = sha256_file(evidence_path)
    write_json(release_root / "summary.json", summary)
    print("release evidence: {}".format(release_root))
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
