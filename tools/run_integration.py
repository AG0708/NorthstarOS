#!/usr/bin/env python3
"""Execute progressive NorthstarOS black-box scenarios under headless QEMU."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import http.client
import json
import os
import pathlib
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time
from typing import Dict, List, Sequence


PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[1]
HOST_TESTS = PROJECT_ROOT / "tests" / "host"
sys.path.insert(0, str(HOST_TESTS))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from test_support import Scenario, ScenarioError, TapError, load_scenarios, parse_tap  # noqa: E402
from run_qemu import PASS_DEBUG_EXIT, RunResult, run_qemu, sha256_file  # noqa: E402


RESERVED_QEMU_OPTIONS = {
    "-boot",
    "-chardev",
    "-daemonize",
    "-debugcon",
    "-drive",
    "-hda",
    "-accel",
    "-cpu",
    "-display",
    "-machine",
    "-monitor",
    "-netdev",
    "-nic",
    "-no-reboot",
    "-no-shutdown",
    "-pidfile",
    "-qmp",
    "-rtc",
    "-serial",
    "-smp",
}

NETWORK_PEER_REQUIRED_EVENTS = (
    "dhcp_offer",
    "dhcp_ack",
    "malformed_injected",
    "arp_reply",
    "dns_answer",
    "icmp_echo",
    "udp_echo",
    "tcp_syn",
    "tcp_established",
    "tcp_injected_drop",
    "http_response",
)


def recorded_argv(argv: Sequence[str]) -> List[str]:
    recorded: List[str] = []
    root = PROJECT_ROOT.resolve()
    for item in argv:
        candidate = pathlib.Path(item)
        if candidate.is_absolute():
            try:
                item = candidate.resolve().relative_to(root).as_posix()
            except ValueError:
                item = candidate.name
        recorded.append(item)
    return recorded


def runtime_path(path: pathlib.Path) -> str:
    """Prefer a project-relative path for child processes launched from ROOT."""

    try:
        return path.resolve().relative_to(PROJECT_ROOT.resolve()).as_posix()
    except ValueError:
        return str(path.resolve())


def validate_scenario_qemu_args(arguments: Sequence[str]) -> None:
    for argument in arguments:
        option = argument.split("=", 1)[0]
        if option in RESERVED_QEMU_OPTIONS:
            raise ScenarioError("scenario may not override evidence-critical QEMU option {}".format(option))
    for index, argument in enumerate(arguments):
        if argument == "-device" and index + 1 < len(arguments):
            if arguments[index + 1].split(",", 1)[0] == "isa-debug-exit":
                raise ScenarioError("scenario may not replace the isa-debug-exit device")


def network_arguments(
    scenario: Scenario,
    *,
    qemu_endpoint: str = "127.0.0.1:10000",
    peer_endpoint: str = "127.0.0.1:10001",
) -> List[str]:
    if not scenario.network:
        return []
    # An explicit HTTP host probe uses slirp/hostfwd.  The canonical M5 gate has
    # no http_probe and instead uses a datagram Ethernet tunnel to net_peer.py,
    # preserving frames for an external packet-level oracle.
    probe = getattr(scenario, "http_probe", None)
    if probe is not None:
        netdev = "user,id=northstar-net,net=10.0.2.0/24,dhcpstart=10.0.2.15"
        netdev += ",hostfwd=tcp:{}:{}-:{}".format(
            probe.host, probe.host_port, probe.guest_port
        )
    else:
        netdev = "socket,id=northstar-net,udp={},localaddr={}".format(
            peer_endpoint, qemu_endpoint
        )
    return [
        "-netdev",
        netdev,
        "-device",
        "rtl8139,netdev=northstar-net,mac=52:54:00:12:34:57",
    ]


def allocate_udp_endpoints() -> tuple[str, str]:
    sockets = []
    try:
        for _ in range(2):
            candidate = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            candidate.bind(("127.0.0.1", 0))
            sockets.append(candidate)
        qemu = "127.0.0.1:{}".format(sockets[0].getsockname()[1])
        peer = "127.0.0.1:{}".format(sockets[1].getsockname()[1])
        return qemu, peer
    finally:
        for candidate in sockets:
            candidate.close()


def terminate_peer(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=2.0)
        return
    except (ProcessLookupError, subprocess.TimeoutExpired):
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def start_network_peer(
    scenario_root: pathlib.Path, attempt: int, timeout_seconds: float
) -> tuple[subprocess.Popen[bytes], Dict[str, object], str, str]:
    qemu_endpoint, peer_endpoint = allocate_udp_endpoints()
    events_path = (scenario_root / "peer-{:02d}.jsonl".format(attempt)).resolve()
    pcap_path = (scenario_root / "peer-{:02d}.pcap".format(attempt)).resolve()
    stderr_path = (scenario_root / "peer-{:02d}.stderr.log".format(attempt)).resolve()
    command = [
        sys.executable,
        str(PROJECT_ROOT / "tools" / "net_peer.py"),
        "--bind",
        peer_endpoint,
        "--qemu",
        qemu_endpoint,
        "--events",
        str(events_path),
        "--pcap",
        str(pcap_path),
        "--timeout",
        str(timeout_seconds),
        "--drop-first-tcp-data",
        "--duplicate-tcp-response",
        "--reorder-tcp-response",
        "--inject-malformed",
    ]
    for event in NETWORK_PEER_REQUIRED_EVENTS:
        command.extend(("--require", event))
    record: Dict[str, object] = {
        "attempt": attempt,
        "passed": False,
        "command": recorded_argv(command),
        "qemu_endpoint": qemu_endpoint,
        "peer_endpoint": peer_endpoint,
        "events": events_path.name,
        "pcap": pcap_path.name,
        "stderr": stderr_path.name,
        "required_events": list(NETWORK_PEER_REQUIRED_EVENTS),
        "_events_path": str(events_path),
        "_pcap_path": str(pcap_path),
    }
    with stderr_path.open("wb") as stderr_stream:
        process = subprocess.Popen(
            command,
            cwd=str(PROJECT_ROOT),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=stderr_stream,
            start_new_session=True,
        )
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise ScenarioError("deterministic network peer exited before readiness")
        if events_path.is_file() and "\"event\": \"peer_ready\"" in events_path.read_text(
            encoding="utf-8", errors="replace"
        ):
            return process, record, qemu_endpoint, peer_endpoint
        time.sleep(0.025)
    terminate_peer(process)
    raise ScenarioError("deterministic network peer did not become ready")


def finish_network_peer(
    process: subprocess.Popen[bytes], record: Dict[str, object]
) -> None:
    try:
        returncode = process.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        terminate_peer(process)
        returncode = process.returncode
    record["returncode"] = returncode
    events_path = pathlib.Path(str(record.pop("_events_path")))
    pcap_path = pathlib.Path(str(record.pop("_pcap_path")))
    names: List[str] = []
    parse_error = None
    try:
        for line in events_path.read_text(encoding="utf-8").splitlines():
            value = json.loads(line)
            if not isinstance(value, dict) or not isinstance(value.get("event"), str):
                raise ValueError("event record is not an object with a name")
            names.append(value["event"])
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
        parse_error = str(error)
    required = set(str(item) for item in record["required_events"])
    missing = sorted(required - set(names))
    record["event_count"] = len(names)
    record["event_names"] = sorted(set(names))
    record["missing_events"] = missing
    record["ethernet_rx_frames"] = names.count("ethernet_rx")
    record["ethernet_tx_frames"] = names.count("ethernet_tx")
    record["parse_error"] = parse_error
    pcap_error = None
    pcap_packets = 0
    pcap_ethertypes: Dict[str, int] = {}
    if pcap_path.is_file():
        record["pcap_bytes"] = pcap_path.stat().st_size
        record["pcap_sha256"] = sha256_file(pcap_path)
        try:
            data = pcap_path.read_bytes()
            if len(data) < 24:
                raise ValueError("truncated PCAP global header")
            magic, major, minor, _, _, snaplen, linktype = struct.unpack_from(
                "<IHHIIII", data, 0
            )
            if magic != 0xA1B23C4D or (major, minor) != (2, 4) or linktype != 1:
                raise ValueError("PCAP is not nanosecond Ethernet version 2.4")
            cursor = 24
            while cursor < len(data):
                if len(data) - cursor < 16:
                    raise ValueError("truncated PCAP packet header")
                _, nanoseconds, captured, original = struct.unpack_from(
                    "<IIII", data, cursor
                )
                cursor += 16
                if nanoseconds >= 1_000_000_000 or captured > original or captured > snaplen:
                    raise ValueError("invalid PCAP packet metadata")
                if captured > len(data) - cursor:
                    raise ValueError("truncated PCAP packet bytes")
                frame = data[cursor:cursor + captured]
                cursor += captured
                if len(frame) < 14:
                    raise ValueError("captured frame is shorter than Ethernet")
                ethertype = "0x{:04x}".format(struct.unpack_from("!H", frame, 12)[0])
                pcap_ethertypes[ethertype] = pcap_ethertypes.get(ethertype, 0) + 1
                pcap_packets += 1
            if cursor != len(data):
                raise ValueError("PCAP has trailing bytes")
        except (OSError, ValueError, struct.error) as error:
            pcap_error = str(error)
    else:
        record["pcap_bytes"] = 0
        record["pcap_sha256"] = None
    record["pcap_error"] = pcap_error
    record["pcap_packets"] = pcap_packets
    record["pcap_ethertypes"] = pcap_ethertypes
    record["passed"] = (
        returncode == 0
        and parse_error is None
        and not missing
        and names.count("peer_complete") == 1
        and record["ethernet_rx_frames"] > 0
        and record["ethernet_tx_frames"] > 0
        and record["pcap_bytes"] > 24
        and pcap_error is None
        and pcap_packets == record["ethernet_rx_frames"] + record["ethernet_tx_frames"]
        and pcap_ethertypes.get("0x0800", 0) > 0
        and pcap_ethertypes.get("0x0806", 0) > 0
    )


def make_http_probe(probe, record: Dict[str, object]):
    """Return a bounded callback that independently validates guest HTTP."""

    def execute() -> None:
        deadline = time.monotonic() + 8.0
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            connection = http.client.HTTPConnection(probe.host, probe.host_port, timeout=1.0)
            try:
                connection.request("GET", probe.path, headers={"Host": "northstar.test", "Connection": "close"})
                response = connection.getresponse()
                body = response.read(1024 * 1024 + 1)
                if len(body) > 1024 * 1024:
                    raise RuntimeError("HTTP probe response exceeds 1 MiB")
                body_hash = hashlib.sha256(body).hexdigest()
                record.update(
                    {
                        "status": response.status,
                        "body_bytes": len(body),
                        "body_sha256": body_hash,
                    }
                )
                if response.status != probe.expected_status:
                    raise RuntimeError(
                        "HTTP status {} != {}".format(response.status, probe.expected_status)
                    )
                if body_hash != probe.expected_body_sha256:
                    raise RuntimeError(
                        "HTTP body SHA-256 {} != {}".format(body_hash, probe.expected_body_sha256)
                    )
                record["passed"] = True
                return
            except (OSError, http.client.HTTPException) as error:
                last_error = error
                time.sleep(0.05)
            finally:
                connection.close()
        raise RuntimeError("HTTP host probe failed: {}".format(last_error))

    return execute


def run_oracle_command(
    argv: Sequence[str], *, cwd: pathlib.Path, timeout_seconds: float
) -> tuple[Dict[str, object], bytes, bytes]:
    started = time.monotonic()
    environment = dict(os.environ)
    environment.update(
        {
            "LANG": "C",
            "LC_ALL": "C",
            "TZ": "UTC",
            "SOURCE_DATE_EPOCH": "1700000000",
        }
    )
    try:
        completed = subprocess.run(
            list(argv),
            cwd=str(cwd),
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_seconds,
            check=False,
        )
        record = {
            "argv": recorded_argv(argv),
            "returncode": completed.returncode,
            "timed_out": False,
            "elapsed_seconds": round(time.monotonic() - started, 6),
        }
        return record, completed.stdout, completed.stderr
    except subprocess.TimeoutExpired as failure:
        record = {
            "argv": recorded_argv(argv),
            "returncode": None,
            "timed_out": True,
            "elapsed_seconds": round(time.monotonic() - started, 6),
        }
        return record, failure.stdout or b"", failure.stderr or b""


def run_filesystem_probe(probe, image: pathlib.Path,
                         scenario_root: pathlib.Path) -> Dict[str, object]:
    oracle_root = scenario_root / "host-filesystem-oracle"
    oracle_root.mkdir(parents=True, exist_ok=False)
    record: Dict[str, object] = {
        "passed": False,
        "offset_bytes": probe.offset_bytes,
        "tail_reserve_bytes": probe.tail_reserve_bytes,
        "path": probe.path,
        "expected_bytes": probe.expected_bytes,
        "expected_sha256": probe.expected_sha256,
        "commands": [],
    }
    image_bytes = image.stat().st_size
    region_bytes = image_bytes - probe.offset_bytes - probe.tail_reserve_bytes
    record["region_bytes"] = region_bytes
    if region_bytes <= 0 or region_bytes % 4096 != 0:
        record["error"] = "filesystem probe region is empty or unaligned"
        return record

    compiler = shutil.which(os.environ.get("HOST_CC", "cc"))
    if compiler is None:
        record["error"] = "host C compiler not found"
        return record
    tools = {
        "fsck": ("tools/fsck_northstar.c", oracle_root / "fsck.northstar"),
        "inspect": ("tools/nsfs_inspect.c", oracle_root / "nsfs_inspect"),
    }
    for name, (source, output) in tools.items():
        command = [
            compiler,
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pedantic",
            "-Iinclude",
            "tools/nsfs_host.c",
            source,
            "-o",
            runtime_path(output),
        ]
        command_record, stdout, stderr = run_oracle_command(
            command, cwd=PROJECT_ROOT, timeout_seconds=30
        )
        (oracle_root / (name + ".compile.stdout.log")).write_bytes(stdout)
        (oracle_root / (name + ".compile.stderr.log")).write_bytes(stderr)
        record["commands"].append(command_record)
        if command_record["returncode"] != 0 or command_record["timed_out"]:
            record["error"] = "could not build independent {} tool".format(name)
            return record

    fsck_json = oracle_root / "fsck.json"
    fsck_stderr = oracle_root / "fsck.stderr.log"
    fsck_command = [
        runtime_path(tools["fsck"][1]),
        "--json",
        "--offset",
        str(probe.offset_bytes),
        "--size",
        str(region_bytes),
        runtime_path(image),
    ]
    fsck_record, fsck_stdout, fsck_error = run_oracle_command(
        fsck_command, cwd=PROJECT_ROOT, timeout_seconds=30
    )
    fsck_stderr.write_bytes(fsck_error)
    record["commands"].append(fsck_record)
    if fsck_record["returncode"] != 0 or fsck_record["timed_out"]:
        record["error"] = "independent fsck rejected the persisted image"
        return record
    try:
        fsck_result = json.loads(fsck_stdout.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError):
        record["error"] = "independent fsck did not emit valid JSON"
        return record
    if isinstance(fsck_result, dict) and isinstance(fsck_result.get("image"), str):
        fsck_result["image"] = runtime_path(image)
    write_json(fsck_json, fsck_result)
    record["fsck"] = fsck_result
    journal = fsck_result.get("journal", {})
    if (
        fsck_result.get("valid") is not True
        or fsck_result.get("clean") is not True
        or fsck_result.get("errors") != 0
        or journal.get("state") != "empty"
    ):
        record["error"] = "independent fsck did not prove a clean empty-journal image"
        return record

    extracted = oracle_root / "payload.bin"
    inspect_stderr = oracle_root / "inspect.stderr.log"
    inspect_command = [
        runtime_path(tools["inspect"][1]),
        "--offset",
        str(probe.offset_bytes),
        "--size",
        str(region_bytes),
        runtime_path(image),
        "cat",
        probe.path,
    ]
    inspect_record, payload, inspect_error = run_oracle_command(
        inspect_command, cwd=PROJECT_ROOT, timeout_seconds=30
    )
    extracted.write_bytes(payload)
    inspect_stderr.write_bytes(inspect_error)
    record["commands"].append(inspect_record)
    actual_hash = hashlib.sha256(payload).hexdigest()
    record["actual_bytes"] = len(payload)
    record["actual_sha256"] = actual_hash
    record["extracted_file"] = str(extracted.relative_to(scenario_root))
    if inspect_record["returncode"] != 0 or inspect_record["timed_out"]:
        record["error"] = "independent inspector could not extract the persisted file"
        return record
    if len(payload) != probe.expected_bytes or actual_hash != probe.expected_sha256:
        record["error"] = "independent payload size or SHA-256 mismatch"
        return record
    record["passed"] = True
    return record


def safe_id(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]", "_", value)


def write_json(path: pathlib.Path, value: object) -> None:
    temporary = path.with_name("." + path.name + ".tmp-{}".format(os.getpid()))
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(str(temporary), str(path))


def maybe_validate_tap(serial_text: str) -> None:
    if re.search(r"(?m)^TAP version 13\r?$", serial_text) is None:
        return
    report = parse_tap(serial_text)
    report.assert_success()


def combined_serial(results: Sequence[RunResult]) -> str:
    pieces: List[str] = []
    for index, result in enumerate(results, start=1):
        text = result.serial_log.read_text(encoding="utf-8", errors="replace")
        pieces.append("# NORTHSTAR HOST PHASE {}\n{}".format(index, text))
    return "\n".join(pieces)


def verify_transcripts(scenario: Scenario, transcripts: Sequence[str]) -> None:
    """Validate aggregate requirements and one terminal sentinel per boot."""

    if not transcripts:
        raise ScenarioError("{}: no serial transcripts".format(scenario.id))
    terminal_pattern = scenario.required_serial[-1]
    for index, transcript in enumerate(transcripts, start=1):
        matches = list(re.finditer(terminal_pattern, transcript, flags=re.MULTILINE))
        if len(matches) != 1:
            raise ScenarioError(
                "{}: boot {} terminal sentinel matched {} times".format(
                    scenario.id, index, len(matches)
                )
            )
        if transcript[matches[0].end() :].strip():
            raise ScenarioError(
                "{}: boot {} emitted output after terminal sentinel".format(
                    scenario.id, index
                )
            )
    aggregate = "\n".join(transcripts)
    missing = [
        pattern
        for pattern in scenario.required_serial[:-1]
        if re.search(pattern, aggregate, flags=re.MULTILINE) is None
    ]
    forbidden = [
        pattern
        for pattern in scenario.forbidden_serial
        if re.search(pattern, aggregate, flags=re.MULTILINE) is not None
    ]
    if missing or forbidden:
        raise ScenarioError(
            "{}: missing required regexes {}; matched forbidden regexes {}".format(
                scenario.id, missing, forbidden
            )
        )


def run_scenario(
    scenario: Scenario,
    *,
    source_image: pathlib.Path,
    session_dir: pathlib.Path,
    qemu: str,
    repeat: int,
) -> Dict[str, object]:
    validate_scenario_qemu_args(scenario.qemu_args)
    scenario_root = session_dir / scenario.id
    scenario_root.mkdir(parents=True, exist_ok=False)
    image = source_image
    if scenario.persistence:
        image = scenario_root / "writable.img"
        shutil.copyfile(str(source_image), str(image))

    attempts = max(2 if scenario.persistence else 1, repeat)
    results: List[RunResult] = []
    probe_records: List[Dict[str, object]] = []
    network_peer_records: List[Dict[str, object]] = []
    filesystem_probe_record = None
    for attempt in range(1, attempts + 1):
        probe = getattr(scenario, "http_probe", None)
        probe_record: Dict[str, object] = {
            "attempt": attempt,
            "declared": probe is not None,
            "passed": False,
        }
        ready_pattern = None if probe is None else probe.ready_serial
        ready_action = None if probe is None else make_http_probe(probe, probe_record)
        extra_args = list(scenario.qemu_args)
        peer_process = None
        peer_record = None
        # A fresh M4 image performs its durability write and exits on boot one.
        # Start the external Ethernet peer only on boots that can reach M5.
        peer_required = scenario.network and probe is None and (
            not scenario.persistence or attempt > 1
        )
        if peer_required:
            peer_process, peer_record, qemu_endpoint, peer_endpoint = start_network_peer(
                scenario_root, attempt, scenario.timeout_seconds
            )
            extra_args += network_arguments(
                scenario,
                qemu_endpoint=qemu_endpoint,
                peer_endpoint=peer_endpoint,
            )
        elif scenario.network and probe is not None:
            extra_args += network_arguments(scenario)
        try:
            result = run_qemu(
                image=image,
                artifacts_dir=scenario_root,
                run_id="boot-{:02d}".format(attempt),
                timeout_seconds=scenario.timeout_seconds,
                expected_debug_exit=scenario.expected_debug_exit,
                # Persistence assertions can be split between pre- and post-reboot
                # phases, so the aggregate transcript is checked below.
                required_patterns=() if scenario.persistence else scenario.required_serial,
                forbidden_patterns=scenario.forbidden_serial,
                qemu=qemu,
                writable=scenario.persistence,
                extra_args=extra_args,
                ready_pattern=ready_pattern,
                ready_action=ready_action,
            )
        finally:
            if peer_process is not None and peer_record is not None:
                finish_network_peer(peer_process, peer_record)
                network_peer_records.append(peer_record)
        results.append(result)
        if probe is not None:
            probe_records.append(probe_record)
        if not result.passed:
            break

    transcripts = [
        result.serial_log.read_text(encoding="utf-8", errors="replace")
        for result in results
    ]
    aggregate = combined_serial(results)
    aggregate_path = scenario_root / "serial.combined.log"
    aggregate_path.write_text(aggregate, encoding="utf-8")
    error: str | None = None
    try:
        if len(results) != attempts:
            raise ScenarioError("only {} of {} required boots completed".format(len(results), attempts))
        if not all(result.passed for result in results):
            raise ScenarioError("one or more QEMU boots failed")
        if getattr(scenario, "http_probe", None) is not None and (
            len(probe_records) != attempts
            or not all(bool(record.get("passed")) for record in probe_records)
        ):
            raise ScenarioError("one or more independent HTTP host probes failed")
        expected_peer_runs = (
            attempts - 1 if scenario.network and scenario.persistence and probe is None
            else attempts if scenario.network and probe is None
            else 0
        )
        if expected_peer_runs != 0 and (
            len(network_peer_records) != expected_peer_runs
            or not all(bool(record.get("passed")) for record in network_peer_records)
        ):
            raise ScenarioError("one or more deterministic Ethernet peer oracles failed")
        verify_transcripts(scenario, transcripts)
        # Each boot owns a distinct TAP stream.  Parsing the concatenation
        # would either hide a later failure or reject its second version line.
        for transcript in transcripts:
            maybe_validate_tap(transcript)
        filesystem_probe = getattr(scenario, "filesystem_probe", None)
        if filesystem_probe is not None:
            filesystem_probe_record = run_filesystem_probe(
                filesystem_probe, image, scenario_root
            )
            if not filesystem_probe_record.get("passed"):
                raise ScenarioError(
                    "independent filesystem probe failed: {}".format(
                        filesystem_probe_record.get("error", "unknown error")
                    )
                )
    except (ScenarioError, TapError) as failure:
        error = str(failure)

    passed = error is None
    record = {
        "schema": "northstar.integration-scenario-result.v1",
        "id": scenario.id,
        "milestone": scenario.milestone,
        "description": scenario.description,
        "passed": passed,
        "required_boots": attempts,
        "completed_boots": len(results),
        "persistent_image": scenario.persistence,
        "network_enabled": scenario.network,
        "http_probe_results": probe_records,
        "network_peer_results": network_peer_records,
        "filesystem_probe_result": filesystem_probe_record,
        "expected_debug_exit": scenario.expected_debug_exit,
        "image_sha256": sha256_file(source_image),
        "run_results": [str(result.result_file.relative_to(session_dir)) for result in results],
        "combined_serial": str(aggregate_path.relative_to(session_dir)),
        "error": error,
    }
    write_json(scenario_root / "scenario-result.json", record)
    return record


def select_scenarios(arguments: argparse.Namespace) -> List[Scenario]:
    scenarios = list(load_scenarios(arguments.scenarios_dir))
    by_id = {scenario.id: scenario for scenario in scenarios}
    if arguments.scenario:
        missing = sorted(set(arguments.scenario) - set(by_id))
        if missing:
            raise ScenarioError("unknown scenario ids: {}".format(missing))
        return [scenario for scenario in scenarios if scenario.id in set(arguments.scenario)]
    maximum = int(arguments.milestone[1])
    return [scenario for scenario in scenarios if int(scenario.milestone[1]) <= maximum]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=pathlib.Path, required=True)
    parser.add_argument(
        "--scenarios-dir",
        type=pathlib.Path,
        default=PROJECT_ROOT / "tests" / "integration" / "scenarios",
    )
    parser.add_argument("--artifacts-dir", type=pathlib.Path, default=PROJECT_ROOT / "artifacts" / "integration")
    parser.add_argument("--session-id")
    parser.add_argument("--milestone", choices=["M0", "M1", "M2", "M3", "M4", "M5"], default="M1")
    parser.add_argument("--scenario", action="append", default=[])
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--keep-going", action="store_true")
    arguments = parser.parse_args()
    if arguments.repeat < 1 or arguments.repeat > 100:
        parser.error("--repeat must be in [1, 100]")
    return arguments


def main() -> int:
    arguments = parse_args()
    if not arguments.image.is_file():
        print("run_integration: image does not exist: {}".format(arguments.image), file=sys.stderr)
        return 2
    session_id = arguments.session_id
    if session_id is None:
        timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        session_id = "integration-{}-{}".format(timestamp, os.getpid())
    session_dir = arguments.artifacts_dir / safe_id(session_id)
    try:
        session_dir.mkdir(parents=True, exist_ok=False)
        scenarios = select_scenarios(arguments)
        if not scenarios:
            raise ScenarioError("scenario selection is empty")
        records: List[Dict[str, object]] = []
        for scenario in scenarios:
            print("RUN  {} ({})".format(scenario.id, scenario.milestone), flush=True)
            record = run_scenario(
                scenario,
                source_image=arguments.image,
                session_dir=session_dir,
                qemu=arguments.qemu,
                repeat=arguments.repeat,
            )
            records.append(record)
            print("{} {}".format("PASS" if record["passed"] else "FAIL", scenario.id), flush=True)
            if not record["passed"] and not arguments.keep_going:
                break
        passed = len(records) == len(scenarios) and all(bool(record["passed"]) for record in records)
        required_cold_boots = sum(
            int(record.get("required_boots", 0)) for record in records
        )
        completed_cold_boots = sum(
            int(record.get("completed_boots", 0)) for record in records
        )
        summary = {
            "schema": (
                "northstar.repetition-summary.v1"
                if arguments.repeat > 1
                else "northstar.integration-summary.v1"
            ),
            "passed": passed,
            "image_sha256": sha256_file(arguments.image),
            "repeat_requested": arguments.repeat,
            "required_cold_boots": required_cold_boots,
            "completed_cold_boots": completed_cold_boots,
            "selected": [scenario.id for scenario in scenarios],
            "completed": [record["id"] for record in records],
            "results": records,
        }
        write_json(session_dir / "summary.json", summary)
    except (OSError, ValueError, ScenarioError, re.error) as error:
        print("run_integration: error: {}".format(error), file=sys.stderr)
        return 2
    print("integration evidence: {}".format(session_dir))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
