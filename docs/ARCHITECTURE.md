# NorthstarOS Architecture

NorthstarOS is a freestanding x86-64 operating system implemented in C and
x86-64 assembly. It uses its own BIOS boot path and does not depend on GRUB,
Limine, Linux kernel code, or a teaching-OS codebase.

## Design goals

1. Make every important subsystem executable and testable under headless QEMU.
2. Prefer small, explicit mechanisms whose invariants can be audited.
3. Separate architecture-specific code from portable kernel services.
4. Treat fault recovery, isolation, and persistent-state correctness as core
   behavior rather than demo-only additions.
5. Keep the repository reproducible with a cross compiler, NASM, Make, Python,
   and QEMU as the only host requirements.

## Boot chain

The boot image begins with a 512-byte BIOS boot sector. Stage 1 preserves the
BIOS boot drive, verifies INT 13h extensions, discovers the stage-2 extent from
build-time metadata, and loads it in bounded reads without crossing a 64-KiB
segment boundary. Stage 2 verifies its header, enables and tests A20, collects
and validates the E820 physical-memory map, and gates required CPU facilities
with CPUID. BIOS reads pass through a low-memory bounce buffer before copying
kernel data above 1 MiB. Stage 2 establishes bootstrap page tables, loads a
64-bit GDT, enables PAE, NX, and long mode in the required order, then transfers
control through a versioned boot-information structure with a 16-byte-aligned
stack and cleared direction flag.

The bootloader is intentionally independent of the runtime filesystem. The
kernel and initial ramdisk occupy deterministic image extents described by a
generated manifest, while the writable filesystem begins at a fixed aligned
LBA. This keeps early boot recoverable even when the filesystem is damaged.

## Kernel layout

The kernel is linked at `0xffffffff80000000` and initially mapped both at its
physical load address and in the higher half. The direct physical-memory map
starts at `0xffff800000000000`. User virtual addresses occupy the lower canonical
half. The kernel owns the upper half and user mappings can never install writable
aliases to kernel frames.

The architecture layer owns the GDT/TSS, IDT, legacy PIC, interrupt entry stubs,
page-table primitives, context switching, userspace entry, syscall entry, and
port/MMIO access. Portable kernel code owns allocators, processes, scheduling,
VFS, filesystems, sockets, and protocol logic.

## Memory management

The physical-memory manager consumes the E820 map and maintains a bitmap of
4-KiB frames. It reserves all firmware, bootloader, kernel, initrd, page-table,
and device regions before exposing frames. The virtual-memory manager provides
page-table construction, kernel/direct-map mappings, isolated user address
spaces, permission changes, and complete teardown.

A page-backed kernel heap provides aligned allocation and coalescing. Critical
objects use typed caches only if profiling shows a need; correctness does not
depend on cache-specific behavior.

## Interrupts and time

All 256 IDT vectors have assembly entry stubs that normalize error-code and
register frames. Exceptions are decoded with useful diagnostics. The legacy PIC
is remapped and masked except where needed, and the PIT supplies the canonical
single-processor timer interrupt. Timer interrupts drive monotonic time,
sleeping, and preemptive scheduling. APIC and SMP support are outside the
current target.

## Processes and scheduler

A process owns a page-table root, file-descriptor table, signal/exit state, and
one kernel-scheduled thread in the current runtime. Each thread has a separate
kernel stack and saved architectural context. The process model can represent
additional threads, but no public multithreaded-process claim is made. The
initial scheduler is a preemptive round-robin run queue with explicit
blocked/sleeping states and idle-task accounting.

Ring-3 programs are standard ELF64 executables loaded into private address
spaces. The kernel validates ELF bounds, segment permissions, overlap, entry
address, and user-pointer ranges. System calls enter through the x86-64
`SYSCALL` mechanism on a trusted kernel stack and return through validated
`IRETQ` frames. `SYSRET` is not part of the current ABI. Every user copy is
range checked and fault-contained.

## Storage and filesystems

