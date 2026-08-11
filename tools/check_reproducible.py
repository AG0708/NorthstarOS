#!/usr/bin/env python3
"""Build NorthstarOS twice in isolated trees and compare release artifacts."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile
from typing import Dict, Iterable, List, Sequence


DEFAULT_EPOCH = 1_700_000_000
EXCLUDED_NAMES = {".git", ".DS_Store", "artifacts", "__pycache__", ".pytest_cache"}


class ReproducibilityError(RuntimeError):
    pass


def hash_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def copy_ignore(source_root: pathlib.Path):
    def ignore(directory: str, names: List[str]) -> List[str]:
        relative = pathlib.Path(directory).resolve().relative_to(source_root.resolve())
        ignored = [name for name in names if name in EXCLUDED_NAMES or name.endswith(".pyc")]
        if relative == pathlib.Path("."):
            ignored.extend(name for name in names if name.startswith("build-"))
        # build/*.mk are versioned source fragments; every other build/ entry
        # is generated and must not seed either supposedly clean build.
        if relative == pathlib.Path("build"):
            ignored.extend(name for name in names if not name.endswith(".mk"))
        return sorted(set(ignored))

    return ignore


def source_files(root: pathlib.Path) -> Iterable[pathlib.Path]:
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        if any(part in EXCLUDED_NAMES for part in relative.parts):
            continue
        if relative.parts and relative.parts[0].startswith("build-"):
            continue
        if relative.parts and relative.parts[0] == "build" and path.suffix != ".mk":
            continue
        if path.suffix == ".pyc":
            continue
        yield path


def source_tree_hash(root: pathlib.Path) -> str:
    digest = hashlib.sha256()
    for path in source_files(root):
        relative = path.relative_to(root).as_posix().encode("utf-8")
        digest.update(relative)
        digest.update(b"\x00")
        digest.update(hash_file(path).encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def display_path(path: pathlib.Path, source: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(source.resolve()).as_posix()
    except ValueError:
        return path.name


def deterministic_environment(epoch: int) -> Dict[str, str]:
    environment = dict(os.environ)
    environment.update(
        {
            "SOURCE_DATE_EPOCH": str(epoch),
            "PYTHONHASHSEED": "0",
            "LC_ALL": "C",
            "LANG": "C",
            "TZ": "UTC",
        }
    )
    return environment


def run_build(
    worktree: pathlib.Path,
    command: Sequence[str],
    environment: Dict[str, str],
    log_path: pathlib.Path,
    timeout_seconds: float,
) -> Dict[str, object]:
    started = dt.datetime.now(dt.timezone.utc)
    try:
        completed = subprocess.run(
            list(command),
            cwd=str(worktree),
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout_seconds,
            check=False,
        )
        output = completed.stdout
        returncode = completed.returncode
        timed_out = False
    except subprocess.TimeoutExpired as error:
        output = error.stdout or b""
        returncode = None
        timed_out = True
    except OSError as error:
        output = str(error).encode("utf-8", errors="replace")
        returncode = None
        timed_out = False
    log_path.write_bytes(output)
    ended = dt.datetime.now(dt.timezone.utc)
    return {
        "returncode": returncode,
        "timed_out": timed_out,
        "elapsed_seconds": round((ended - started).total_seconds(), 6),
        "log": log_path.name,
        "log_sha256": hash_file(log_path),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--command", default="make all", help="build argv, parsed without a shell")
    parser.add_argument(
        "--artifact",
        action="append",
        default=[],
        help="relative artifact to compare (repeatable; defaults to build/northstar.img and its manifests)",
    )
    parser.add_argument("--source-date-epoch", type=int, default=DEFAULT_EPOCH)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument(
        "--artifacts-dir",
        type=pathlib.Path,
        default=pathlib.Path("artifacts/reproducibility"),
    )
    arguments = parser.parse_args()
    if arguments.source_date_epoch < 0:
        parser.error("--source-date-epoch must be non-negative")
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    if not arguments.artifact:
        arguments.artifact = [
            "build/northstar.img",
            "build/northstar.img.sha256",
            "build/image-layout.json",
        ]
    for artifact in arguments.artifact:
        path = pathlib.PurePosixPath(artifact)
        if path.is_absolute() or ".." in path.parts:
            parser.error("--artifact paths must stay under each isolated source tree")
    return arguments


def main() -> int:
    arguments = parse_args()
    source = arguments.source.resolve()
    if not (source / "Makefile").is_file():
        print("check_reproducible: source tree has no Makefile: {}".format(source), file=sys.stderr)
        return 2
    command = shlex.split(arguments.command)
    if not command:
        print("check_reproducible: --command is empty", file=sys.stderr)
        return 2
    artifacts_dir = arguments.artifacts_dir.resolve()
    artifacts_dir.mkdir(parents=True, exist_ok=True)
    report: Dict[str, object] = {
        "schema": "northstar.reproducibility.v1",
        "source_tree_sha256": source_tree_hash(source),
        "source_date_epoch": arguments.source_date_epoch,
        "command": command,
        "artifacts": arguments.artifact,
        "builds": [],
        "comparisons": [],
        "passed": False,
    }
    exit_code = 1
    try:
        with tempfile.TemporaryDirectory(prefix="northstar-repro-") as temporary:
            temporary_root = pathlib.Path(temporary)
            worktrees: List[pathlib.Path] = []
            for index in (1, 2):
                worktree = temporary_root / "build-{}".format(index)
                shutil.copytree(
                    str(source),
                    str(worktree),
                    symlinks=True,
                    ignore=copy_ignore(source),
                )
                worktrees.append(worktree)
                build_record = run_build(
                    worktree,
                    command,
                    deterministic_environment(arguments.source_date_epoch),
                    artifacts_dir / "build-{}.log".format(index),
                    arguments.timeout,
                )
                report["builds"].append(build_record)
                if build_record["returncode"] != 0 or build_record["timed_out"]:
                    raise ReproducibilityError("isolated build {} failed".format(index))

            comparisons = []
            for artifact_name in arguments.artifact:
                paths = [worktree / artifact_name for worktree in worktrees]
                if not all(path.is_file() for path in paths):
                    missing = [str(path) for path in paths if not path.is_file()]
                    raise ReproducibilityError("missing comparison artifacts: {}".format(missing))
                hashes = [hash_file(path) for path in paths]
                comparison = {
                    "path": artifact_name,
                    "build_1_sha256": hashes[0],
                    "build_2_sha256": hashes[1],
                    "identical": hashes[0] == hashes[1],
                    "bytes": [path.stat().st_size for path in paths],
                }
                comparisons.append(comparison)
            report["comparisons"] = comparisons
            if not all(comparison["identical"] for comparison in comparisons):
                raise ReproducibilityError("one or more artifacts differ between clean builds")
            image_comparisons = [
                comparison
                for comparison in comparisons
                if comparison["path"].endswith("northstar.img")
            ]
            if image_comparisons:
                report["image_sha256"] = image_comparisons[0]["build_1_sha256"]
            report["passed"] = True
            exit_code = 0
    except (OSError, ReproducibilityError) as error:
        report["error"] = str(error)
        print("check_reproducible: FAIL: {}".format(error), file=sys.stderr)
    report_path = artifacts_dir / "report.json"
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if exit_code == 0:
        print("reproducible: {}".format(display_path(report_path, source)))
        for comparison in report["comparisons"]:
            print("{}  {}".format(comparison["build_1_sha256"], comparison["path"]))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
