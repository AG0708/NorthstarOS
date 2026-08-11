# Honest Completion Gates

NorthstarOS may be described as a **working freestanding x86-64 hobby operating
system with the capabilities enumerated below** only when gates G0-G8 pass from
a clean checkout. Until then it must be described by the latest passing
milestone, never by planned features. The word "complete" is not a substitute
for listing those capabilities and is not a claim of production readiness.

## Provenance and dependency boundary

"From scratch" has a narrow, auditable meaning in this repository: all runtime
bootloader, kernel, driver, protocol-stack, filesystem, C runtime, shell, and
user-program implementation is repository-owned. No GRUB, Limine, Linux/BSD
kernel code, pre-existing teaching OS, lwIP, libc, or third-party filesystem is
linked into the image. Public processor, device, executable-format, and network
protocol specifications may be used as references.

Host development still depends on ordinary tools: an x86-64 ELF cross compiler
and binutils, NASM, Make, Python, and QEMU. Those tools translate, assemble,
package, and emulate the system; they are not shipped as its runtime. Small
host-side validation scripts may use the Python standard library. Any future
vendored runtime component must be identified explicitly and would narrow the
from-scratch claim.

## G0: Reproducible build

- The documented toolchain creates a bootable raw image non-interactively.
- Two clean builds with `SOURCE_DATE_EPOCH` set produce identical image hashes.
- No downloaded binary blob is required to build or boot.

## G1: Self-hosted boot path

- The BIOS loads repository-owned stage 1 and stage 2 code.
- Stage 2 obtains E820 data, enters long mode, and transfers a validated boot
  structure to a higher-half C kernel.
- No third-party bootloader or pre-existing kernel participates.

## G2: Kernel isolation and memory

- The frame allocator never returns reserved or duplicate frames in stress tests.
- User mappings cannot access kernel pages.
- A kernel guard-page fault produces a decoded diagnostic; a user page fault
  terminates only that process.

## G3: Preemptive processes

- At least four CPU-bound ring-3 processes make progress without voluntary
  yielding.
- ELF files are loaded from the VFS, not linked into the kernel.
- Spawn/exec, wait/exit, pipes, sleep, and file-descriptor inheritance pass
  deterministic tests.

## G4: Persistent filesystem

- Files and nested directories survive a complete QEMU power cycle.
- Files cross direct/indirect allocation boundaries and preserve exact hashes.
- An independent checker detects seeded bitmap, inode, extent, and directory
  corruption; the kernel rejects unsafe mounts.

## G5: Interactive user environment

- PID 1 launches a ring-3 shell from an ELF file.
- The shell can run external programs, pipelines, redirection, background jobs,
  and report exit status.
- A PTY-driven QEMU gate types commands through the real serial receive path,
  reads back a persistent file, exits the shell, and observes PID 1 restart it.
- Core utilities exercise process, file, time, and network syscalls.

## G6: Real device I/O

- Persistent storage passes through an ATA-compatible emulated controller.
- Network traffic passes through an emulated RTL8139 NIC using DMA and device
  interrupts, not a host-side shortcut.
- Serial is used only for console/test reporting, not to fake storage or network.

## G7: Network interoperability

- Guest and host exchange ICMP echo traffic.
- DHCP configuration and DNS resolution work against deterministic test peers.
- A Ring-3 socket program exchanges UDP and TCP data with host peers.
- A Ring-3 HTTP client retrieves and verifies an exact response from the host
  peer through the guest TCP stack.
- Loss, duplication, and reordering tests exercise recovery without memory or
  state corruption.

## G8: Release evidence

- One command runs the headless suite with enforced timeouts and a nonzero exit
  on any failed assertion.
- The suite includes repeated boots, persistence, malformed inputs, and stress.
- The release records source hash, tool versions, image hash, logs, and limits.
- README claims match the gates; planned or partial features are labeled as such.

## Explicit non-claims

Passing these gates does not imply POSIX conformance, production security,
hardware portability beyond documented targets, SMP scalability, or parity with
general-purpose operating systems. Those claims require separate specifications
and evidence.