The block layer exposes sector-oriented asynchronous-looking operations with a
synchronous implementation first. The first hardware driver is ATA PIO because
it is deterministic in QEMU and exercises real device I/O. A memory block device
is used for isolated tests.

NorthstarFS is a small on-disk filesystem with a versioned superblock, allocation
bitmaps, fixed-size inodes, direct and indirect extents, directory entries, and
clean/dirty mount state. Metadata uses a bounded redo journal with transaction
identifiers, checksums, commit markers, and idempotent replay. The automated
hard-cut matrix terminates QEMU at four durable journal boundaries, cold-boots
recovery, and then uses independent host tools to check structure and exact file
bytes. This evidence assumes QEMU `cache=writeback` honors guest flushes and
does not cover torn sectors, faulty media, or loss of writes reported durable.
`fsck.northstar` independently validates allocation, reachability, link counts,
extent bounds, and directory structure on the host.

The VFS supplies path walking, mount points, directories, regular files, device
nodes, pipes, and per-process descriptors. The initramfs supplies the read-only
user-program tree; the validated disk filesystem is mounted at `/persist`.

## User space

The repository contains a freestanding C user library, CRT entry code, linker
script, and separate ELF programs. PID 1 launches an interactive shell. Core
programs cover filesystem inspection and mutation, process inspection, timing,
network interoperability, and an HTTP client. The shell supports
arguments, environment expansion, redirection, pipelines, background processes,
and exit-status propagation as milestones are completed.

The automated release image runs bounded milestone workloads and exits through
QEMU's debug-exit device. A separate `NORTHSTAR_INTERACTIVE=1` image attaches
`/dev/console` to COM1 receive/transmit, launches PID 1 without the test
environment, and restarts the Ring-3 shell when it exits. Its disk is writable
across `make run` invocations. The current interactive profile omits M5; the
network acceptance profile instead grants its bounded global socket table only
for the lifetime of one isolated `/bin/netcheck` process.

## Networking

The first NIC driver targets RTL8139 under QEMU. It owns a DMA-safe receive ring
and the device's four transmit address/status slots, validates frame bounds, and
reports drops and errors. PCI discovery enables I/O decoding and bus mastering,
and all device-visible buffers remain below 4 GiB.
The network stack implements Ethernet II, ARP, IPv4 validation and fragmentation
policy, ICMP echo, UDP sockets, DHCP, DNS, and a bounded-state TCP implementation.
TCP includes sequence/window validation, retransmission timers, connection setup
and teardown, buffering, and deterministic loss tests. Socket syscalls connect
user programs to protocol control blocks.

## Testing and observability

Serial output is the machine-readable test channel. Kernel and userspace tests
emit TAP-compatible records and a final sentinel before QEMU exits through the
debug-exit device. Host scripts build deterministic images, launch QEMU without a
GUI, enforce timeouts, and archive serial logs.

Verification is layered:

- host unit tests for pure parsers, image builders, and filesystem checking;
- kernel self-tests for allocators, paging, scheduling, VFS, and protocols;
- boot integration tests that reach ring 3 and execute ELF programs;
- persistence tests across a forced QEMU reboot, externally checked from the
  raw disk image rather than trusted from serial output;
- Ring-3 socket/HTTP interoperability against an independent host-side Ethernet
  peer with a separate PCAP oracle;
- negative tests for malformed ELF, invalid syscalls, user faults, corrupt
  filesystem metadata, packet truncation, loss, duplication, and reordering.

Benchmarks are reported only with the exact command, QEMU version, guest
configuration, warm-up, repetitions, and raw logs. No performance claim is part
of a completion gate.

The canonical correctness machine is QEMU's versioned `pc-i440fx-7.2` machine
with one `qemu64` CPU, 128 MiB RAM, single-threaded TCG, an explicit PIIX IDE raw
disk, RTL8139, 16550 serial, and ISA debug-exit on port `0xf4`. Compatibility with
other machines, accelerators, SMP, APIC modes, or physical hardware is a separate
gate and is never inferred from this target.
