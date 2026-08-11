"""Deterministic host-side support for NorthstarOS verification tools.

This module deliberately depends only on the Python standard library.  It is
both a small library for ``tools/*.py`` and a unittest module for the library
itself.  Keeping scenario validation here makes malformed acceptance tests fail
before QEMU is launched.
"""

from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType
from typing import Any, Iterable, Mapping, Sequence
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[2]
SCENARIO_ROOT = PROJECT_ROOT / "tests" / "integration" / "scenarios"
SCENARIO_SCHEMA = PROJECT_ROOT / "tests" / "integration" / "scenario.schema.json"
TOOLS_ROOT = PROJECT_ROOT / "tools"
DEFAULT_SOURCE_DATE_EPOCH = 1_700_000_000
DEFAULT_DEBUG_EXIT = 0x10
UNIVERSAL_COMPLETION_PATTERN = r"^NS:RUN:COMPLETE$"


class VerificationError(RuntimeError):
    """Base class for deterministic verification failures."""


class CommandError(VerificationError):
    """A host command failed or exceeded its deadline."""


class TapError(VerificationError):
    """A guest TAP stream was malformed or reported a failure."""


class ScenarioError(VerificationError):
    """An integration scenario violated the static contract."""


@dataclass(frozen=True)
class CommandResult:
    """Captured result from an argv-only host command."""

    argv: tuple[str, ...]
    returncode: int
    stdout: str
    stderr: str


def deterministic_environment(
    overrides: Mapping[str, str] | None = None,
    *,
    source_date_epoch: int = DEFAULT_SOURCE_DATE_EPOCH,
) -> dict[str, str]:
    """Return a copy of the host environment with reproducible defaults.

    ``PATH`` and toolchain-related variables remain available, while locale,
    timezone, Python hash randomization, and source timestamps are pinned.
    Callers may override any value explicitly.  The caller's mapping and
    ``os.environ`` are never mutated.
    """

    if source_date_epoch < 0:
        raise ValueError("source_date_epoch must be non-negative")
    env = dict(os.environ)
    env.update(
        {
            "LANG": "C",
            "LC_ALL": "C",
            "TZ": "UTC",
            "PYTHONHASHSEED": "0",
            "SOURCE_DATE_EPOCH": str(source_date_epoch),
        }
    )
    if overrides:
        for key, value in overrides.items():
            if not isinstance(key, str) or not key or "\x00" in key or "=" in key:
                raise ValueError(f"invalid environment key: {key!r}")
            if not isinstance(value, str) or "\x00" in value:
                raise ValueError(f"invalid environment value for {key!r}")
            env[key] = value
    return env


