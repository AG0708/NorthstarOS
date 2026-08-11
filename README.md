# NorthstarOS

NorthstarOS is a freestanding x86-64 hobby OS written in C and assembly. It
uses a custom BIOS bootloader and targets QEMU `pc-i440fx-7.2` with one
`qemu64` CPU.

## Features

- Two-stage BIOS bootloader, E820 memory map, long mode, and higher-half kernel
- Physical and virtual memory managers, four-level paging, NX, and kernel heap
- Preemptive Ring-3 ELF processes, syscalls, pipes, file descriptors, and fault
  isolation
- PID 1, shell, pipelines, redirection, background jobs, and basic utilities
- ATA PIO, VFS, initramfs, and journaled NorthstarFS with `mkfs` and `fsck`
- RTL8139, Ethernet, IPv4, ICMP, UDP, DHCP, DNS, TCP, and HTTP client support

## Verification

- Strict and UBSan host tests for memory, process, storage, and network code
- QEMU tests for boot, paging, preemption, Ring 3, user faults, and syscalls
- Four forced shutdown points in the filesystem journal, followed by cold-boot
  recovery and an independent host `fsck`
- External Ethernet peer and PCAP checks with packet loss, retransmission,
  reordering, duplication, and invalid checksums

The v0.1.0 release passed 700/700 cold boots: 100 runs of each supported
scenario.

## Build and test

Requirements: `x86_64-elf-` GCC/binutils, NASM, Make, Python 3, and
`qemu-system-x86_64`.

```sh
make image
make test
make test-journal
make reproducibility
```

Run the separate interactive shell image with:

```sh
make run
```

`make release` requires a clean Git tree, runs the full test set and 700 cold
boots, then writes the evidence manifest and SPDX SBOM under `artifacts/`.
The bootable image is `build/northstar.img`.

## Layout

```text
boot/                BIOS bootloader
kernel/arch/x86_64/  CPU, interrupts, context switches, syscalls
kernel/mm/           physical and virtual memory, heap
kernel/proc/         processes, scheduler, ELF loader, IPC
kernel/fs/           block layer, VFS, NorthstarFS
kernel/net/          Ethernet through TCP and sockets
kernel/drivers/      ATA PIO and RTL8139
user/                libc subset, PID 1, shell, utilities
tools/               image, QEMU, fsck, peer, and release tools
tests/               host and QEMU tests
docs/                design, testing, security, and roadmap
```

## Scope

"From scratch" covers the bootloader, kernel, drivers, filesystem, network
stack, libc subset, shell, and user programs. The compiler, QEMU, specifications,
and host test tools are external dependencies.

Current limits:

- QEMU is the only verified target; there is no UEFI, APIC, or SMP support.
- The interactive and network images are separate profiles.
- Networking uses one global socket table for the test client.
- The project does not claim POSIX compliance, real-hardware support, or
  production readiness.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md),
[`docs/TESTING.md`](docs/TESTING.md), and
[`docs/SECURITY.md`](docs/SECURITY.md) for details.

## License

MIT. See [`LICENSE`](LICENSE) and
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
