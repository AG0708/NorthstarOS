# NorthstarOS

NorthstarOS is a freestanding x86-64 hobby operating system written in C and
assembly. It boots through its own two-stage BIOS loader, enters a higher-half
kernel, runs isolated preemptive Ring-3 ELF processes, persists files through an
ATA-backed journaled filesystem, and exchanges real packets through an emulated
RTL8139.

It is an engineering and research system, not a Linux replacement, a POSIX
implementation, or a production security boundary. The canonical target is one
`qemu64` CPU under QEMU `pc-i440fx-7.2` with TCG.

## What is implemented

- Repository-owned 512-byte stage 1 and protected/long-mode stage 2; no GRUB,
  Limine, Linux/BSD kernel, teaching OS, or binary runtime blob.
- E820-aware physical allocation, four-level paging, higher-half/direct maps,
  NX, a page-backed heap, guard-page fault recovery, and isolated user CR3s.
- PIT-driven preemptive round-robin scheduling, ELF loading, pipes, sleep,
  spawn/wait/exit, file descriptors, and a `SYSCALL`/validated-`IRETQ` ABI.
- PID 1 and a Ring-3 shell with external commands, quoting/expansion,
  pipelines, redirection, background jobs, `wait`, and exit-status propagation.
- ATA PIO, a VFS/initramfs, and NorthstarFS with checksummed metadata, direct
  and indirect blocks, a bounded redo journal, independent `mkfs`, `fsck`, and
  raw-image inspection tools.
- PCI/RTL8139 device I/O with bus-master DMA and interrupts; Ethernet, ARP,
  IPv4, ICMP, UDP, DHCP, DNS, bounded TCP, socket plumbing, and HTTP interop.

The strongest evidence is cross-boundary rather than serial text alone:

- Ring-3 tests execute separate ELF files and contain both page faults and
  privileged-instruction faults without stopping the shell.
- Filesystem tests hard-kill QEMU at four journal durability boundaries,
  cold-boot recovery, and independently inspect the mutated raw disk and exact
  file hash on the host.
- Network tests use a separate host Ethernet peer and PCAP oracle, force TCP
  loss/retransmission, inject reorder/duplicate/bad-checksum traffic, and verify
  an exact HTTP response from a Ring-3 client through the real RTL8139 path.

## Build and verify

Host requirements are an `x86_64-elf-` GCC/binutils toolchain, NASM, Make,
Python 3, and `qemu-system-x86_64`. Build the deterministic raw image with:

```sh
make image
```

Run the supported host and QEMU scenarios, the independent journal-cut matrix,
and the reproducibility check:

```sh
make test
make test-journal
make reproducibility
```

`make release` is the stricter publication path. It refuses a dirty or parent
Git worktree, verifies that `HEAD` remains unchanged, executes 100 repetitions
of every canonical scenario (700 cold boots), and emits a commit-bound evidence
manifest plus an SPDX 2.3 source SBOM.

Boot the separate interactive profile with:

```sh
make run
```

That profile attaches the Ring-3 shell to QEMU's serial console and preserves
its writable disk at `artifacts/interactive.img`. `make test-interactive`
independently drives the serial PTY, types commands, reads back an ATA-backed
file, exits the shell, and observes PID 1 restart it.

Artifacts are written under `artifacts/`; the raw bootable disk is
`build/northstar.img`. Every QEMU scenario has a hard timeout, required and
forbidden output patterns, a decoded debug-exit status, and a machine-readable
result. See [`docs/TESTING.md`](docs/TESTING.md) for the exact contract.

## System path

```text
BIOS -> stage1 -> stage2/E820/long mode -> higher-half kernel
     -> PMM/VMM/interrupts/scheduler -> Ring-3 init and shell
     -> ATA -> NorthstarFS journal -> independent host fsck
     -> RTL8139 -> Ethernet/IP/TCP -> independent host peer/PCAP
```

Subsystem boundaries, memory layout, ABI choices, on-disk behavior, and network
scope are documented in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Repository map

```text
boot/                BIOS stage 1/stage 2 and generated layout contract
kernel/arch/x86_64/  privilege, interrupts, context switching, syscall entry
kernel/mm/           physical/virtual memory and heap
kernel/proc/         processes, scheduler, ELF, user copy, IPC, descriptors
kernel/fs/           block layer, VFS, NorthstarFS
kernel/net/          Ethernet through TCP and socket adapter
kernel/drivers/      ATA PIO and RTL8139
user/                CRT, libc subset, PID 1, shell, and ELF utilities
tools/               image, QEMU, fsck, network-peer, and evidence tooling
tests/               hosted property/negative tests and black-box scenarios
docs/                architecture, ADRs, threat model, gates, and demo
```

## Claim boundary

“From scratch” means the runtime bootloader, kernel, drivers, filesystem,
protocol stack, C runtime subset, shell, and programs are implemented here.
External compilers, QEMU, public hardware/CPU/ELF/protocol specifications, and
host verification tools remain explicit dependencies.

Passing the current QEMU gates does not establish SMP/APIC support, UEFI boot,
physical-hardware portability, POSIX conformance, public-network safety, general
torn-sector tolerance, or production readiness. The authoritative claim gates
and non-claims are in
[`docs/COMPLETION_GATES.md`](docs/COMPLETION_GATES.md) and
[`docs/SECURITY.md`](docs/SECURITY.md).

The interactive profile currently disables the NIC, while the automated M5
profile grants one global socket table only to the isolated `/bin/netcheck`
process. NorthstarOS does not claim concurrent interactive networking or
per-process socket ownership yet.

## License

MIT. See [`LICENSE`](LICENSE) and
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Release history is in
[`CHANGELOG.md`](CHANGELOG.md).
