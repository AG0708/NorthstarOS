#!/usr/bin/env python3
"""Generate a deterministic SPDX 2.3 source SBOM for NorthstarOS."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
from typing import Iterable, Optional, Sequence


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_EPOCH = 1_700_000_000


class SbomError(RuntimeError):
    pass


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha1_file(path: pathlib.Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git(source: pathlib.Path, *arguments: str) -> str:
    try:
        completed = subprocess.run(
            ["git", "-C", str(source)] + list(arguments),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=20,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise SbomError("cannot inspect Git source: {}".format(error))
    if completed.returncode != 0:
        diagnostic = completed.stderr.decode("utf-8", errors="replace").strip()
        raise SbomError(
            "Git command failed: {}".format(
                diagnostic or "exit {}".format(completed.returncode)
            )
        )
    return completed.stdout.decode("utf-8", errors="surrogateescape").rstrip("\n")


def tracked_files(source: pathlib.Path) -> Iterable[pathlib.Path]:
    raw = subprocess.run(
        ["git", "-C", str(source), "ls-files", "-z"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=20,
        check=False,
    )
    if raw.returncode != 0:
        raise SbomError("cannot enumerate tracked source files")
    for encoded in sorted(item for item in raw.stdout.split(b"\x00") if item):
        relative = pathlib.Path(encoded.decode("utf-8", errors="surrogateescape"))
        if relative.is_absolute() or ".." in relative.parts:
            raise SbomError("Git returned an unsafe tracked path")
        path = source / relative
        if not path.is_file():
            raise SbomError("tracked path is not a regular file: {}".format(relative))
        yield path


def spdx_file_id(relative: str) -> str:
    digest = hashlib.sha256(relative.encode("utf-8")).hexdigest()[:24]
    return "SPDXRef-File-{}".format(digest)


def display_path(path: pathlib.Path, source: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(source.resolve()).as_posix()
    except ValueError:
        return path.name


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-date-epoch", type=int, default=None)
    parser.add_argument("--require-clean", action="store_true")
    arguments = parser.parse_args(argv)
    if arguments.source_date_epoch is None:
        try:
            arguments.source_date_epoch = int(
                os.environ.get("SOURCE_DATE_EPOCH", DEFAULT_EPOCH)
            )
        except ValueError:
            parser.error("SOURCE_DATE_EPOCH must be an integer")
    if arguments.source_date_epoch < 0:
        parser.error("--source-date-epoch must be non-negative")
    if not re.fullmatch(
        r"[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?", arguments.version
    ):
        parser.error("--version must be a semantic version")
    return arguments


def main(argv: Optional[Sequence[str]] = None) -> int:
    arguments = parse_args(argv)
    source = arguments.source.resolve()
    output = arguments.output
    if not output.is_absolute():
        output = source / output
    try:
        if not source.is_dir():
            raise SbomError("source directory does not exist")
        top_level = pathlib.Path(git(source, "rev-parse", "--show-toplevel")).resolve()
        if top_level != source:
            raise SbomError("source must be the root of its own Git repository")
        commit = git(source, "rev-parse", "--verify", "HEAD")
        if not re.fullmatch(r"[0-9a-f]{40,64}", commit):
            raise SbomError("Git HEAD is not a full object identifier")
        if arguments.require_clean:
            status = git(source, "status", "--porcelain=v1", "--untracked-files=all")
            if status:
                raise SbomError("--require-clean requested but the worktree is dirty")

        paths = list(tracked_files(source))
        if not paths:
            raise SbomError("repository contains no tracked files")
        files = []
        relationships = [
            {
                "spdxElementId": "SPDXRef-DOCUMENT",
                "relationshipType": "DESCRIBES",
                "relatedSpdxElement": "SPDXRef-Package-NorthstarOS",
            }
        ]
        verification_sha1s = []
        for path in paths:
            relative = path.relative_to(source).as_posix()
            identifier = spdx_file_id(relative)
            sha1 = sha1_file(path)
            verification_sha1s.append(sha1)
            files.append(
                {
                    "SPDXID": identifier,
                    "fileName": "./" + relative,
                    "checksums": [
                        {"algorithm": "SHA1", "checksumValue": sha1},
                        {"algorithm": "SHA256", "checksumValue": sha256_file(path)},
                    ],
                    "licenseConcluded": "NOASSERTION",
                    "licenseInfoInFiles": ["NOASSERTION"],
                    "copyrightText": "NOASSERTION",
                }
            )
            relationships.append(
                {
                    "spdxElementId": "SPDXRef-Package-NorthstarOS",
                    "relationshipType": "CONTAINS",
                    "relatedSpdxElement": identifier,
                }
            )
        package_verification = hashlib.sha1(
            "".join(sorted(verification_sha1s)).encode("ascii")
        ).hexdigest()
        created = dt.datetime.fromtimestamp(
            arguments.source_date_epoch, tz=dt.timezone.utc
        ).isoformat().replace("+00:00", "Z")
        document = {
            "SPDXID": "SPDXRef-DOCUMENT",
            "spdxVersion": "SPDX-2.3",
            "dataLicense": "CC0-1.0",
            "name": "NorthstarOS-{}".format(arguments.version),
            "documentNamespace": (
                "https://github.com/AG0708/NorthstarOS/spdx/{}".format(commit)
            ),
            "creationInfo": {
                "created": created,
                "creators": ["Tool: NorthstarOS-tools-gen_sbom.py"],
            },
            "documentDescribes": ["SPDXRef-Package-NorthstarOS"],
            "packages": [
                {
                    "SPDXID": "SPDXRef-Package-NorthstarOS",
                    "name": "NorthstarOS",
                    "versionInfo": arguments.version,
                    "downloadLocation": (
                        "git+https://github.com/AG0708/NorthstarOS.git@{}".format(
                            commit
                        )
                    ),
                    "filesAnalyzed": True,
                    "packageVerificationCode": {
                        "packageVerificationCodeValue": package_verification
                    },
                    "licenseConcluded": "MIT",
                    "licenseDeclared": "MIT",
                    "licenseInfoFromFiles": ["NOASSERTION"],
                    "copyrightText": "Copyright (c) 2026 Abhinav Gorrepati",
                }
            ],
            "files": files,
            "relationships": relationships,
        }
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_name("." + output.name + ".tmp-{}".format(os.getpid()))
        with temporary.open("w", encoding="utf-8", newline="\n") as stream:
            json.dump(document, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(str(temporary), str(output))
    except (OSError, SbomError, subprocess.TimeoutExpired) as error:
        print("gen_sbom: error: {}".format(error), file=sys.stderr)
        return 2
    print("{}  {}".format(sha256_file(output), display_path(output, source)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
