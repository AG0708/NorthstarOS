#!/usr/bin/env python3
"""Compile and execute the freestanding memory algorithms as host tests."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HOST_CC = shutil.which(os.environ.get("HOST_CC", "cc"))


@unittest.skipUnless(HOST_CC, "a native C compiler is required")
class MemoryHostTests(unittest.TestCase):
    def compile_and_run(self, name: str, sources: list[str], *, pthread: bool = False) -> None:
        with tempfile.TemporaryDirectory(prefix="northstar-mm-test-") as temporary:
            executable = Path(temporary) / name
            command = [
                str(HOST_CC),
                "-std=c17",
                "-O2",
                "-g",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Werror",
                "-Iinclude",
            ]
            if pthread:
                command.append("-pthread")
            command.extend(sources)
            command.extend(["-o", str(executable)])
            compiled = subprocess.run(
                command,
                cwd=ROOT,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=30,
            )
            self.assertEqual(
                compiled.returncode,
                0,
                compiled.stderr.decode("utf-8", errors="replace"),
            )
            completed = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=30,
            )
            self.assertEqual(
                completed.returncode,
                0,
                completed.stderr.decode("utf-8", errors="replace"),
            )
            self.assertIn(
                f"{name}: ok", completed.stdout.decode("utf-8", errors="replace")
            )

    def test_pmm(self) -> None:
        self.compile_and_run(
            "test_mm_pmm",
            ["tests/host/test_mm_pmm.c", "kernel/mm/pmm.c"],
        )

    def test_vmm(self) -> None:
        self.compile_and_run(
            "test_mm_vmm",
            [
                "tests/host/test_mm_vmm.c",
                "kernel/mm/pmm.c",
                "kernel/mm/vmm.c",
            ],
        )

    def test_heap(self) -> None:
        self.compile_and_run(
            "test_mm_heap",
            [
                "tests/host/test_mm_heap.c",
                "kernel/mm/heap.c",
                "kernel/mm/pmm.c",
                "kernel/mm/vmm.c",
            ],
            pthread=True,
        )


if __name__ == "__main__":
    unittest.main()
