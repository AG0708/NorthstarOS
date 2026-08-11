# NorthstarOS Roadmap

Each milestone ends in an executable artifact and a headless test. Later
milestones do not erase evidence from earlier ones.

## M0: Reproducible toolchain and image

- Cross-compiled freestanding C and NASM assembly.
- Deterministic raw-disk image layout and manifest.
- Headless QEMU launcher, serial capture, timeout, and debug-exit handling.

Exit evidence: the image builds twice with identical hashes and BIOS executes
stage 1.

## M1: Independent 64-bit boot

- Stage-1 and stage-2 loaders.
- A20, E820, GDT, page tables, long mode, and higher-half kernel entry.
- Boot-information validation and serial diagnostics.

Exit evidence: kernel C entry reports the memory map and passes boot contract
self-tests without GRUB or another boot manager.

## M2: Kernel foundations

- IDT and exception handling.
- Physical-frame allocator and virtual-memory manager.
- Page-backed kernel heap.
- Timer interrupt, monotonic clock, and preemptive kernel threads.

Exit evidence: allocation stress, guard-page fault, and scheduler fairness tests
pass under repeated boots.

## M3: Isolated user space

- TSS and privilege transitions.
- Process address spaces, ELF64 loader, syscall ABI, and user-copy validation.
- `spawn`, `exec`, `wait`, `exit`, sleep, and pipe primitives.
- Freestanding user C runtime and PID 1.

Exit evidence: two ring-3 processes are preempted, communicate through a pipe,
and a deliberate user fault terminates only the offending process.

## M4: Persistent storage

- ATA PIO driver and generic block interface.
- VFS, initramfs, file descriptors, directories, and device nodes.
- NorthstarFS formatter, kernel driver, and independent checker.
- Shell and filesystem utilities.

Exit evidence: a user program creates and hashes a file, the VM reboots, the hash
is unchanged, and the independent checker accepts the image.

## M5: Networked system

- RTL8139 driver and Ethernet/ARP/IPv4/ICMP/UDP.
- DHCP and DNS clients.
- TCP state machine, client-side socket syscalls, and a Ring-3 HTTP client.

Exit evidence: the guest exchanges ICMP with the host peer, resolves a test DNS
name, a Ring-3 ELF exchanges UDP, and its TCP/HTTP request survives injected
loss/reordering before validating the host peer's exact response.

## M6: Hardening and release evidence

- Malformed-input corpus for ELF, filesystem metadata, syscalls, and packets.
- Long-running scheduler/storage/network stress.
- Reproducible release image, symbol map, architecture guide, and demo script.
- CI recipe matching local headless tests.

Exit evidence: all completion gates in `COMPLETION_GATES.md` pass from a clean
checkout, and every public claim links to raw, reproducible evidence.
