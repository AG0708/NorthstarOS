# NorthstarOS Security Model

NorthstarOS is an educational, research-oriented operating system. It is not
production hardened, audited, POSIX conformant, or suitable for protecting real
secrets. This document defines the intended trust boundaries and the evidence
required before narrower security properties may be claimed.

Security-sensitive behavior that exists only on the roadmap is not an
implemented guarantee. Consult the current test results and release notes before
relying on any property described here.

## Supported environment

The initial supported target is a single-processor x86-64 guest under QEMU's
documented BIOS machine configuration, with the repository-owned bootloader,
ATA-compatible disk controller, RTL8139 network interface, serial console, and
debug-exit device. Behavior on physical hardware, alternate firmware, SMP,
unlisted device models, or a malicious hypervisor is outside the supported
security boundary.

## Trust boundaries

NorthstarOS initially trusts:

- the checked-out source and build toolchain;
- the generated boot image before it is started;
- CPU, firmware, QEMU, and emulated-device behavior;
- kernel code and statically linked kernel data; and
- administrators with access to the image or host process.

NorthstarOS treats the following as untrusted:

- Ring-3 executable bytes and syscall arguments;
- user virtual addresses, lengths, handles, and strings;
- persistent filesystem metadata and file contents after boot;
- every received Ethernet frame and nested protocol field;
- DHCP and DNS responses;
- device-reported lengths and status values; and
- arithmetic derived from any untrusted offset, count, or size.

The primary assets are kernel control flow, kernel memory, process isolation,
filesystem structural integrity, protocol-state integrity, and continued
progress of unrelated processes after one process fails.

## Security goals

Within the supported QEMU configuration, the implementation is designed to
provide the following testable properties:

1. A Ring-3 process cannot read, write, or execute kernel-only mappings.
2. One process cannot access another process's private mappings without an
   explicit kernel-mediated shared object.
3. User faults terminate or report against the offending process rather than
   corrupting the kernel or unrelated processes.
4. Syscalls validate pointer ranges, permissions, alignment where required, and
   integer arithmetic before dereferencing user-controlled data.
5. ELF, filesystem, and network parsers reject malformed input before committing
   state derived from it.
6. The frame allocator never exposes reserved frames and never allocates the
   same live frame twice.
7. Kernel stacks and critical mappings use non-present guard pages; writable
   memory is non-executable when CPU support and bootstrap state permit it.
8. Device I/O and DMA buffers remain bounded to kernel-owned regions.
9. Filesystem corruption is detected and mounted fail-closed when consistency
   cannot be established.
10. Panics and diagnostics expose enough state to debug a fault without dumping
    arbitrary user file contents or stale heap memory.

Each goal requires an automated positive and negative test. Design intent alone
must not be represented as a security guarantee.

## Explicit non-goals

The initial design does not claim to defend against:

- a malicious or compromised hypervisor, firmware, compiler, linker, or host;
- physical attacks, DMA-capable hardware outside the supported device model, or
  cold-boot attacks;
- speculative-execution, cache-timing, power, electromagnetic, or other side
  channels;
- Rowhammer, CPU errata, or microcode defects;
- secure boot, measured boot, image authenticity, disk encryption, or encrypted
  transport;
- denial of service by a process that exhausts a globally shared resource until
  resource accounting and limits are implemented and tested;
- complete TCP/IP robustness on hostile public networks;
- concurrent interactive networking or per-process socket ownership (the M5
  gate installs one global socket table only for `/bin/netcheck`);
- multi-user authorization, capabilities, access-control lists, or sandboxing
  beyond page-table and process separation; or
- formal verification or compliance with a security certification standard.

Networking should be performed only on an isolated test network. Do not expose a
NorthstarOS guest directly to the public Internet.

## Kernel isolation invariants

### Page tables

- Lower-half user mappings must never set `U/S=1` for kernel-owned frames.
- Kernel text is read-only and executable; read-only data is non-writable; data,
  stacks, heaps, direct-map memory, and device mappings are non-executable once
  NX is enabled.
- Page-table creation and cloning must not inherit unintended writable aliases.
- Translation invalidation is performed before a frame can be reused.
- User-supplied virtual ranges are canonical, remain entirely in the user half,
  and cannot wrap during `start + length` calculations.

### Privilege transitions

- GDT, TSS, IDT, syscall MSRs, and kernel-stack pointers are initialized before
  Ring-3 execution.
- Every entry path normalizes its saved register frame and switches to a trusted
  kernel stack before calling C code.
- Return paths validate user instruction and stack pointers and sanitize flags.
- The current ABI returns with `IRETQ`; saved user instruction/stack pointers,
  segments, and flags must pass validation before return.

### User copies