def run_command(
    argv: Sequence[str | os.PathLike[str]],
    *,
    cwd: Path = PROJECT_ROOT,
    timeout_seconds: float = 60.0,
    env: Mapping[str, str] | None = None,
    check: bool = True,
) -> CommandResult:
    """Run an argument vector without a shell and capture UTF-8 output.

    Output is decoded with replacement so a corrupt guest/tool byte cannot
    hide the command's return status.  Timeouts and OS launch failures are
    normalized to ``CommandError`` with the exact argv in the diagnostic.
    """

    normalized_argv = tuple(os.fspath(argument) for argument in argv)
    if not normalized_argv or any(not argument for argument in normalized_argv):
        raise ValueError("argv must contain only non-empty arguments")
    if timeout_seconds <= 0:
        raise ValueError("timeout_seconds must be positive")
    if not cwd.is_dir():
        raise ValueError(f"command cwd is not a directory: {cwd}")

    command_env = deterministic_environment() if env is None else dict(env)
    try:
        completed = subprocess.run(
            normalized_argv,
            cwd=cwd,
            env=command_env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_seconds,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        partial_stdout = _decode_output(exc.stdout)
        partial_stderr = _decode_output(exc.stderr)
        raise CommandError(
            f"command timed out after {timeout_seconds:g}s: "
            f"{normalized_argv!r}\nstdout:\n{partial_stdout}\nstderr:\n{partial_stderr}"
        ) from exc
    except OSError as exc:
        raise CommandError(f"could not launch {normalized_argv!r}: {exc}") from exc

    result = CommandResult(
        argv=normalized_argv,
        returncode=completed.returncode,
        stdout=_decode_output(completed.stdout),
        stderr=_decode_output(completed.stderr),
    )
    if check and result.returncode != 0:
        raise CommandError(
            f"command exited {result.returncode}: {result.argv!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def _decode_output(value: bytes | str | None) -> str:
    if value is None:
        return ""
    if isinstance(value, str):
        return value
    return value.decode("utf-8", errors="replace")


def sha256_file(path: Path, *, chunk_size: int = 1024 * 1024) -> str:
    """Return the lowercase SHA-256 digest of a regular file."""

    if chunk_size <= 0:
        raise ValueError("chunk_size must be positive")
    if not path.is_file():
        raise VerificationError(f"not a regular file: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(chunk_size), b""):
            digest.update(chunk)
    return digest.hexdigest()


def relative_artifact(path: Path, *, root: Path = PROJECT_ROOT) -> str:
    """Return a stable POSIX path and reject artifacts outside ``root``."""

    try:
        relative = path.resolve(strict=True).relative_to(root.resolve(strict=True))
    except (FileNotFoundError, ValueError) as exc:
        raise VerificationError(f"artifact escapes or is missing from root: {path}") from exc
    return relative.as_posix()


def load_tool_module(name: str) -> ModuleType:
    """Load one repository tool by filename without requiring a Python package."""

    if not re.fullmatch(r"[a-z][a-z0-9_]*", name):
        raise ValueError(f"invalid tool module name: {name!r}")
    module_name = f"_northstar_tool_{name}"
    existing = sys.modules.get(module_name)
    if existing is not None:
        return existing
    path = TOOLS_ROOT / f"{name}.py"
    if not path.is_file():
        raise VerificationError(f"tool does not exist: {path}")
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise VerificationError(f"could not construct import spec for tool: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(module_name, None)
        raise
    return module


@dataclass(frozen=True)
class TapCase:
    """One TAP assertion emitted by the guest."""

    number: int
    ok: bool
    description: str
    directive: str | None = None
    reason: str = ""

    @property
    def is_failure(self) -> bool:
        return not self.ok and self.directive != "TODO"


@dataclass(frozen=True)
class TapReport:
    """A validated, single TAP v13 stream."""

    planned: int
    cases: tuple[TapCase, ...]
    bailed_out: str | None = None

    @property
    def successful(self) -> bool:
        return self.bailed_out is None and not any(case.is_failure for case in self.cases)

    def assert_success(self) -> None:
        if self.bailed_out is not None:
            raise TapError(f"guest bailed out: {self.bailed_out or '(no reason)'}")
        failures = [case for case in self.cases if case.is_failure]
        if failures:
            details = ", ".join(
                f"{case.number} ({case.description or 'unnamed'})" for case in failures
            )
            raise TapError(f"guest TAP failures: {details}")


_TAP_VERSION = re.compile(r"^TAP version (?P<version>\d+)$", re.IGNORECASE)
_TAP_PLAN = re.compile(r"^1\.\.(?P<count>\d+)(?:\s+#\s*(?P<note>.*))?$")
_TAP_BAIL = re.compile(r"^Bail out!\s*(?P<reason>.*)$", re.IGNORECASE)
_TAP_CASE = re.compile(
    r"^(?P<status>ok|not ok)"
    r"(?:\s+(?P<number>\d+))?"
    r"(?:\s*-?\s*(?P<description>.*?))?"
    r"(?:\s+#\s*(?P<directive>SKIP|TODO)(?:\s+(?P<reason>.*))?)?$",
    re.IGNORECASE,
)


def parse_tap(serial_output: str, *, require_version: bool = True) -> TapReport:
    """Extract and strictly validate one TAP stream from a serial transcript.

    Boot chatter before ``TAP version 13`` and ordinary chatter after the plan
    has completed are ignored.  Inside the TAP stream, comments and indented
    diagnostic/YAML lines are accepted; an unrecognized top-level line fails
    closed.  Assertion numbers must be contiguous and the plan must match the
    exact number of assertions.
    """

    lines = serial_output.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    started = False
    saw_version = False
    plan: int | None = None
    cases: list[TapCase] = []
    bailout: str | None = None

    for line_number, raw_line in enumerate(lines, start=1):
        stripped = _strip_ansi(raw_line).strip()
        version_match = _TAP_VERSION.fullmatch(stripped)
        if not started:
            if version_match:
                started = True
                saw_version = True
                if int(version_match.group("version")) != 13:
                    raise TapError(
                        f"unsupported TAP version on serial line {line_number}: {stripped}"
                    )
                continue
            if require_version:
                continue
            if _TAP_PLAN.fullmatch(stripped) or _TAP_CASE.fullmatch(stripped):
                started = True
            else:
                continue
        elif version_match:
            raise TapError(f"duplicate TAP version on serial line {line_number}")

        if not stripped or stripped.startswith("#") or raw_line[:1].isspace():
            continue

        bail_match = _TAP_BAIL.fullmatch(stripped)
        if bail_match:
            bailout = bail_match.group("reason")
            break

        plan_match = _TAP_PLAN.fullmatch(stripped)
        if plan_match:
            if plan is not None:
                raise TapError(f"duplicate TAP plan on serial line {line_number}")
            plan = int(plan_match.group("count"))
            if plan == 0:
                note = (plan_match.group("note") or "").upper()
                if not note.startswith("SKIP"):
                    raise TapError("a zero-test TAP plan must include a SKIP directive")
            continue

        case_match = _TAP_CASE.fullmatch(stripped)
        if case_match:
            expected_number = len(cases) + 1
            explicit_number = case_match.group("number")
            number = int(explicit_number) if explicit_number is not None else expected_number
            if number != expected_number:
                raise TapError(
                    f"non-contiguous TAP number on serial line {line_number}: "
                    f"expected {expected_number}, got {number}"
                )
            directive = case_match.group("directive")
            cases.append(
                TapCase(
                    number=number,
                    ok=case_match.group("status").lower() == "ok",
                    description=(case_match.group("description") or "").strip(),
                    directive=directive.upper() if directive else None,
                    reason=(case_match.group("reason") or "").strip(),
                )
            )
            continue

        if plan is not None and len(cases) == plan:
            break
        raise TapError(f"unrecognized TAP content on serial line {line_number}: {stripped!r}")

    if not started:
        expected = "TAP version 13" if require_version else "a TAP plan or assertion"
        raise TapError(f"serial transcript did not contain {expected}")
    if require_version and not saw_version:
        raise TapError("serial transcript did not contain TAP version 13")
    if bailout is not None:
        return TapReport(planned=plan or 0, cases=tuple(cases), bailed_out=bailout)
    if plan is None:
        raise TapError("TAP stream did not contain a plan")
    if len(cases) != plan:
        raise TapError(f"TAP plan expected {plan} assertions, observed {len(cases)}")
    return TapReport(planned=plan, cases=tuple(cases))


def _strip_ansi(value: str) -> str:
    return re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", value)


_SCENARIO_KEYS = frozenset(
    {
        "id",
        "milestone",
        "description",
        "timeout_seconds",
        "required_serial",
        "forbidden_serial",
        "qemu_args",
        "expected_debug_exit",
        "requires",
        "http_probe",
        "filesystem_probe",
    }
)
_SCENARIO_REQUIRED_KEYS = _SCENARIO_KEYS - {
    "expected_debug_exit",
    "http_probe",
    "filesystem_probe",
}
_REQUIRES_KEYS = frozenset({"network", "persistence"})
_HTTP_PROBE_KEYS = frozenset(
    {
        "ready_serial",
        "host",
        "host_port",
        "guest_port",
        "path",
        "expected_status",
        "expected_body_sha256",
    }
)
_FILESYSTEM_PROBE_KEYS = frozenset(
    {
        "offset_bytes",
        "tail_reserve_bytes",
        "path",
        "expected_bytes",
        "expected_sha256",
    }
)
_SCENARIO_ID = re.compile(r"^[a-z][a-z0-9_-]{2,63}$")
_MILESTONE = re.compile(r"^M[0-5]$")


@dataclass(frozen=True)
class HttpProbe:
    """Independent host HTTP assertion executed after a guest-ready marker."""

    ready_serial: str
    host: str
    host_port: int
    guest_port: int
    path: str
    expected_status: int
    expected_body_sha256: str


@dataclass(frozen=True)
class FilesystemProbe:
    """Independent host fsck and byte-extraction assertion."""

    offset_bytes: int
    tail_reserve_bytes: int
    path: str
    expected_bytes: int
    expected_sha256: str


@dataclass(frozen=True)
class Scenario:
    """A progressive black-box QEMU acceptance-test contract."""

    id: str
    milestone: str
    description: str
    timeout_seconds: int
    required_serial: tuple[str, ...]
    forbidden_serial: tuple[str, ...]
    qemu_args: tuple[str, ...]
    expected_debug_exit: int
    network: bool
    persistence: bool
    http_probe: HttpProbe | None
    filesystem_probe: FilesystemProbe | None
    source: Path

    @classmethod
    def from_mapping(cls, value: Mapping[str, Any], *, source: Path) -> "Scenario":
        if not isinstance(value, Mapping):
            raise ScenarioError(f"{source}: top-level JSON value must be an object")
        unknown = set(value) - _SCENARIO_KEYS
        missing = _SCENARIO_REQUIRED_KEYS - set(value)
        if unknown:
            raise ScenarioError(f"{source}: unknown scenario keys: {sorted(unknown)}")
        if missing:
            raise ScenarioError(f"{source}: missing scenario keys: {sorted(missing)}")

        scenario_id = _expect_string(value["id"], source, "id")
        if not _SCENARIO_ID.fullmatch(scenario_id):
            raise ScenarioError(f"{source}: invalid scenario id: {scenario_id!r}")
        milestone = _expect_string(value["milestone"], source, "milestone")
        if not _MILESTONE.fullmatch(milestone):
            raise ScenarioError(f"{source}: milestone must be M0 through M5")
        description = _expect_string(value["description"], source, "description")
        if len(description) > 240:
            raise ScenarioError(f"{source}: description exceeds 240 characters")

        timeout = _expect_plain_int(value["timeout_seconds"], source, "timeout_seconds")
        if not 1 <= timeout <= 3600:
            raise ScenarioError(f"{source}: timeout_seconds must be in [1, 3600]")
        debug_exit = _expect_plain_int(
            value.get("expected_debug_exit", DEFAULT_DEBUG_EXIT), source, "expected_debug_exit"
        )
        if not 0 <= debug_exit <= 127:
            raise ScenarioError(f"{source}: expected_debug_exit must be in [0, 127]")

        required = _expect_string_list(value["required_serial"], source, "required_serial")
        forbidden = _expect_string_list(value["forbidden_serial"], source, "forbidden_serial")
        qemu_args = _expect_string_list(value["qemu_args"], source, "qemu_args")
        if not required:
            raise ScenarioError(f"{source}: required_serial must not be empty")
        if required[-1] != UNIVERSAL_COMPLETION_PATTERN:
            raise ScenarioError(
                f"{source}: final required_serial must be {UNIVERSAL_COMPLETION_PATTERN!r}"
            )
        if len(set(required)) != len(required) or len(set(forbidden)) != len(forbidden):
            raise ScenarioError(f"{source}: serial regex lists must not contain duplicates")
        for field, patterns in (("required_serial", required), ("forbidden_serial", forbidden)):
            for pattern in patterns:
                try:
                    re.compile(pattern)
                except re.error as exc:
                    raise ScenarioError(
                        f"{source}: invalid {field} regex {pattern!r}: {exc}"
                    ) from exc

        requires = value["requires"]
        if not isinstance(requires, Mapping):
            raise ScenarioError(f"{source}: requires must be an object")
        if set(requires) != _REQUIRES_KEYS:
            raise ScenarioError(
                f"{source}: requires must contain exactly {sorted(_REQUIRES_KEYS)}"
            )
        for key in _REQUIRES_KEYS:
            if not isinstance(requires[key], bool):
                raise ScenarioError(f"{source}: requires.{key} must be a boolean")

        http_probe = None
        if "http_probe" in value:
            http_probe = _parse_http_probe(value["http_probe"], source)
            if not requires["network"]:
                raise ScenarioError(f"{source}: http_probe requires network=true")

        filesystem_probe = None
        if "filesystem_probe" in value:
            filesystem_probe = _parse_filesystem_probe(
                value["filesystem_probe"], source
            )
            if not requires["persistence"]:
                raise ScenarioError(
                    f"{source}: filesystem_probe requires persistence=true"
                )

        return cls(
            id=scenario_id,
            milestone=milestone,
            description=description,
            timeout_seconds=timeout,
            required_serial=required,
            forbidden_serial=forbidden,
            qemu_args=qemu_args,
            expected_debug_exit=debug_exit,
            network=requires["network"],
            persistence=requires["persistence"],
            http_probe=http_probe,
            filesystem_probe=filesystem_probe,
            source=source,
        )

    def verify_serial(self, serial_output: str) -> None:
        """Validate regex requirements and the unique terminal sentinel.

        The final entry in ``required_serial`` is the scenario's completion
        sentinel.  It must occur exactly once and no non-whitespace guest output
        may follow it.  This prevents a stale or duplicated pass line from
        masking a later crash.
        """

        absent = [
            pattern
            for pattern in self.required_serial
            if not re.search(pattern, serial_output, flags=re.MULTILINE)
        ]
        present = [
            pattern
            for pattern in self.forbidden_serial
            if re.search(pattern, serial_output, flags=re.MULTILINE)
        ]
        if absent or present:
            details: list[str] = []
            if absent:
                details.append(f"missing required regexes: {absent!r}")
            if present:
                details.append(f"matched forbidden regexes: {present!r}")
            raise ScenarioError(f"{self.id}: " + "; ".join(details))
        completion_pattern = self.required_serial[-1]
        completions = list(
            re.finditer(completion_pattern, serial_output, flags=re.MULTILINE)
        )
        if len(completions) != 1:
            raise ScenarioError(
                f"{self.id}: completion regex must match exactly once; "
                f"observed {len(completions)}: {completion_pattern!r}"
            )
        trailing_output = serial_output[completions[0].end() :]
        if trailing_output.strip():
            raise ScenarioError(
                f"{self.id}: non-whitespace output follows completion sentinel"
            )


def _expect_string(value: Any, source: Path, field: str) -> str:
    if not isinstance(value, str) or not value.strip() or "\x00" in value:
        raise ScenarioError(f"{source}: {field} must be a non-empty string without NUL")
    return value


def _expect_plain_int(value: Any, source: Path, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ScenarioError(f"{source}: {field} must be an integer")
    return value


def _expect_string_list(value: Any, source: Path, field: str) -> tuple[str, ...]:
    if not isinstance(value, list):
        raise ScenarioError(f"{source}: {field} must be a JSON array")
    result: list[str] = []
    for index, item in enumerate(value):
        if not isinstance(item, str) or not item or "\x00" in item:
            raise ScenarioError(f"{source}: {field}[{index}] must be a non-empty string")
        result.append(item)
    return tuple(result)


def _parse_http_probe(value: Any, source: Path) -> HttpProbe:
    if not isinstance(value, Mapping):
        raise ScenarioError(f"{source}: http_probe must be an object")
    if set(value) != _HTTP_PROBE_KEYS:
        missing = sorted(_HTTP_PROBE_KEYS - set(value))
        unknown = sorted(set(value) - _HTTP_PROBE_KEYS)
        raise ScenarioError(
            f"{source}: http_probe keys invalid; missing={missing}, unknown={unknown}"
        )
    ready_serial = _expect_string(value["ready_serial"], source, "http_probe.ready_serial")
    try:
        re.compile(ready_serial)
    except re.error as exc:
        raise ScenarioError(f"{source}: invalid http_probe.ready_serial regex: {exc}") from exc
    host = _expect_string(value["host"], source, "http_probe.host")
    if host != "127.0.0.1":
        raise ScenarioError(f"{source}: http_probe.host must be 127.0.0.1")
    host_port = _expect_plain_int(value["host_port"], source, "http_probe.host_port")
    if not 1024 <= host_port <= 65535:
        raise ScenarioError(f"{source}: http_probe.host_port must be in [1024, 65535]")
    guest_port = _expect_plain_int(value["guest_port"], source, "http_probe.guest_port")
    if not 1 <= guest_port <= 65535:
        raise ScenarioError(f"{source}: http_probe.guest_port must be in [1, 65535]")
    path = _expect_string(value["path"], source, "http_probe.path")
    if not path.startswith("/"):
        raise ScenarioError(f"{source}: http_probe.path must start with /")
    expected_status = _expect_plain_int(
        value["expected_status"], source, "http_probe.expected_status"
    )
    if not 100 <= expected_status <= 599:
        raise ScenarioError(f"{source}: http_probe.expected_status must be in [100, 599]")
    body_hash = _expect_string(
        value["expected_body_sha256"], source, "http_probe.expected_body_sha256"
    )
    if not re.fullmatch(r"[0-9a-f]{64}", body_hash):
        raise ScenarioError(
            f"{source}: http_probe.expected_body_sha256 must be 64 lowercase hex characters"
        )
    return HttpProbe(
        ready_serial=ready_serial,
        host=host,
        host_port=host_port,
        guest_port=guest_port,
        path=path,
        expected_status=expected_status,
        expected_body_sha256=body_hash,
    )


def _parse_filesystem_probe(value: Any, source: Path) -> FilesystemProbe:
    if not isinstance(value, Mapping):
        raise ScenarioError(f"{source}: filesystem_probe must be an object")
    if set(value) != _FILESYSTEM_PROBE_KEYS:
        missing = sorted(_FILESYSTEM_PROBE_KEYS - set(value))
        unknown = sorted(set(value) - _FILESYSTEM_PROBE_KEYS)
        raise ScenarioError(
            f"{source}: filesystem_probe keys invalid; "
            f"missing={missing}, unknown={unknown}"
        )
    offset = _expect_plain_int(
        value["offset_bytes"], source, "filesystem_probe.offset_bytes"
    )
    tail = _expect_plain_int(
        value["tail_reserve_bytes"], source,
        "filesystem_probe.tail_reserve_bytes"
    )
    expected_bytes = _expect_plain_int(
        value["expected_bytes"], source, "filesystem_probe.expected_bytes"
    )
    if offset < 0 or tail < 0 or offset % 4096 != 0 or tail % 4096 != 0:
        raise ScenarioError(
            f"{source}: filesystem probe offsets must be non-negative and 4096-byte aligned"
        )
    if not 1 <= expected_bytes <= 1024 * 1024 * 1024:
        raise ScenarioError(
            f"{source}: filesystem_probe.expected_bytes must be in [1, 1 GiB]"
        )
    path = _expect_string(value["path"], source, "filesystem_probe.path")
    components = path.split("/")
    if not path.startswith("/") or any(part in {".", ".."} for part in components):
        raise ScenarioError(
            f"{source}: filesystem_probe.path must be an absolute canonical path"
        )
    digest = _expect_string(
        value["expected_sha256"], source,
        "filesystem_probe.expected_sha256"
    )
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ScenarioError(
            f"{source}: filesystem_probe.expected_sha256 must be 64 lowercase hex characters"
        )
    return FilesystemProbe(
        offset_bytes=offset,
        tail_reserve_bytes=tail,
        path=path,
        expected_bytes=expected_bytes,
        expected_sha256=digest,
    )


def load_scenario(path: Path) -> Scenario:
    """Read and validate one UTF-8 JSON scenario."""

    try:
        with path.open("r", encoding="utf-8") as stream:
            value = json.load(stream)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ScenarioError(f"could not read scenario {path}: {exc}") from exc
    return Scenario.from_mapping(value, source=path)


def load_scenarios(root: Path = SCENARIO_ROOT) -> tuple[Scenario, ...]:
    """Load all scenarios in stable milestone/id order and reject ambiguity."""

    if not root.is_dir():
        raise ScenarioError(f"scenario directory does not exist: {root}")
    paths = sorted(root.glob("*.json"))
    if not paths:
        raise ScenarioError(f"scenario directory contains no JSON files: {root}")
    scenarios = [load_scenario(path) for path in paths]
    ids: set[str] = set()
    for scenario in scenarios:
        if scenario.id in ids:
            raise ScenarioError(f"duplicate scenario id: {scenario.id}")
        ids.add(scenario.id)
    return tuple(sorted(scenarios, key=lambda scenario: (scenario.milestone, scenario.id)))


def scenario_ids_through(scenarios: Iterable[Scenario], milestone: str) -> tuple[str, ...]:
    """Return the progressive set of scenario IDs required through a milestone."""

    if not _MILESTONE.fullmatch(milestone):
        raise ValueError("milestone must be M0 through M5")
    maximum = int(milestone[1])
    return tuple(s.id for s in scenarios if int(s.milestone[1]) <= maximum)


def _valid_scenario_mapping() -> dict[str, Any]:
    return {
        "id": "m0_boot_smoke",
        "milestone": "M0",
        "description": "Boot smoke test.",
        "timeout_seconds": 10,
        "required_serial": [UNIVERSAL_COMPLETION_PATTERN],
        "forbidden_serial": [r"PANIC"],
        "qemu_args": ["-m", "128M"],
        "expected_debug_exit": DEFAULT_DEBUG_EXIT,
        "requires": {"network": False, "persistence": False},
    }


def _valid_http_probe_mapping() -> dict[str, Any]:
    return {
        "ready_serial": r"^NS:NET:HTTP:READY port=80$",
        "host": "127.0.0.1",
        "host_port": 18080,
        "guest_port": 80,
        "path": "/evidence",
        "expected_status": 200,
        "expected_body_sha256": hashlib.sha256(b"northstar-network-ok\n").hexdigest(),
    }


class DeterministicSupportTests(unittest.TestCase):
    def test_environment_is_pinned_without_mutating_override(self) -> None:
        override = {"EXAMPLE": "value", "LANG": "C.UTF-8"}
        result = deterministic_environment(override, source_date_epoch=123)
        self.assertEqual(result["SOURCE_DATE_EPOCH"], "123")
        self.assertEqual(result["PYTHONHASHSEED"], "0")
        self.assertEqual(result["TZ"], "UTC")
        self.assertEqual(result["LANG"], "C.UTF-8")
        self.assertEqual(override, {"EXAMPLE": "value", "LANG": "C.UTF-8"})

    def test_file_hash_and_relative_artifact_are_stable(self) -> None:
        with tempfile.TemporaryDirectory(dir=PROJECT_ROOT) as temporary:
            artifact = Path(temporary) / "artifact.bin"
            artifact.write_bytes(b"northstar\x00test\n")
            self.assertEqual(
                sha256_file(artifact, chunk_size=3),
                "6735795802f78995a45932098314269ee2c01d5898065fa3d3b672517ae5ae89",
            )
            relative = relative_artifact(artifact)
            self.assertFalse(relative.startswith("/"))
            self.assertTrue(relative.endswith("artifact.bin"))

    def test_command_capture_and_failure(self) -> None:
        result = run_command(
            [sys.executable, "-c", "import sys; print('out'); print('err', file=sys.stderr)"],
            timeout_seconds=5,
        )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "out\n")
        self.assertEqual(result.stderr, "err\n")
        with self.assertRaisesRegex(CommandError, "exited 7"):
            run_command([sys.executable, "-c", "raise SystemExit(7)"], timeout_seconds=5)


class HostVerificationToolTests(unittest.TestCase):
    def test_debug_exit_decoding_distinguishes_guest_and_host_failures(self) -> None:
        run_qemu = load_tool_module("run_qemu")
        self.assertEqual(run_qemu.decode_debug_exit((0x10 << 1) | 1), DEFAULT_DEBUG_EXIT)
        for invalid in (None, -9, 0, 32, 34):
            with self.subTest(returncode=invalid):
                self.assertIsNone(run_qemu.decode_debug_exit(invalid))

    def test_qemu_command_is_headless_deterministic_and_snapshot_by_default(self) -> None:
        run_qemu = load_tool_module("run_qemu")
        command = run_qemu.canonical_qemu_command(
            "qemu-system-x86_64",
            Path("image.raw"),
            Path("serial.log"),
            128,
            1,
            False,
            (),
        )
        rendered = " ".join(command)
        self.assertEqual(command[command.index("-machine") + 1], "pc-i440fx-7.2")
        self.assertEqual(command[command.index("-accel") + 1], "tcg,thread=single")
        self.assertIn("base=2000-01-01T00:00:00,clock=vm", rendered)
        self.assertIn("snapshot=on", rendered)
        self.assertIn("cache=writeback", rendered)
        self.assertIn("isa-debug-exit,iobase=0xf4", rendered)
        self.assertNotIn("-daemonize", command)
        self.assertNotIn("-no-shutdown", command)

    def test_qemu_evidence_runner_passes_and_fails_closed_without_real_qemu(self) -> None:
        run_qemu = load_tool_module("run_qemu")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            image_path = root / "image.raw"
            image_path.write_bytes(b"northstar test image")
            fake_qemu = root / "fake-qemu"
            fake_qemu.write_text(
                f"#!{sys.executable}\n"
                "import pathlib, sys\n"
                "args = sys.argv[1:]\n"
                "serial = args[args.index('-serial') + 1]\n"
                "assert serial.startswith('file:')\n"
                "pathlib.Path(serial[5:]).write_text('NS:FAKE:PASS\\n', encoding='utf-8')\n"
                "raise SystemExit(33)\n",
                encoding="utf-8",
            )
            fake_qemu.chmod(0o755)
            passing = run_qemu.run_qemu(
                image=image_path,
                artifacts_dir=root / "artifacts",
                run_id="passing",
                timeout_seconds=5,
                expected_debug_exit=DEFAULT_DEBUG_EXIT,
                required_patterns=(r"^NS:FAKE:PASS$",),
                forbidden_patterns=(r"(?i)panic",),
                qemu=str(fake_qemu),
            )
            self.assertTrue(passing.passed)
            self.assertEqual(passing.debug_exit, DEFAULT_DEBUG_EXIT)
            evidence = json.loads(passing.result_file.read_text(encoding="utf-8"))
            self.assertEqual(evidence["schema"], "northstar.qemu-result.v1")
            self.assertRegex(evidence["serial_sha256"], r"^[0-9a-f]{64}$")

            failing = run_qemu.run_qemu(
                image=image_path,
                artifacts_dir=root / "artifacts",
                run_id="missing-marker",
                timeout_seconds=5,
                expected_debug_exit=DEFAULT_DEBUG_EXIT,
                required_patterns=(r"^NS:REAL:PASS$",),
                qemu=str(fake_qemu),
            )
            self.assertFalse(failing.passed)
            self.assertEqual(failing.missing_patterns, [r"^NS:REAL:PASS$"])

    def test_image_builder_rejects_bad_boot_signature_and_overlap(self) -> None:
        build_image = load_tool_module("build_image")
        with tempfile.TemporaryDirectory() as temporary:
            stage1 = Path(temporary) / "stage1.bin"
            stage1.write_bytes(bytes(510) + b"\x55\xaa")
            metadata = build_image.validate_artifact("stage1", stage1, 1)
            self.assertEqual(metadata["bytes"], 512)
            stage1.write_bytes(bytes(512))
            with self.assertRaisesRegex(build_image.ImageError, "0x55AA"):
                build_image.validate_artifact("stage1", stage1, 1)
        self.assertEqual(
            build_image.zero_ranges(((10, 20), (30, 40)), 50),
            [(0, 10), (20, 30), (40, 50)],
        )
        with self.assertRaisesRegex(build_image.ImageError, "overlap"):
            build_image.zero_ranges(((10, 30), (20, 40)), 50)

    def test_generated_boot_layout_is_bounded_and_content_addressed(self) -> None:
        layout_tool = load_tool_module("gen_image_layout")
        with tempfile.TemporaryDirectory() as temporary:
            kernel = Path(temporary) / "kernel.bin"
            kernel.write_bytes(b"kernel" * 100)
            layout = layout_tool.build_layout(
                kernel,
                None,
                None,
                layout_tool.KERNEL_ENTRY,
                layout_tool.KERNEL_LOAD_ADDR,
                layout_tool.INITRD_LOAD_ADDR,
                layout_tool.KERNEL_VIRT_ADDR,
                4096,
            )
        self.assertEqual(layout["schema"], "northstar.boot-layout.v1")
        self.assertEqual(layout["extents"]["kernel"]["sectors"], 2)
        self.assertEqual(layout["extents"]["kernel"]["memory_bytes"], 4096)
        self.assertRegex(layout["extents"]["kernel"]["sha256"], r"^[0-9a-f]{64}$")
        self.assertIn("%define KERNEL_LBA", layout_tool.nasm_text(layout))

    def test_boot_layout_derives_memory_extent_from_linker_symbols(self) -> None:
        layout_tool = load_tool_module("gen_image_layout")
        symbol_output = (
            b"_start T ffffffff80001000\n"
            b"__kernel_start A ffffffff80000000\n"
            b"__kernel_phys_start A 00100000\n"
            b"__kernel_phys_end A 00108000\n"
        )
        completed = subprocess.CompletedProcess(
            args=["fake-nm"], returncode=0, stdout=symbol_output, stderr=b""
        )
        with tempfile.TemporaryDirectory() as temporary:
            kernel_elf = Path(temporary) / "kernel.elf"
            kernel_elf.write_bytes(b"ELF fixture")
            with mock.patch.object(layout_tool.subprocess, "run", return_value=completed):
                derived = layout_tool.elf_layout(kernel_elf, "fake-nm")
        self.assertEqual(derived["kernel_entry"], 0xFFFFFFFF80001000)
        self.assertEqual(derived["kernel_virtual"], 0xFFFFFFFF80000000)
        self.assertEqual(derived["kernel_load"], 0x00100000)
        self.assertEqual(derived["kernel_memory_bytes"], 0x8000)

    def test_integration_runner_protects_evidence_channels(self) -> None:
        integration = load_tool_module("run_integration")
        for argument in (
            "-serial",
            "-drive=attacker.raw",
            "-qmp",
            "-machine=unversioned",
            "-accel",
            "-cpu",
            "-display",
            "-rtc",
            "-nic",
            "-netdev",
            "-smp",
        ):
            with self.subTest(argument=argument), self.assertRaises(integration.ScenarioError):
                integration.validate_scenario_qemu_args((argument,))
        with self.assertRaises(integration.ScenarioError):
            integration.validate_scenario_qemu_args(("-device", "isa-debug-exit,iobase=0x80"))
        integration.validate_scenario_qemu_args(("-device", "rtl8139", "-m", "512M"))

    def test_integration_runner_enables_real_emulated_network_device(self) -> None:
        integration = load_tool_module("run_integration")
        mapping = _valid_scenario_mapping()
        mapping["requires"] = {"network": True, "persistence": False}
        mapping["http_probe"] = _valid_http_probe_mapping()
        scenario = Scenario.from_mapping(mapping, source=Path("test.json"))
        arguments = integration.network_arguments(scenario)
        self.assertTrue(
            any(argument.startswith("rtl8139,netdev=northstar-net,") for argument in arguments)
        )
        self.assertTrue(any(argument.startswith("user,id=northstar-net") for argument in arguments))
        self.assertTrue(
            any("hostfwd=tcp:127.0.0.1:18080-:80" in argument for argument in arguments)
        )

    def test_integration_runner_uses_deterministic_ethernet_peer_for_m5(self) -> None:
        integration = load_tool_module("run_integration")
        mapping = _valid_scenario_mapping()
        mapping["requires"] = {"network": True, "persistence": True}
        scenario = Scenario.from_mapping(mapping, source=Path("test.json"))
        arguments = integration.network_arguments(
            scenario,
            qemu_endpoint="127.0.0.1:31000",
            peer_endpoint="127.0.0.1:31001",
        )
        self.assertIn(
            "socket,id=northstar-net,udp=127.0.0.1:31001,localaddr=127.0.0.1:31000",
            arguments,
        )
        self.assertIn(
            "rtl8139,netdev=northstar-net,mac=52:54:00:12:34:57",
            arguments,
        )

    def test_independent_http_probe_checks_status_and_body_hash(self) -> None:
        integration = load_tool_module("run_integration")
        mapping = _valid_scenario_mapping()
        mapping["requires"] = {"network": True, "persistence": False}
        mapping["http_probe"] = _valid_http_probe_mapping()
        scenario = Scenario.from_mapping(mapping, source=Path("test.json"))
        assert scenario.http_probe is not None

        class FakeResponse:
            status = 200

            def __init__(self, body: bytes) -> None:
                self.body = body

            def read(self, limit: int) -> bytes:
                self.test_limit = limit
                return self.body

        class FakeConnection:
            def __init__(self, body: bytes) -> None:
                self.response = FakeResponse(body)
                self.requests: list[tuple[str, str, Mapping[str, str]]] = []

            def request(self, method: str, path: str, *, headers: Mapping[str, str]) -> None:
                self.requests.append((method, path, headers))

            def getresponse(self) -> FakeResponse:
                return self.response

            def close(self) -> None:
                return None

        connection = FakeConnection(b"northstar-network-ok\n")
        record: dict[str, object] = {}
        with mock.patch.object(
            integration.http.client, "HTTPConnection", return_value=connection
        ):
            integration.make_http_probe(scenario.http_probe, record)()
        self.assertTrue(record["passed"])
        self.assertEqual(record["status"], 200)
        self.assertEqual(record["body_bytes"], len(b"northstar-network-ok\n"))
        self.assertEqual(connection.requests[0][0:2], ("GET", "/evidence"))

        bad_connection = FakeConnection(b"wrong body")
        with mock.patch.object(
            integration.http.client, "HTTPConnection", return_value=bad_connection
        ), mock.patch.object(
            integration.time, "monotonic", side_effect=[0.0, 0.0, 9.0]
        ), mock.patch.object(
            integration.time, "sleep", return_value=None
        ):
            with self.assertRaisesRegex(RuntimeError, "body SHA-256"):
                integration.make_http_probe(scenario.http_probe, {})()

    def test_persistence_transcripts_each_require_one_terminal_sentinel(self) -> None:
        integration = load_tool_module("run_integration")
        mapping = _valid_scenario_mapping()
        mapping["required_serial"] = [
            r"^PHASE:CREATE:PASS$",
            r"^PHASE:VERIFY:PASS$",
            UNIVERSAL_COMPLETION_PATTERN,
        ]
        mapping["requires"] = {"network": False, "persistence": True}
        scenario = Scenario.from_mapping(mapping, source=Path("test.json"))
        transcripts = (
            "PHASE:CREATE:PASS\nNS:RUN:COMPLETE\n",
            "PHASE:VERIFY:PASS\nNS:RUN:COMPLETE\n",
        )
        integration.verify_transcripts(scenario, transcripts)
        with self.assertRaisesRegex(integration.ScenarioError, "matched 2 times"):
            integration.verify_transcripts(
                scenario,
                (transcripts[0], transcripts[1] + "NS:RUN:COMPLETE\n"),
            )
        with self.assertRaisesRegex(integration.ScenarioError, "missing required regexes"):
            integration.verify_transcripts(scenario, (transcripts[0], transcripts[0]))

    def test_reproducible_copy_preserves_versioned_make_fragments(self) -> None:
        reproducible = load_tool_module("check_reproducible")
        release = load_tool_module("gen_release_evidence")
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            build = source / "build"
            build.mkdir()
            (build / "verification.mk").write_text("HOST_TESTS := 1\n", encoding="utf-8")
            (build / "storage_fs.mk").write_text("FS_TESTS := 1\n", encoding="utf-8")
            (build / "generated.bin").write_bytes(b"generated")
            (source / "kernel.c").write_text("int kernel;\n", encoding="utf-8")
            (source / "cached.pyc").write_bytes(b"cache")
            generated_profile = source / "build-interactive"
            generated_profile.mkdir()
            (generated_profile / "kernel.bin").write_bytes(b"generated profile")
            ignored = reproducible.copy_ignore(source)
            build_names = ["verification.mk", "storage_fs.mk", "generated.bin"]
            ignored_build_names = ignored(str(build), build_names)
            self.assertNotIn("verification.mk", ignored_build_names)
            self.assertNotIn("storage_fs.mk", ignored_build_names)
            self.assertIn(
                "generated.bin", ignored_build_names
            )
            self.assertIn("cached.pyc", ignored(str(source), ["cached.pyc", "kernel.c"]))

            included = {
                path.relative_to(source).as_posix()
                for path in reproducible.source_files(source)
            }
            self.assertEqual(
                included,
                {"build/storage_fs.mk", "build/verification.mk", "kernel.c"},
            )
            release_included = {
                path.relative_to(source).as_posix() for path in release.source_files(source)
            }
            self.assertEqual(release_included, included)
            first_hash = reproducible.source_tree_hash(source)
            self.assertEqual(release.source_tree(source)["sha256"], first_hash)
            os.utime(source / "kernel.c", (1, 1))
            self.assertEqual(reproducible.source_tree_hash(source), first_hash)
            (source / "kernel.c").write_text("int changed;\n", encoding="utf-8")
            self.assertNotEqual(reproducible.source_tree_hash(source), first_hash)

    def test_reproducibility_report_compares_two_isolated_command_builds(self) -> None:
        reproducible = load_tool_module("check_reproducible")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            (source / "Makefile").write_text("# test fixture\n", encoding="utf-8")
            (source / "build").mkdir()
            (source / "build" / "verification.mk").write_text(
                "# versioned build input\n", encoding="utf-8"
            )
            writer = source / "writer.py"
            writer.write_text(
                "from pathlib import Path\n"
                "Path('build').mkdir(exist_ok=True)\n"
                "Path('build/out.bin').write_bytes(b'identical')\n",
                encoding="utf-8",
            )
            artifacts = root / "repro-evidence"
            arguments = type(
                "Arguments",
                (),
                {
                    "source": source,
                    "command": shlex.join([sys.executable, "writer.py"]),
                    "artifact": ["build/out.bin"],
                    "source_date_epoch": 123,
                    "timeout": 10.0,
                    "artifacts_dir": artifacts,
                },
            )()
            with mock.patch.object(reproducible, "parse_args", return_value=arguments):
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(
                    io.StringIO()
                ):
                    self.assertEqual(reproducible.main(), 0)
            report = json.loads((artifacts / "report.json").read_text(encoding="utf-8"))
            self.assertTrue(report["passed"])
            self.assertEqual(report["source_tree_sha256"], reproducible.source_tree_hash(source))
            self.assertTrue(report["comparisons"][0]["identical"])
            self.assertEqual(report["comparisons"][0]["bytes"], [9, 9])

            writer.write_text(
                "from pathlib import Path\n"
                "Path('build').mkdir(exist_ok=True)\n"
                "Path('build/out.bin').write_text(Path.cwd().name, encoding='utf-8')\n",
                encoding="utf-8",
            )
            with mock.patch.object(reproducible, "parse_args", return_value=arguments):
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(
                    io.StringIO()
                ):
                    self.assertEqual(reproducible.main(), 1)
            report = json.loads((artifacts / "report.json").read_text(encoding="utf-8"))
            self.assertFalse(report["passed"])
            self.assertFalse(report["comparisons"][0]["identical"])
            self.assertIn("artifacts differ", report["error"])

    def test_release_evidence_accepts_only_matching_image_and_passing_results(self) -> None:
        release = load_tool_module("gen_release_evidence")
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "source"
            build = source / "build"
            evidence_dir = source / "artifacts" / "verification"
            build.mkdir(parents=True)
            evidence_dir.mkdir(parents=True)
            image = build / "northstar.img"
            image.write_bytes(b"release image fixture")
            image_manifest = build / "image-layout.json"
            boot_layout = build / "boot-layout.json"
            test_result = evidence_dir / "result.json"
            raw_log = evidence_dir / "serial.log"
            output = source / "release" / "evidence.json"
            image_hash = release.hash_file(image)
            image_manifest.write_text(
                json.dumps({"image": {"sha256": image_hash}}), encoding="utf-8"
            )
            boot_layout.write_text(
                json.dumps({"schema": "northstar.boot-layout.v1"}), encoding="utf-8"
            )
            test_result.write_text(
                json.dumps(
                    {
                        "schema": "northstar.integration-summary.v1",
                        "passed": True,
                        "image_sha256": image_hash,
                        "repeat_requested": 1,
                        "required_cold_boots": 10,
                        "completed_cold_boots": 10,
                        "selected": [
                            "m0_stage1_boot",
                            "m1_long_mode_boot",
                            "m2_memory_scheduler",
                            "m3_ring3_processes",
                            "m4_nsfs_persistence",
                            "m4_user_environment",
                            "m5_network_interop",
                        ],
                        "results": [
                            {
                                "id": scenario,
                                "passed": True,
                                "network_peer_results": (
                                    [
                                        {
                                            "passed": True,
                                            "pcap_sha256": "a" * 64,
                                            "pcap_packets": 1,
                                        }
                                    ]
                                    if scenario == "m5_network_interop"
                                    else []
                                ),
                            }
                            for scenario in (
                                "m0_stage1_boot",
                                "m1_long_mode_boot",
                                "m2_memory_scheduler",
                                "m3_ring3_processes",
                                "m4_nsfs_persistence",
                                "m4_user_environment",
                                "m5_network_interop",
                            )
                        ],
                    }
                ),
                encoding="utf-8",
            )
            raw_log.write_text("TAP version 13\n1..1\nok 1 - release\n", encoding="utf-8")

            arguments = type(
                "Arguments",
                (),
                {
                    "source": source,
                    "image": Path("build/northstar.img"),
                    "image_manifest": Path("build/image-layout.json"),
                    "boot_layout": Path("build/boot-layout.json"),
                    "interactive_image": None,
                    "sbom": None,
                    "evidence": [Path("artifacts/verification")],
                    "test_result": [Path("artifacts/verification/result.json")],
                    "host_result": [],
                    "output": Path("release/evidence.json"),
                    "source_date_epoch": 123,
                    "require_clean": False,
                    "strict_tools": False,
                    "require_release_gates": False,
                },
            )()
            stable_git = {
                "available": False,
                "commit": None,
                "repository": None,
                "project_prefix": None,
                "clean": None,
                "status_sha256": hashlib.sha256(b"").hexdigest(),
                "status_entries": 0,
            }
            with mock.patch.object(
                release, "parse_args", return_value=arguments
            ), mock.patch.object(
                release, "git_state", return_value=stable_git
            ), mock.patch.object(
                release, "TOOL_COMMANDS", {}
            ):
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(
                    io.StringIO()
                ):
                    self.assertEqual(release.main(), 0)
            manifest = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(manifest["release_image"]["sha256"], image_hash)
            self.assertEqual(manifest["generated_at"], "1970-01-01T00:02:03Z")
            self.assertEqual(len(manifest["passing_test_results"]), 1)
            self.assertTrue(output.with_suffix(".json.sha256").is_file())
            accepted_manifest_hash = release.hash_file(output)

            image_manifest.write_text(
                json.dumps({"image": {"sha256": "0" * 64}}), encoding="utf-8"
            )
            error_output = io.StringIO()
            with mock.patch.object(
                release, "parse_args", return_value=arguments
            ), mock.patch.object(
                release, "git_state", return_value=stable_git
            ), mock.patch.object(
                release, "TOOL_COMMANDS", {}
            ):
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(
                    error_output
                ):
                    self.assertEqual(release.main(), 2)
            self.assertIn("image hash does not match", error_output.getvalue())
            self.assertEqual(release.hash_file(output), accepted_manifest_hash)

            image_manifest.write_text(
                json.dumps({"image": {"sha256": image_hash}}), encoding="utf-8"
            )
            test_result.write_text(
                json.dumps(
                    {
                        "schema": "northstar.integration-summary.v1",
                        "passed": True,
                        "image_sha256": "1" * 64,
                    }
                ),
                encoding="utf-8",
            )
            error_output = io.StringIO()
            with mock.patch.object(
                release, "parse_args", return_value=arguments
            ), mock.patch.object(
                release, "git_state", return_value=stable_git
            ), mock.patch.object(
                release, "TOOL_COMMANDS", {}
            ):
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(
                    error_output
                ):
                    self.assertEqual(release.main(), 2)
            self.assertIn("not bound to the release image", error_output.getvalue())
            self.assertEqual(release.hash_file(output), accepted_manifest_hash)

            test_result.write_text(
                json.dumps(
                    {
                        "schema": "untrusted.test-result.v1",
                        "passed": True,
                        "image_sha256": image_hash,
                    }
                ),
                encoding="utf-8",
            )
            error_output = io.StringIO()
            with mock.patch.object(
                release, "parse_args", return_value=arguments
            ), mock.patch.object(
                release, "git_state", return_value=stable_git
            ), mock.patch.object(
                release, "TOOL_COMMANDS", {}
            ):
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(
                    error_output
                ):
                    self.assertEqual(release.main(), 2)
            self.assertIn("unknown or non-image-bound schema", error_output.getvalue())
            self.assertEqual(release.hash_file(output), accepted_manifest_hash)

            test_result.write_text(
                json.dumps(
                    {
                        "schema": "northstar.integration-summary.v1",
                        "passed": False,
                        "image_sha256": image_hash,
                    }
                ),
                encoding="utf-8",
            )
            error_output = io.StringIO()
            with mock.patch.object(
                release, "parse_args", return_value=arguments
            ), mock.patch.object(
                release, "git_state", return_value=stable_git
            ), mock.patch.object(
                release, "TOOL_COMMANDS", {}
            ):
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(
                    error_output
                ):
                    self.assertEqual(release.main(), 2)
            self.assertIn("does not record passed=true", error_output.getvalue())
            self.assertEqual(release.hash_file(output), accepted_manifest_hash)

    def test_release_gate_requires_a_clean_standalone_repository(self) -> None:
        gate = load_tool_module("run_release_gate")
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "NorthstarOS"
            source.mkdir()
            subprocess.run(
                ["git", "init", "-b", "main"], cwd=source, check=True,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            subprocess.run(
                ["git", "config", "user.name", "Northstar Test"],
                cwd=source, check=True,
            )
            subprocess.run(
                ["git", "config", "user.email", "northstar@example.invalid"],
                cwd=source, check=True,
            )
            (source / "tracked.txt").write_text("tracked\n", encoding="utf-8")
            subprocess.run(["git", "add", "tracked.txt"], cwd=source, check=True)
            subprocess.run(
                ["git", "commit", "-m", "test fixture"], cwd=source, check=True,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            commit = subprocess.run(
                ["git", "rev-parse", "HEAD"], cwd=source, check=True,
                stdout=subprocess.PIPE, text=True,
            ).stdout.strip()
            with mock.patch.object(gate, "ROOT", source):
                self.assertEqual(gate.require_clean_git_repository(), commit)
                self.assertEqual(
                    gate.require_clean_git_repository(expected_commit=commit), commit
                )
                (source / "dirty.txt").write_text("dirty\n", encoding="utf-8")
                with self.assertRaisesRegex(gate.ReleaseGateError, "uncommitted"):
                    gate.require_clean_git_repository()
                (source / "dirty.txt").unlink()
                nested = source / "nested"
                nested.mkdir()
                with mock.patch.object(gate, "ROOT", nested), self.assertRaisesRegex(
                    gate.ReleaseGateError, "root of its own"
                ):
                    gate.require_clean_git_repository()

    def test_spdx_sbom_is_deterministic_commit_bound_and_fail_closed(self) -> None:
        sbom_tool = load_tool_module("gen_sbom")
        release_tool = load_tool_module("gen_release_evidence")
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "NorthstarOS"
            source.mkdir()
            subprocess.run(
                ["git", "init", "-b", "main"], cwd=source, check=True,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            subprocess.run(
                ["git", "config", "user.name", "Northstar Test"],
                cwd=source, check=True,
            )
            subprocess.run(
                ["git", "config", "user.email", "northstar@example.invalid"],
                cwd=source, check=True,
            )
            (source / ".gitignore").write_text("/artifacts/\n", encoding="utf-8")
            (source / "VERSION").write_text("0.1.0\n", encoding="ascii")
            (source / "kernel.c").write_text("int kernel;\n", encoding="utf-8")
            subprocess.run(["git", "add", "."], cwd=source, check=True)
            subprocess.run(
                ["git", "commit", "-m", "test fixture"], cwd=source, check=True,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            output = source / "artifacts" / "NorthstarOS.spdx.json"
            arguments = type(
                "Arguments",
                (),
                {
                    "source": source,
                    "output": output,
                    "version": "0.1.0",
                    "source_date_epoch": 123,
                    "require_clean": True,
                },
            )()
            tool_output = io.StringIO()
            with mock.patch.object(
                sbom_tool, "parse_args", return_value=arguments
            ), contextlib.redirect_stdout(tool_output):
                self.assertEqual(sbom_tool.main(), 0)
                first_hash = sbom_tool.sha256_file(output)
                self.assertEqual(sbom_tool.main(), 0)
            self.assertNotIn(str(source), tool_output.getvalue())
            self.assertIn("artifacts/NorthstarOS.spdx.json", tool_output.getvalue())
            self.assertEqual(sbom_tool.sha256_file(output), first_hash)
            document = json.loads(output.read_text(encoding="utf-8"))
            commit = subprocess.run(
                ["git", "rev-parse", "HEAD"], cwd=source, check=True,
                stdout=subprocess.PIPE, text=True,
            ).stdout.strip()
            self.assertEqual(document["spdxVersion"], "SPDX-2.3")
            self.assertEqual(document["packages"][0]["versionInfo"], "0.1.0")
            self.assertTrue(
                document["packages"][0]["downloadLocation"].endswith("@" + commit)
            )
            self.assertEqual(len(document["files"]), 3)
            self.assertEqual(release_tool.validate_sbom_inventory(document, source), 3)

            document["files"][0]["checksums"][1]["checksumValue"] = "0" * 64
            with self.assertRaisesRegex(release_tool.EvidenceError, "checksum mismatch"):
                release_tool.validate_sbom_inventory(document, source)

            (source / "kernel.c").write_text("int changed;\n", encoding="utf-8")
            error_output = io.StringIO()
            with mock.patch.object(
                sbom_tool, "parse_args", return_value=arguments
            ), contextlib.redirect_stderr(error_output):
                self.assertEqual(sbom_tool.main(), 2)
            self.assertIn("worktree is dirty", error_output.getvalue())


class TapParserTests(unittest.TestCase):
    def test_valid_stream_with_noise_crlf_directives_and_ansi(self) -> None:
        report = parse_tap(
            "BIOS boot\r\n"
            "\x1b[32mTAP version 13\x1b[0m\r\n"
            "1..4\r\n"
            "ok 1 - frame allocator\r\n"
            "ok 2 - optional nic # SKIP no device\r\n"
            "not ok 3 - pending hardening # TODO tracked\r\n"
            "ok 4 - scheduler\r\n"
            "NORTHSTAR:TESTS:DONE\r\n"
        )
        self.assertEqual(report.planned, 4)
        self.assertTrue(report.successful)
        self.assertEqual(report.cases[1].directive, "SKIP")
        self.assertEqual(report.cases[2].directive, "TODO")
        report.assert_success()

    def test_real_failure_is_not_hidden_by_todo_or_skip(self) -> None:
        report = parse_tap("TAP version 13\n1..1\nnot ok 1 - paging\n")
        self.assertFalse(report.successful)
        with self.assertRaisesRegex(TapError, "paging"):
            report.assert_success()

    def test_rejects_number_gap_plan_mismatch_and_unstructured_content(self) -> None:
        invalid = (
            "TAP version 13\n1..2\nok 2 - gap\n",
            "TAP version 13\n1..2\nok 1 - short\n",
            "TAP version 13\n1..1\nopaque output\nok 1 - hidden\n",
        )
        for transcript in invalid:
            with self.subTest(transcript=transcript), self.assertRaises(TapError):
                parse_tap(transcript)

    def test_bailout_is_preserved_and_fails(self) -> None:
        report = parse_tap("TAP version 13\nBail out! heap corrupt\n")
        self.assertEqual(report.bailed_out, "heap corrupt")
        with self.assertRaisesRegex(TapError, "heap corrupt"):
            report.assert_success()


class ScenarioContractTests(unittest.TestCase):
    def test_valid_mapping_and_serial_contract(self) -> None:
        scenario = Scenario.from_mapping(_valid_scenario_mapping(), source=Path("test.json"))
        scenario.verify_serial("booting\nNS:RUN:COMPLETE\n")
        with self.assertRaisesRegex(ScenarioError, "forbidden"):
            scenario.verify_serial("PANIC: bad\nNS:RUN:COMPLETE\n")

    def test_completion_sentinel_is_unique_and_terminal(self) -> None:
        scenario = Scenario.from_mapping(_valid_scenario_mapping(), source=Path("test.json"))
        with self.assertRaisesRegex(ScenarioError, "exactly once"):
            scenario.verify_serial("NS:RUN:COMPLETE\nNS:RUN:COMPLETE\n")
        with self.assertRaisesRegex(ScenarioError, "output follows"):
            scenario.verify_serial("NS:RUN:COMPLETE\nlater crash\n")

    def test_http_probe_is_loopback_only_and_content_addressed(self) -> None:
        probe = _valid_http_probe_mapping()
        mapping = _valid_scenario_mapping()
        mapping["requires"] = {"network": True, "persistence": False}
        mapping["http_probe"] = probe
        scenario = Scenario.from_mapping(mapping, source=Path("test.json"))
        self.assertIsNotNone(scenario.http_probe)
        assert scenario.http_probe is not None
        self.assertEqual(scenario.http_probe.host, "127.0.0.1")
        self.assertEqual(scenario.http_probe.expected_status, 200)

        invalid_values = {
            "ready_serial": "(",
            "host": "0.0.0.0",
            "host_port": 1023,
            "guest_port": 0,
            "path": "evidence",
            "expected_status": 600,
            "expected_body_sha256": "A" * 64,
        }
        for field, invalid in invalid_values.items():
            invalid_mapping = _valid_scenario_mapping()
            invalid_mapping["requires"] = {"network": True, "persistence": False}
            invalid_mapping["http_probe"] = dict(probe, **{field: invalid})
            with self.subTest(field=field), self.assertRaises(ScenarioError):
                Scenario.from_mapping(invalid_mapping, source=Path("test.json"))

        non_network_mapping = _valid_scenario_mapping()
        non_network_mapping["http_probe"] = probe
        with self.assertRaisesRegex(ScenarioError, "requires network=true"):
            Scenario.from_mapping(non_network_mapping, source=Path("test.json"))

    def test_schema_is_closed_and_types_are_strict(self) -> None:
        mutations = (
            ("unknown", 1),
            ("timeout_seconds", True),
            ("required_serial", "NORTHSTAR"),
            ("requires", {"network": False}),
        )
        for key, value in mutations:
            mapping = _valid_scenario_mapping()
            mapping[key] = value
            with self.subTest(key=key), self.assertRaises(ScenarioError):
                Scenario.from_mapping(mapping, source=Path("test.json"))

    def test_invalid_regex_is_rejected_before_qemu(self) -> None:
        mapping = _valid_scenario_mapping()
        mapping["required_serial"] = ["(", UNIVERSAL_COMPLETION_PATTERN]
        with self.assertRaisesRegex(ScenarioError, "invalid required_serial regex"):
            Scenario.from_mapping(mapping, source=Path("test.json"))

    def test_json_schema_and_runtime_validator_do_not_drift(self) -> None:
        with SCENARIO_SCHEMA.open("r", encoding="utf-8") as stream:
            schema = json.load(stream)
        self.assertEqual(set(schema["properties"]), _SCENARIO_KEYS)
        self.assertEqual(set(schema["required"]), _SCENARIO_REQUIRED_KEYS)
        self.assertFalse(schema["additionalProperties"])

    def test_debug_exit_defaults_to_canonical_guest_success(self) -> None:
        mapping = _valid_scenario_mapping()
        del mapping["expected_debug_exit"]
        scenario = Scenario.from_mapping(mapping, source=Path("test.json"))
        self.assertEqual(scenario.expected_debug_exit, DEFAULT_DEBUG_EXIT)

    def test_debug_exit_rejects_posix_alias_boundary(self) -> None:
        mapping = _valid_scenario_mapping()
        mapping["expected_debug_exit"] = 127
        self.assertEqual(
            Scenario.from_mapping(mapping, source=Path("test.json")).expected_debug_exit,
            127,
        )
        mapping["expected_debug_exit"] = 128
        with self.assertRaisesRegex(ScenarioError, r"\[0, 127\]"):
            Scenario.from_mapping(mapping, source=Path("test.json"))

    def test_repository_scenarios_cover_progressive_milestones(self) -> None:
        scenarios = load_scenarios()
        covered = {scenario.milestone for scenario in scenarios}
        self.assertEqual(covered, {f"M{number}" for number in range(6)})
        all_ids = scenario_ids_through(scenarios, "M5")
        self.assertEqual(len(all_ids), len(set(all_ids)))
        self.assertEqual(
            scenario_ids_through(scenarios, "M0"),
            tuple(s.id for s in scenarios if s.milestone == "M0"),
        )
        for scenario in scenarios:
            self.assertEqual(scenario.required_serial[-1], UNIVERSAL_COMPLETION_PATTERN)
            self.assertTrue(
                any("panic" in pattern.lower() for pattern in scenario.forbidden_serial),
                f"{scenario.id} must fail closed on a kernel panic marker",
            )
        probes = [scenario for scenario in scenarios if scenario.http_probe is not None]
        self.assertEqual(probes, [])
        m5 = next(scenario for scenario in scenarios if scenario.id == "m5_network_interop")
        self.assertTrue(m5.network)
        self.assertTrue(m5.persistence)
        self.assertIsNone(m5.http_probe)


if __name__ == "__main__":
    unittest.main()
