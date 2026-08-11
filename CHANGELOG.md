# Changelog

## 0.1.0 - 2026-08-11

- Custom BIOS bootloader and higher-half x86-64 kernel
- Paging, heap allocation, preemptive Ring-3 processes, syscalls, and
  fault isolation.
- PID 1, shell, pipes, redirection, background jobs, and utilities
- ATA PIO, VFS, initramfs, and journaled NorthstarFS
- RTL8139 and a small TCP/IP client stack
- Host tests, QEMU tests, crash-recovery tests, reproducible builds, an
  evidence manifest, and an SPDX SBOM.

Limits: QEMU-only, one CPU, separate interactive and network profiles, no UEFI,
SMP, POSIX, or production-readiness claim.
