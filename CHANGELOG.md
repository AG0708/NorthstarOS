# Changelog

All notable changes to NorthstarOS are recorded here. Release claims remain
bounded by the tagged source, checksummed artifacts, and the evidence manifest.

## 0.1.0 - 2026-08-11

Initial public release of the freestanding x86-64 hobby operating system.

### Implemented

- Repository-owned two-stage BIOS boot path, E820 discovery, long-mode entry,
  higher-half kernel, physical/virtual memory managers, and page-backed heap.
- Timer-preempted Ring-3 ELF processes, validated syscall/user-copy boundary,
  PID 1, shell, pipelines, redirection, background jobs, and external programs.
- ATA PIO, VFS/initramfs, and NorthstarFS with checksummed metadata, direct and
  indirect blocks, bounded redo journal, and independent host tools.
- RTL8139 PCI/DMA/interrupt driver and bounded Ethernet, ARP, IPv4, ICMP, UDP,
  DHCP, DNS, TCP, socket, and Ring-3 HTTP-client paths.
- Headless QEMU, hard-cut recovery, PCAP-backed interoperability, PTY-driven
  interactive, adversarial-input, reproducibility, and release-evidence gates.
- Deterministic SPDX 2.3 source SBOM and SHA-256-bound release evidence.

### Boundaries

- Verified only on one `qemu64` CPU under QEMU `pc-i440fx-7.2` with TCG.
- The automated networking profile and interactive shell profile are separate.
- The socket table is global and granted only to the isolated network test.
- No UEFI, APIC/SMP, POSIX conformance, real-hardware portability, general
  torn-sector tolerance, or production-security claim.
