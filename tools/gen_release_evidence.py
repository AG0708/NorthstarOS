#!/usr/bin/env python3
"""Generate a content-addressed NorthstarOS release evidence manifest."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys
from typing import Dict, Iterable, List, Optional, Sequence


DEFAULT_EPOCH = 1_700_000_000
EXCLUDED_PARTS = {".git", ".DS_Store", "artifacts", "__pycache__", ".pytest_cache"}
DEFAULT_CROSS = os.environ.get("CROSS", "x86_64-elf-")
TOOL_COMMANDS = {
    "python": [sys.executable, "--version"],
    "make": ["make", "--version"],
    "nasm": ["nasm", "-v"],
    "cross_gcc": [DEFAULT_CROSS + "gcc", "--version"],
    "cross_ld": [DEFAULT_CROSS + "ld", "--version"],
    "qemu": ["qemu-system-x86_64", "--version"],
}

IMAGE_BOUND_RESULT_SCHEMAS = {
    "northstar.integration-summary.v1",
    "northstar.repetition-summary.v1",
    "northstar.journal-matrix.v1",
    "northstar.reproducibility.v1",
    "northstar.adversarial-coverage.v1",
}
INTERACTIVE_RESULT_SCHEMA = "northstar.interactive-result.v1"
HOST_RESULT_SCHEMA = "northstar.host-test-summary.v1"
REQUIRED_RELEASE_SCHEMAS = IMAGE_BOUND_RESULT_SCHEMAS | {INTERACTIVE_RESULT_SCHEMA}
CANONICAL_SCENARIOS = {
    "m0_stage1_boot",
    "m1_long_mode_boot",
    "m2_memory_scheduler",
    "m3_ring3_processes",
    "m4_nsfs_persistence",
    "m4_user_environment",
    "m5_network_interop",
}


class EvidenceError(RuntimeError):
    pass


def hash_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_files(root: pathlib.Path) -> Iterable[pathlib.Path]:
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        if any(part in EXCLUDED_PARTS for part in relative.parts):
            continue
        if relative.parts and relative.parts[0].startswith("build-"):
            continue
        if relative.parts and relative.parts[0] == "build" and path.suffix != ".mk":
            continue
        if path.suffix == ".pyc":
            continue
        yield path


def source_tree(root: pathlib.Path) -> Dict[str, object]:
    digest = hashlib.sha256()
    count = 0
    for path in source_files(root):
        relative = path.relative_to(root).as_posix()
        file_hash = hash_file(path)
        digest.update(relative.encode("utf-8"))
        digest.update(b"\x00")
        digest.update(file_hash.encode("ascii"))
        digest.update(b"\n")
        count += 1
    return {"sha256": digest.hexdigest(), "files": count}


def capture(argv: Sequence[str], cwd: pathlib.Path) -> Dict[str, object]:
    executable = shutil.which(argv[0]) if os.path.sep not in argv[0] else argv[0]
    recorded_argv = [pathlib.Path(argv[0]).name] + list(argv[1:])
    if executable is None:
        return {
            "available": False,
            "argv": recorded_argv,
            "executable": pathlib.Path(argv[0]).name,
            "version": None,
        }
    try:
        completed = subprocess.run(
            [executable] + list(argv[1:]),
            cwd=str(cwd),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return {
            "available": False,
            "argv": recorded_argv,
            "executable": pathlib.Path(argv[0]).name,
            "version": None,
            "error": type(error).__name__,
        }
    text = completed.stdout.decode("utf-8", errors="replace").strip()
    return {
        "available": completed.returncode == 0,
        "argv": recorded_argv,
        "executable": pathlib.Path(argv[0]).name,
        "version": text.splitlines()[0] if text else "",
        "returncode": completed.returncode,
    }


def git_state(root: pathlib.Path) -> Dict[str, object]:
    def git(*arguments: str) -> Optional[str]:
        try:
            completed = subprocess.run(
                ["git", "-C", str(root)] + list(arguments),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                timeout=10,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired):
            return None
        if completed.returncode != 0:
            return None
        return completed.stdout.decode("utf-8", errors="replace").rstrip("\n")

    top_level = git("rev-parse", "--show-toplevel")
    commit = git("rev-parse", "HEAD")
    status = git("status", "--porcelain=v1", "--untracked-files=all")
    prefix = None
    if top_level:
        try:
            prefix = root.resolve().relative_to(pathlib.Path(top_level).resolve()).as_posix()
        except ValueError:
            prefix = None
    return {
        "available": commit is not None,
        "commit": commit,
        "repository": pathlib.Path(top_level).name if top_level else None,
        "project_prefix": prefix,
        "clean": status == "" if status is not None else None,
        "status_sha256": hashlib.sha256((status or "").encode("utf-8")).hexdigest(),
        "status_entries": 0 if not status else len(status.splitlines()),
    }


def load_json(path: pathlib.Path) -> Dict[str, object]:
    try:
        with path.open("r", encoding="utf-8") as stream:
            value = json.load(stream)
    except (OSError, json.JSONDecodeError) as error:
        raise EvidenceError("cannot read JSON {}: {}".format(path, error))
    if not isinstance(value, dict):
        raise EvidenceError("{} must contain a JSON object".format(path))
    return value


def evidence_files(paths: Sequence[pathlib.Path], output: pathlib.Path) -> List[pathlib.Path]:
    files: List[pathlib.Path] = []
    output_resolved = output.resolve()
    for candidate in paths:
        if candidate.is_file():
            discovered = [candidate]
        elif candidate.is_dir():
            discovered = [
                path
                for path in candidate.rglob("*")
                if path.is_file()
                and path.suffix.lower()
                in {".json", ".jsonl", ".log", ".pcap", ".sha256", ".tap", ".txt"}
            ]
        else:
            raise EvidenceError("evidence path does not exist: {}".format(candidate))
        for path in discovered:
            if path.resolve() != output_resolved:
                files.append(path)
    return sorted(set(path.resolve() for path in files), key=str)


def display_path(path: pathlib.Path, source: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(source.resolve()).as_posix()
    except ValueError:
        return path.name


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--image", type=pathlib.Path, default=pathlib.Path("build/northstar.img"))
    parser.add_argument("--image-manifest", type=pathlib.Path, default=pathlib.Path("build/image-layout.json"))
    parser.add_argument("--boot-layout", type=pathlib.Path, default=pathlib.Path("build/generated/boot_layout.json"))
    parser.add_argument("--interactive-image", type=pathlib.Path)
    parser.add_argument("--sbom", type=pathlib.Path)
    parser.add_argument("--evidence", type=pathlib.Path, action="append", default=[])
    parser.add_argument("--test-result", type=pathlib.Path, action="append", default=[])
    parser.add_argument("--host-result", type=pathlib.Path, action="append", default=[])
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("artifacts/release/evidence.json"))
    parser.add_argument("--source-date-epoch", type=int, default=None)
    parser.add_argument("--require-clean", action="store_true")
    parser.add_argument("--strict-tools", action="store_true")
    parser.add_argument("--require-release-gates", action="store_true")
    arguments = parser.parse_args()
    if arguments.source_date_epoch is None:
        try:
            arguments.source_date_epoch = int(os.environ.get("SOURCE_DATE_EPOCH", DEFAULT_EPOCH))
        except ValueError:
            parser.error("SOURCE_DATE_EPOCH must be an integer")
    if arguments.source_date_epoch < 0:
        parser.error("--source-date-epoch must be non-negative")
    return arguments


def main() -> int:
    arguments = parse_args()
    source = arguments.source.resolve()
    image = arguments.image if arguments.image.is_absolute() else source / arguments.image
    image_manifest_path = (
        arguments.image_manifest if arguments.image_manifest.is_absolute() else source / arguments.image_manifest
    )
    boot_layout_path = arguments.boot_layout if arguments.boot_layout.is_absolute() else source / arguments.boot_layout
    interactive_image = None
    if arguments.interactive_image is not None:
        interactive_image = (
            arguments.interactive_image
            if arguments.interactive_image.is_absolute()
            else source / arguments.interactive_image
        )
    sbom_path = None
    if arguments.sbom is not None:
        sbom_path = (
            arguments.sbom
            if arguments.sbom.is_absolute()
            else source / arguments.sbom
        )
    output = arguments.output if arguments.output.is_absolute() else source / arguments.output
    evidence_inputs = [path if path.is_absolute() else source / path for path in arguments.evidence]
    test_results = [path if path.is_absolute() else source / path for path in arguments.test_result]
    host_results = [path if path.is_absolute() else source / path for path in arguments.host_result]
    try:
        if not source.is_dir():
            raise EvidenceError("source directory does not exist: {}".format(source))
        if not image.is_file():
            raise EvidenceError("release image does not exist: {}".format(image))
        image_hash = hash_file(image)
        image_manifest = load_json(image_manifest_path)
        recorded_hash = image_manifest.get("image", {}).get("sha256") if isinstance(image_manifest.get("image"), dict) else None
        if recorded_hash != image_hash:
            raise EvidenceError("release image hash does not match image-layout manifest")
        boot_layout = load_json(boot_layout_path)
        if boot_layout.get("schema") != "northstar.boot-layout.v1":
            raise EvidenceError("boot layout has an unknown schema")
        interactive_image_hash = None
        if interactive_image is not None:
            if not interactive_image.is_file():
                raise EvidenceError(
                    "interactive profile image does not exist: {}".format(
                        interactive_image
                    )
                )
            interactive_image_hash = hash_file(interactive_image)

        state = git_state(source)
        if arguments.require_clean and state["clean"] is not True:
            raise EvidenceError("--require-clean requested but the Git worktree is not clean")
        tools = {name: capture(command, source) for name, command in TOOL_COMMANDS.items()}
        if arguments.strict_tools:
            missing = [name for name, value in tools.items() if not value["available"]]
            if missing:
                raise EvidenceError("required tools unavailable: {}".format(missing))

        sbom_record = None
        if sbom_path is not None:
            sbom = load_json(sbom_path)
            packages = sbom.get("packages")
            if (
                sbom.get("spdxVersion") != "SPDX-2.3"
                or sbom.get("dataLicense") != "CC0-1.0"
                or sbom.get("documentDescribes")
                != ["SPDXRef-Package-NorthstarOS"]
                or not isinstance(packages, list)
                or len(packages) != 1
                or not isinstance(packages[0], dict)
                or packages[0].get("SPDXID")
                != "SPDXRef-Package-NorthstarOS"
                or packages[0].get("licenseDeclared") != "MIT"
                or not isinstance(sbom.get("files"), list)
                or not sbom["files"]
            ):
                raise EvidenceError("SBOM is not the expected SPDX 2.3 source inventory")
            if state.get("commit") is not None and not str(
                packages[0].get("downloadLocation", "")
            ).endswith("@" + str(state["commit"])):
                raise EvidenceError("SBOM is not bound to the release Git commit")
            sbom_record = {
                "path": display_path(sbom_path, source),
                "sha256": hash_file(sbom_path),
                "spdx_version": "SPDX-2.3",
                "files": len(sbom["files"]),
            }
        elif arguments.require_release_gates:
            raise EvidenceError("--require-release-gates requires --sbom")

        result_records = []
        observed_schemas = set()
        for result_path in test_results:
            result = load_json(result_path)
            if result.get("passed") is not True:
                raise EvidenceError("test result does not record passed=true: {}".format(result_path))
            schema = result.get("schema")
            if schema not in IMAGE_BOUND_RESULT_SCHEMAS and schema != INTERACTIVE_RESULT_SCHEMA:
                raise EvidenceError(
                    "test result has unknown or non-image-bound schema {}: {}".format(
                        schema, result_path
                    )
                )
            if schema == INTERACTIVE_RESULT_SCHEMA:
                if interactive_image_hash is None:
                    raise EvidenceError(
                        "interactive result requires --interactive-image: {}".format(
                            result_path
                        )
                    )
                if result.get("image_sha256") != interactive_image_hash:
                    raise EvidenceError(
                        "interactive result is not bound to the interactive image: {}".format(
                            result_path
                        )
                    )
                if result.get("canonical_image_sha256") != image_hash:
                    raise EvidenceError(
                        "interactive result is not bound to the canonical release image: {}".format(
                            result_path
                        )
                    )
                markers = result.get("markers")
                if (
                    not isinstance(markers, dict)
                    or not markers
                    or not all(value is True for value in markers.values())
                    or result.get("network_enabled") is not False
                ):
                    raise EvidenceError(
                        "interactive result does not prove every PTY marker: {}".format(
                            result_path
                        )
                    )
                bound_hash = interactive_image_hash
            elif result.get("image_sha256") != image_hash:
                raise EvidenceError(
                    "test result is not bound to the release image SHA-256: {}".format(
                        result_path
                    )
                )
            else:
                bound_hash = image_hash
            if schema in {
                "northstar.integration-summary.v1",
                "northstar.repetition-summary.v1",
            }:
                selected = result.get("selected")
                scenario_results = result.get("results")
                required = result.get("required_cold_boots")
                completed = result.get("completed_cold_boots")
                if (
                    not isinstance(selected, list)
                    or set(selected) != CANONICAL_SCENARIOS
                    or not isinstance(scenario_results, list)
                    or len(scenario_results) != len(CANONICAL_SCENARIOS)
                    or not all(
                        isinstance(item, dict) and item.get("passed") is True
                        for item in scenario_results
                    )
                ):
                    raise EvidenceError(
                        "integration result is not the complete canonical M0-M5 suite: {}".format(
                            result_path
                        )
                    )
                network_results = [
                    item
                    for item in scenario_results
                    if item.get("id") == "m5_network_interop"
                ]
                if len(network_results) != 1:
                    raise EvidenceError(
                        "integration result has no unique M5 network gate: {}".format(
                            result_path
                        )
                    )
                peer_results = network_results[0].get("network_peer_results")
                if (
                    not isinstance(peer_results, list)
                    or not peer_results
                    or not all(
                        isinstance(peer, dict)
                        and peer.get("passed") is True
                        and isinstance(peer.get("pcap_sha256"), str)
                        and peer.get("pcap_packets", 0) > 0
                        for peer in peer_results
                    )
                ):
                    raise EvidenceError(
                        "integration result has no passing PCAP-backed network peer: {}".format(
                            result_path
                        )
                    )
            if schema == "northstar.repetition-summary.v1":
                if (
                    result.get("repeat_requested") != 100
                    or required != 700
                    or completed != required
                ):
                    raise EvidenceError(
                        "repetition result is not the exact 100 x 7 cold-boot gate: {}".format(
                            result_path
                        )
                    )
                if any(
                    item.get("required_boots") != 100
                    or item.get("completed_boots") != 100
                    for item in scenario_results
                ):
                    raise EvidenceError(
                        "repetition result does not contain 100 boots for every scenario: {}".format(
                            result_path
                        )
                    )
            elif schema == "northstar.integration-summary.v1":
                if (
                    result.get("repeat_requested") != 1
                    or required != 10
                    or completed != required
                ):
                    raise EvidenceError(
                        "integration result is not the exact single canonical M0-M5 gate: {}".format(
                            result_path
                        )
                    )
            elif schema == "northstar.journal-matrix.v1":
                cases = result.get("cases")
                if (
                    not isinstance(cases, list)
                    or len(cases) != 4
                    or not all(
                        isinstance(case, dict) and case.get("passed") is True
                        for case in cases
                    )
                ):
                    raise EvidenceError(
                        "journal result does not prove all four cut points: {}".format(
                            result_path
                        )
                    )
            elif schema == "northstar.reproducibility.v1":
                comparisons = result.get("comparisons")
                if (
                    not isinstance(comparisons, list)
                    or not comparisons
                    or not all(
                        isinstance(item, dict) and item.get("identical") is True
                        for item in comparisons
                    )
                ):
                    raise EvidenceError(
                        "reproducibility result has no complete identical comparison set: {}".format(
                            result_path
                        )
                    )
            elif schema == "northstar.adversarial-coverage.v1":
                cases = result.get("cases")
                expected = {"elf", "syscall", "filesystem", "network"}
                if (
                    not isinstance(cases, dict)
                    or set(cases) != expected
                    or not all(
                        isinstance(case, dict) and case.get("passed") is True
                        for case in cases.values()
                    )
                ):
                    raise EvidenceError(
                        "adversarial result does not prove the four supported cases: {}".format(
                            result_path
                        )
                    )
            observed_schemas.add(schema)
            result_records.append(
                {
                    "path": display_path(result_path, source),
                    "schema": schema,
                    "image_sha256": bound_hash,
                    "sha256": hash_file(result_path),
                }
            )
        if arguments.require_release_gates:
            missing_schemas = sorted(REQUIRED_RELEASE_SCHEMAS - observed_schemas)
            if missing_schemas:
                raise EvidenceError(
                    "required release result schemas are missing: {}".format(
                        missing_schemas
                    )
                )

        host_records = []
        for result_path in host_results:
            result = load_json(result_path)
            if (
                result.get("schema") != HOST_RESULT_SCHEMA
                or result.get("passed") is not True
                or result.get("completed_test_executions")
                != result.get("expected_test_executions")
            ):
                raise EvidenceError(
                    "host result is not a complete passing host suite: {}".format(
                        result_path
                    )
                )
            host_records.append(
                {
                    "path": display_path(result_path, source),
                    "schema": HOST_RESULT_SCHEMA,
                    "sha256": hash_file(result_path),
                }
            )

        files = evidence_files(evidence_inputs + test_results + host_results, output)
        evidence_records = [
            {
                "path": display_path(path, source),
                "bytes": path.stat().st_size,
                "sha256": hash_file(path),
            }
            for path in files
        ]
        generated = dt.datetime.fromtimestamp(arguments.source_date_epoch, tz=dt.timezone.utc)
        manifest = {
            "schema": "northstar.release-evidence.v1",
            "generated_from_source_date_epoch": arguments.source_date_epoch,
            "generated_at": generated.isoformat().replace("+00:00", "Z"),
            "source": {"tree": source_tree(source), "git": state},
            "platform": {
                "system": platform.system(),
                "release": platform.release(),
                "machine": platform.machine(),
            },
            "tools": tools,
            "release_image": {
                "path": display_path(image, source),
                "bytes": image.stat().st_size,
                "sha256": image_hash,
                "layout_manifest_sha256": hash_file(image_manifest_path),
                "boot_layout_sha256": hash_file(boot_layout_path),
            },
            "passing_test_results": result_records,
            "passing_host_results": host_records,
            "evidence_files": evidence_records,
            "limitations": [
                "QEMU pc-i440fx-7.2/qemu64 is the verified hardware target.",
                "Passing gates do not establish POSIX conformance, SMP scalability, or production security.",
            ],
        }
        if sbom_record is not None:
            manifest["sbom"] = sbom_record
        if interactive_image is not None and interactive_image_hash is not None:
            manifest["interactive_profile_image"] = {
                "path": display_path(interactive_image, source),
                "bytes": interactive_image.stat().st_size,
                "sha256": interactive_image_hash,
                "network_enabled": False,
            }
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        manifest_hash = hash_file(output)
        hash_path = output.with_suffix(output.suffix + ".sha256")
        hash_path.write_text("{}  {}\n".format(manifest_hash, output.name), encoding="ascii")
    except (OSError, EvidenceError) as error:
        print("gen_release_evidence: error: {}".format(error), file=sys.stderr)
        return 2
    print("{}  {}".format(manifest_hash, display_path(output, source)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