All user-memory access goes through audited copy helpers. Validation includes
canonicality, user-half containment, overflow-safe range calculation, page
presence, direction-specific permission, and faults that occur after validation.
Callers do not retain unchecked user pointers across blocking operations.

### Interrupts and concurrency

Interrupt-context code does not sleep or acquire a lock that can be held by the
interrupted context. Shared state documents its interrupt and scheduler locking
rules. Reference counts, wait queues, descriptor tables, and process teardown
must remain valid across preemption and asynchronous device completion.

## Untrusted-format rules

ELF images, disk structures, packets, and syscalls follow the same parsing
discipline:

1. Copy or read a fixed-size header into kernel-owned memory.
2. Validate magic, version, type, and minimum size.
3. Convert and validate lengths with checked addition and multiplication.
4. Prove every nested range lies within the containing object.
5. Apply a documented maximum before allocating.
6. Parse into temporary state.
7. Commit externally visible state only after complete validation.
8. Return a specific error without using partially initialized objects.

Network checks additionally validate link/IP/transport lengths against the
actual received byte count. Filesystem checks validate block ownership,
reachability, extent bounds, link counts, and directory cycles independently of
the kernel driver.

## Memory-safety approach

The kernel is written primarily in C and assembly and therefore cannot rely on
language-level memory safety. Risk is reduced through:

- small ownership-explicit interfaces;
- checked integer and range helpers at trust boundaries;
- guard pages and non-executable writable mappings;
- deterministic initialization and poisoning in debug builds;
- assertions for allocator, scheduler, VFS, and protocol invariants;
- host harnesses under AddressSanitizer and UndefinedBehaviorSanitizer;
- warnings as errors and static analysis; and
- fuzzing plus QEMU negative tests described in
  [`TESTING.md`](TESTING.md).

These measures reduce risk; they do not make the kernel memory safe.

## Filesystem durability and recovery

The NorthstarFS design records clean/dirty mount state. The kernel must refuse a
read-write mount when structural consistency cannot be established. The
independent checker is a separate implementation boundary and must not simply
reuse the kernel parser. Ordered writes alone are not treated as crash
atomicity. The implemented bounded redo journal is tested by hard-cutting QEMU
at its prepared, committed, home-written, and cleared boundaries, cold-booting
recovery, and validating the mutated raw image with the independent checker.
Claims remain scoped to those metadata transactions and to a storage stack that
honors successful flushes; torn sectors, faulty media, and unmodeled
multi-transaction crashes remain outside the evidence.

## Network exposure

The network stack must enforce bounded queues, connection counts, reassembly
policy, and retransmission limits. Unsupported IP fragmentation must be rejected
according to the documented policy. DHCP and DNS data must never be trusted as C
strings or used as allocation sizes without validation. TCP state transitions
must validate sequence and acknowledgment windows before mutating connection
state.

The user-space HTTP client demonstration is a protocol interoperability test,
not evidence that the stack is safe on a public network.

## Security verification gate

Before a release may claim process isolation or robust malformed-input handling,
CI evidence must include:

- attempted read, write, and execute access to kernel mappings from Ring 3;
- cross-process mapping attacks;
- null, kernel-half, wraparound, cross-page, and unmapped syscall buffers;
- invalid ELF class, machine, entry point, flags, ranges, and truncated tables;
- user divide-by-zero, invalid opcode, page fault, and runaway process;
- allocator exhaustion, fragmentation, guard-page access, and repeated teardown;
- corrupt and truncated filesystem images, including conflicting ownership;
- truncated and oversized packets at every implemented protocol layer;
- bad checksums, invalid state transitions, loss, duplication, and reordering;
- hosted sanitizer runs for testable components; and
- retained regression inputs for every fixed security defect.

The release notes must list unresolved security-relevant failures and disable the
affected claim. Tests must fail closed; skipping a case because a tool or feature
is unavailable is a CI failure unless the entire job is explicitly outside the
supported matrix.

## Reporting a vulnerability

For a public repository, use GitHub's private vulnerability-reporting or
security-advisory channel when it is enabled. Include the affected revision,
QEMU configuration, minimal reproducer, expected and observed behavior, and
whether the issue crosses a documented trust boundary. Avoid publishing working
exploitation details before the maintainers have had a reasonable opportunity
to reproduce and fix the issue.

Because NorthstarOS is not intended for production deployment, there is no
guaranteed response SLA. Security reports are still treated as correctness bugs
and should receive regression tests before closure.

## Safe language for public claims

Acceptable claims enumerate implemented, tested properties and point to evidence,
for example: "invalid user pointers were contained across the published negative
test corpus under QEMU." Do not describe NorthstarOS as secure, hardened,
production-ready, formally verified, or safe for real workloads.
