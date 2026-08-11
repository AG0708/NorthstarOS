#!/usr/bin/env python3
"""Bind the supported malformed-input gates to executable host and guest results."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import sys
from typing import Dict, Iterable, Optional, Sequence


ROOT = pathlib.Path(__file__).resolve().parents[1]
CANONICAL_SCENARIOS = {
    "m0_stage1_boot",
    "m1_long_mode_boot",
    "m2_memory_scheduler",
    "m3_ring3_processes",
    "m4_nsfs_persistence",
    "m4_user_environment",
    "m5_network_interop",
}


class CoverageError(RuntimeError):
    pass


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: pathlib.Path) -> Dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise CoverageError("cannot read {}: {}".format(path, error)) from error
    if not isinstance(value, dict):
        raise CoverageError("{} does not contain a JSON object".format(path))
    return value


def write_json(path: pathlib.Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name("." + path.name + ".tmp-{}".format(os.getpid()))
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(str(temporary), str(path))


def find_record(
    records: Iterable[object], test_id: str, mode: str
) -> Dict[str, object]:
    matches = [
        record
        for record in records
        if isinstance(record, dict)
        and record.get("id") == test_id
        and record.get("mode") == mode
    ]
    if len(matches) != 1 or matches[0].get("passed") is not True:
        raise CoverageError(
            "host result lacks one passing {} [{}] record".format(test_id, mode)
        )
    return matches[0]


def host_log(
    host_root: pathlib.Path,
    records: Iterable[object],
    test_id: str,
    mode: str,
    pattern: str,
) -> Dict[str, object]:
    record = find_record(records, test_id, mode)
    run = record.get("run")
    if not isinstance(run, dict) or not isinstance(run.get("log"), str):
        raise CoverageError("{} [{}] has no executable run log".format(test_id, mode))
    path = host_root / run["log"]
    text = path.read_text(encoding="utf-8", errors="replace")
    if re.search(pattern, text, flags=re.MULTILINE) is None:
        raise CoverageError(
            "{} [{}] log lacks required result {!r}".format(test_id, mode, pattern)
        )
    return {
        "test": test_id,
        "mode": mode,
        "log": path.relative_to(host_root).as_posix(),
        "log_sha256": sha256_file(path),
        "required_pattern": pattern,
    }


def scenario_record(
    integration: Dict[str, object], scenario_id: str
) -> Dict[str, object]:
    results = integration.get("results")
    if not isinstance(results, list):
        raise CoverageError("integration summary has no scenario results")
    matches = [
        result
        for result in results
        if isinstance(result, dict) and result.get("id") == scenario_id
    ]
    if len(matches) != 1 or matches[0].get("passed") is not True:
        raise CoverageError("integration result lacks passing {}".format(scenario_id))
    return matches[0]


def guest_log(
    integration_root: pathlib.Path,
    record: Dict[str, object],
    patterns: Sequence[str],
) -> Dict[str, object]:
    relative = record.get("combined_serial")
    if not isinstance(relative, str):
        raise CoverageError("{} has no combined serial log".format(record.get("id")))
    path = integration_root / relative
    text = path.read_text(encoding="utf-8", errors="replace")
    missing = [
        pattern
        for pattern in patterns
        if re.search(pattern, text, flags=re.MULTILINE) is None
    ]
    if missing:
        raise CoverageError(
            "{} guest log lacks {}".format(record.get("id"), missing)
        )
    return {
        "scenario": record.get("id"),
        "serial": relative,
        "serial_sha256": sha256_file(path),
        "required_patterns": list(patterns),
    }


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=pathlib.Path, required=True)
    parser.add_argument("--host-summary", type=pathlib.Path, required=True)
    parser.add_argument("--integration-summary", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    return parser.parse_args(argv)


def main() -> int:
    arguments = parse_args()
    try:
        if not arguments.image.is_file():
            raise CoverageError("image does not exist: {}".format(arguments.image))
        image_hash = sha256_file(arguments.image)
        host = load_json(arguments.host_summary)
        integration = load_json(arguments.integration_summary)
        if (
            host.get("schema") != "northstar.host-test-summary.v1"
            or host.get("passed") is not True
            or host.get("completed_test_executions")
            != host.get("expected_test_executions")
        ):
            raise CoverageError("host summary is not complete and passing")
        if (
            integration.get("schema") != "northstar.integration-summary.v1"
            or integration.get("passed") is not True
            or integration.get("image_sha256") != image_hash
            or set(integration.get("selected", [])) != CANONICAL_SCENARIOS
        ):
            raise CoverageError(
                "integration summary is not the passing canonical M0-M5 suite"
            )

        host_root = arguments.host_summary.parent
        integration_root = arguments.integration_summary.parent
        records = host.get("records")
        if not isinstance(records, list):
            raise CoverageError("host summary has no records")
        m3 = scenario_record(integration, "m3_ring3_processes")
        m4 = scenario_record(integration, "m4_nsfs_persistence")
        m5 = scenario_record(integration, "m5_network_interop")

        peer_results = m5.get("network_peer_results")
        if not isinstance(peer_results, list) or not peer_results:
            raise CoverageError("M5 has no independent Ethernet peer result")
        if not all(
            isinstance(peer, dict)
            and peer.get("passed") is True
            and "malformed_injected" in peer.get("event_names", [])
            and isinstance(peer.get("pcap_sha256"), str)
            and peer.get("pcap_packets", 0) > 0
            for peer in peer_results
        ):
            raise CoverageError("M5 peer did not prove malformed injection and PCAP")

        cases = {
            "elf": {
                "passed": True,
                "scope": "hosted production ELF loader",
                "oracles": [
                    host_log(
                        host_root,
                        records,
                        "test_proc_elf",
                        mode,
                        r"^ok 2 - malformed ELF metadata rejection$",
                    )
                    for mode in ("strict", "ubsan")
                ],
            },
            "syscall": {
                "passed": True,
                "scope": "hosted dispatcher plus Ring-3 fault containment",
                "oracles": [
                    *[
                        host_log(
                            host_root,
                            records,
                            "test_proc_syscall",
                            mode,
                            r"^ok 2 - invalid user pointers return EFAULT$",
                        )
                        for mode in ("strict", "ubsan")
                    ],
                    guest_log(
                        integration_root,
                        m3,
                        (
                            r"^ok 4 - user-fault-contained$",
                            r"^ok 5 - privileged-fault-contained$",
                        ),
                    ),
                ],
            },
            "filesystem": {
                "passed": True,
                "scope": "kernel parser, journal, and independent host checker",
                "oracles": [
                    *[
                        host_log(
                            host_root,
                            records,
                            "test_fs_nsfs",
                            mode,
                            r"^ok 2 - corrupt superblock and inode rejection$",
                        )
                        for mode in ("strict", "ubsan")
                    ],
                    host_log(
                        host_root,
                        records,
                        "test_fs_tools",
                        "script",
                        r"^ok - independent fsck rejects dual-superblock corruption$",
                    ),
                    guest_log(
                        integration_root,
                        m4,
                        (r"^ok 3 - nsfs rejects corrupt metadata$",),
                    ),
                ],
            },
            "network": {
                "passed": True,
                "scope": "hosted TCP parser plus RTL8139 guest/peer interop",
                "oracles": [
                    *[
                        host_log(
                            host_root,
                            records,
                            "test_net_tcp",
                            mode,
                            r"^ok 5 - malformed header and checksum rejection$",
                        )
                        for mode in ("strict", "ubsan")
                    ],
                    host_log(
                        host_root,
                        records,
                        "test_net_peer",
                        "script",
                        r"^test_net_peer: PASS$",
                    ),
                    guest_log(
                        integration_root,
                        m5,
                        (
                            r"^# NS_TEST net\.ring3-usercopy PASS$",
                            r"^NS:NET:ROBUST .*bad_checksum=[1-9][0-9]* .*dropped=[1-9][0-9]*$",
                        ),
                    ),
                    {
                        "peer_runs": len(peer_results),
                        "pcap_sha256": [peer["pcap_sha256"] for peer in peer_results],
                        "required_event": "malformed_injected",
                    },
                ],
            },
        }
        result = {
            "schema": "northstar.adversarial-coverage.v1",
            "passed": all(case["passed"] for case in cases.values()),
            "image_sha256": image_hash,
            "host_summary_sha256": sha256_file(arguments.host_summary),
            "integration_summary_sha256": sha256_file(
                arguments.integration_summary
            ),
            "cases": cases,
            "limitations": [
                "Malformed ELF coverage executes the production loader in hosted strict and UBSan modes; it is not a guest fuzzing campaign.",
                "The network gate covers the preregistered malformed checksum frames; it is not protocol-complete fuzzing.",
            ],
        }
        write_json(arguments.output, result)
    except (CoverageError, OSError) as error:
        print("check_adversarial_results: error: {}".format(error), file=sys.stderr)
        return 2
    print("adversarial coverage: {}".format(arguments.output))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
