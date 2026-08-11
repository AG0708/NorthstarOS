#!/usr/bin/env python3
"""Host-side structural and negative tests for the independent BIOS loader."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOOT = ROOT / "boot"
NASM = shutil.which("nasm")


DEFAULT_LAYOUT = {
    "BOOT_SECTOR_SIZE": 512,
    "STAGE2_LBA": 1,
    "STAGE2_SECTORS": 32,
    "KERNEL_LBA": 128,
    "KERNEL_SECTORS": 2,
    "KERNEL_BYTES": 513,
    "KERNEL_MEMORY_BYTES": 0x300000,
    "KERNEL_LOAD_ADDR": 0x200000,
    "KERNEL_VIRT_ADDR": 0xFFFFFFFF80000000,
    "KERNEL_ENTRY": 0xFFFFFFFF80000000,
    "INITRD_LBA": 4096,
    "INITRD_SECTORS": 0,
    "INITRD_BYTES": 0,
    "INITRD_LOAD_ADDR": 0x400000,
    "FS_LBA": 32768,
    "FS_SECTORS": 0,
}


def render_layout(overrides: dict[str, int] | None = None) -> str:
    values = dict(DEFAULT_LAYOUT)
    values.update(overrides or {})
    return "".join(f"%define {name:<24} 0x{value:x}\n" for name, value in values.items())


@unittest.skipUnless(NASM, "NASM is required for boot artifact tests")
class BootArtifactTests(unittest.TestCase):
    def assemble(
        self, source_name: str, overrides: dict[str, int] | None = None
    ) -> tuple[subprocess.CompletedProcess[bytes], bytes]:
        with tempfile.TemporaryDirectory(prefix="northstar-boot-test-") as temporary:
            directory = Path(temporary)
            (directory / "boot_layout.inc").write_bytes(
                render_layout(overrides).encode("ascii")
            )
            output = directory / (source_name + ".bin")
            completed = subprocess.run(
                [
                    str(NASM),
                    "-f",
                    "bin",
                    "-I" + str(directory) + "/",
                    "-I" + str(BOOT) + "/",
                    "-o",
                    str(output),
                    str(BOOT / source_name),
                ],
                cwd=ROOT,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=10,
            )
            data = output.read_bytes() if output.is_file() else b""
            return completed, data

    def assert_assembly_rejected(
        self, overrides: dict[str, int], diagnostic_fragment: bytes
    ) -> None:
        completed, data = self.assemble("stage2.asm", overrides)
        self.assertNotEqual(completed.returncode, 0)
        self.assertEqual(data, b"")
        self.assertIn(diagnostic_fragment, completed.stderr)

    def test_stage1_is_exact_bios_sector_with_loader_contract(self) -> None:
        completed, image = self.assemble("stage1.asm")
        self.assertEqual(completed.returncode, 0, completed.stderr.decode(errors="replace"))
        self.assertEqual(len(image), 512)
        self.assertEqual(image[510:], b"\x55\xaa")
        self.assertIn(b"NS:BOOT:S1:PASS\r\n\0", image)
        self.assertIn(b"NS:BOOT:FAIL EDD\r\n\0", image)
        self.assertIn(b"NS:BOOT:FAIL READ\r\n\0", image)
        self.assertIn(b"NS:BOOT:FAIL S2MAGIC\r\n\0", image)

    def test_stage2_header_and_diagnostic_contract(self) -> None:
        completed, image = self.assemble("stage2.asm")
        self.assertEqual(completed.returncode, 0, completed.stderr.decode(errors="replace"))
        self.assertLessEqual(len(image), 32 * 512)
        self.assertEqual(image[:3], b"\xeb\x0d\x90")
        self.assertEqual(image[3:7], b"NST2")
        for marker in (
            b"NS:BOOT:S2:START\n\0",
            b"NS:BOOT:S2:LONG_MODE\n\0",
            b"NS:BOOT:FAIL S2_CPU\n\0",
            b"NS:BOOT:FAIL S2_A20\n\0",
            b"NS:BOOT:FAIL S2_E820\n\0",
            b"NS:BOOT:FAIL S2_LAYOUT\n\0",
            b"NS:BOOT:FAIL S2_DISK\n\0",
        ):
            self.assertIn(marker, image)

    def test_stage2_accepts_maximum_staging_extent(self) -> None:
        completed, image = self.assemble(
            "stage2.asm",
            {
                "KERNEL_SECTORS": (0x80000 - 0x10000) // 512,
                "KERNEL_BYTES": 0x80000 - 0x10000,
                "KERNEL_MEMORY_BYTES": 0x600000,
            },
        )
        self.assertEqual(completed.returncode, 0, completed.stderr.decode(errors="replace"))
        self.assertTrue(image)

    def test_stage2_rejects_staging_overflow(self) -> None:
        self.assert_assembly_rejected(
            {
                "KERNEL_SECTORS": (0x80000 - 0x10000) // 512 + 1,
                "KERNEL_BYTES": 0x80000 - 0x10000 + 1,
                "KERNEL_MEMORY_BYTES": 0x80000 - 0x10000 + 1,
            },
            b"does not fit in the conventional-memory staging area",
        )

    def test_stage2_reuses_bounce_area_for_kernel_and_initrd(self) -> None:
        capacity = 0x80000 - 0x10000
        completed, image = self.assemble(
            "stage2.asm",
            {
                "KERNEL_SECTORS": capacity // 512,
                "KERNEL_BYTES": capacity,
                "KERNEL_MEMORY_BYTES": 0x600000,
                "INITRD_SECTORS": capacity // 512,
                "INITRD_BYTES": capacity,
                "INITRD_LOAD_ADDR": 0x800000,
            },
        )
        self.assertEqual(completed.returncode, 0, completed.stderr.decode(errors="replace"))
        self.assertTrue(image)

    def test_stage2_rejects_initrd_staging_overflow(self) -> None:
        capacity = 0x80000 - 0x10000
        self.assert_assembly_rejected(
            {
                "INITRD_SECTORS": capacity // 512 + 1,
                "INITRD_BYTES": capacity + 1,
            },
            b"initrd does not fit in the conventional-memory staging area",
        )

    def test_stage2_rejects_truncated_sector_count(self) -> None:
        self.assert_assembly_rejected(
            {"KERNEL_SECTORS": 1, "KERNEL_BYTES": 513},
            b"KERNEL_SECTORS truncates KERNEL_BYTES",
        )

    def test_stage2_rejects_unaligned_physical_load(self) -> None:
        self.assert_assembly_rejected(
            {"KERNEL_LOAD_ADDR": 0x210000}, b"must be 2-MiB aligned"
        )

    def test_stage2_rejects_memory_extent_smaller_than_file(self) -> None:
        self.assert_assembly_rejected(
            {"KERNEL_BYTES": 513, "KERNEL_MEMORY_BYTES": 512},
            b"must cover the complete raw kernel",
        )


if __name__ == "__main__":
    unittest.main()
