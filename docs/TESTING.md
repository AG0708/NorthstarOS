# Testing NorthstarOS

NorthstarOS treats tests as executable evidence. A feature is not complete because
it compiles or produces convincing serial output; it is complete only when the
appropriate positive, negative, recovery, and repeatability checks pass from a
clean checkout.

This document defines the test contract. It does not claim that every layer is
already implemented. The current passing evidence is the CI result and its
archived logs, not the roadmap.

## Quick start

The supported entry points are:

| Command | Contract |
| --- | --- |
| `make image` | Build `build/northstar.img` and all required boot artifacts. |
| `make test-host` | Run deterministic host-side unit and format tests. |
| `make test-boot` | Boot headless QEMU, enforce a timeout, validate the test protocol, and fail on any guest failure. |
| `make test` | Run the host and integration suites. This is the ordinary pre-push gate. |
| `make reproducibility` | Build twice from clean state and compare release artifacts byte for byte. |
| `make clean` | Remove generated build output. |

Run the full local gate with:

```sh
make clean
make test
make reproducibility
```

The Python host support uses only the standard library and may also be invoked
directly:

```sh
python3 -m unittest discover -s tests/host -p 'test_*.py'
```

Direct invocation is useful while developing a host tool. Native C host
harnesses and the complete host suite are compiled and run by `make test-host`.
CI and release evidence use the Make targets because they also validate
repository-level preconditions.

## Test layers

### Host unit tests

Pure logic should be exercised without booting a virtual machine whenever that
does not weaken the test. The host suite covers image-layout validation,
boot-manifest parsing, filesystem formatting and checking, binary-structure
bounds, checksums, protocol state transitions, and build invariants.

Host tests must:

- be deterministic unless a seed is printed and accepted as an input;
- use temporary directories rather than repository paths;
- compare parsed structure, not only tool exit status;
- cover minimum, maximum, truncated, overflowing, and inconsistent inputs; and
- leave every discovered bug with a minimal regression case.

### Headless boot tests

`make test-boot` launches QEMU without a graphical console, captures serial
output, and gives the VM a bounded amount of time to finish. The guest emits a
machine-readable test stream and terminates through the QEMU debug-exit device.

A boot is successful only if all of the following are true:

1. QEMU starts with the expected disk image and device model.
2. The expected suite-start record is observed.
3. Every test record is syntactically valid and reports success.
4. Exactly one final completion sentinel is observed.
5. QEMU exits with the expected debug-exit status before the timeout.
6. The host runner reports zero failures and preserves the serial log.

Text such as `booted`, `OK`, or a shell prompt is never sufficient on its own.
An unexpected QEMU exit, timeout, malformed record, duplicate sentinel, missing
sentinel, kernel panic, or guest assertion is a hard failure.

Each integration case is declared under `tests/integration/scenarios/` and must
validate against `tests/integration/scenario.schema.json` before QEMU starts.
The declaration records its milestone, timeout, required and forbidden serial
patterns, QEMU arguments, expected logical debug-exit result, and whether it
requires mutable storage or a host network peer. An unknown field or malformed
pattern fails static validation rather than being ignored.

### Kernel self-tests

Kernel tests run in the environment whose invariants they exercise. They cover
reserved-frame exclusion, duplicate allocation, map/unmap behavior, heap
coalescing, interrupt-frame decoding, timer progress, scheduler fairness, VFS
object lifetime, and packet/state-machine behavior. Destructive tests run in a
dedicated test boot, never as an optional branch after a normal boot.

Debug builds keep invariant assertions enabled. Tests must not depend on a
particular allocation address, timer phase, or incidental task ordering unless
that ordering is the contract being tested.

### Black-box integration tests

Integration tests drive observable behavior across subsystem boundaries. The
required scenarios grow with the implemented milestone:

- independent BIOS boot into the higher-half C kernel;
- multiple preempted Ring-3 ELF processes making progress;
- process creation, pipes, wait/exit, and descriptor inheritance;
- containment of invalid opcodes, divide-by-zero, and user page faults;
- file creation, exact-content hashing, power-cycle persistence, and remount;
- rejection of corrupted filesystem metadata;
- Ethernet/ARP/IPv4/ICMP/UDP/TCP interoperability with a host peer; and
- recovery under deterministic packet loss, duplication, delay, and reordering;
  and
- a separate PTY-driven interactive boot that sends commands through COM1 RX,
  verifies persistent readback, and observes PID 1 restart the shell.

An integration test must cross the real boundary named by its claim. A storage
test uses the emulated ATA device; a network test uses the emulated RTL8139; a
user-isolation test executes a separate Ring-3 ELF image. Serial injection or a
kernel-only shortcut cannot stand in for those paths.

`make test-interactive` is intentionally separate from the M5 image: it proves
the human shell path with networking disabled. `make test-boot` proves the
network path with a single isolated Ring-3 netcheck process.

### Reproducibility tests

`make reproducibility` performs two clean builds with the same nonzero
`SOURCE_DATE_EPOCH` and compares the SHA-256 digest of
`build/northstar.img`. Release verification also compares the independently
useful boot artifacts when present:

- `build/stage1.bin`
- `build/stage2.bin`
- `build/kernel.bin`
- `build/initrd.tar`
- `build/northstar.fs`

A mismatch is a release-blocking failure. The diagnostic should retain both
digests and identify the first differing artifact; it must never overwrite one
build with the other before comparison.

## Required adversarial matrix

The suite is expanded alongside each subsystem. Before a subsystem can support
a release claim, its row must have automated negative coverage.

| Boundary | Required failures and stress cases |
| --- | --- |
| Boot contract | Bad magic/version, truncated E820 map, overlapping or out-of-range extents, impossible image layout. |
| ELF loader | Wrong class/machine, truncated headers, integer overflow, overlapping segments, W+X segment, kernel-half entry point. |
| User/kernel ABI | Null, unmapped, kernel-space, wraparound, cross-page, and concurrently unmapped user buffers; unknown syscall numbers. |
| Memory | OOM, fragmentation, repeated map/unmap, reserved-frame pressure, duplicate free, guard-page access. |
| Scheduling | CPU-bound contention, sleep/wakeup races, task exit during wait, interrupt-heavy runs, repeated process churn. |
| Filesystem | Full disk, invalid bitmap/inode/extent, cycles, duplicate names, long paths, truncated image, interrupted metadata update. |
| Network | Truncated frames, invalid lengths/checksums, fragments, unknown protocols, retransmission, reset, loss, duplication, reordering. |

Untrusted-length arithmetic is tested at both sides of every boundary. Cases
must include values near `0`, the format maximum, and integer wraparound.

## Repetition and flake policy

The default suite is deterministic and runs once per pull request. Release
candidates additionally require at least 100 repeated full-suite boots across
recorded seeds, with no unexplained failures. A failure is not dismissed as a
flake. It must be reproduced or retained as an open release blocker with the
seed, image hash, serial log, QEMU command, and host configuration.

Timing assertions should express safety margins and progress invariants instead
of relying on exact wall-clock scheduling under emulation. Increasing a timeout
is not an acceptable fix without evidence that the original bound was invalid.

## Sanitizers, static analysis, and fuzzing

Code that can be compiled into a hosted harness should run under AddressSanitizer
and UndefinedBehaviorSanitizer. Freestanding code is checked with warnings as
errors and the project's static-analysis configuration. The parser and decoder
fuzz targets should include, at minimum, ELF, boot metadata, NorthstarFS, DHCP,
DNS, IPv4, TCP, and syscall argument decoding.

Sanitizer and fuzzer results support, but do not replace, QEMU tests. A hosted
harness does not reproduce page permissions, privilege transitions, interrupt
state, DMA behavior, or device ordering.

## Test artifacts

Every CI run preserves enough context to reproduce a failure:

- the exact source revision;
- compiler, linker, assembler, Python, Make, and QEMU versions;
- the image SHA-256 digest;
- serial output and host-runner output;
- the QEMU command line and timeout;
- random seeds and network fault schedules, when used; and
- failing input corpora that are safe to publish.

Do not include credentials, local paths containing usernames, or unrelated host
environment variables in logs. Release evidence is tied to a tag and immutable
artifact digest.

## Adding a test

1. Identify the invariant or externally visible contract.
2. Add the smallest positive case.
3. Add boundary and malformed-input cases.
4. Make failure observable through an exit status and structured diagnostic.
5. Confirm that temporarily reintroducing the bug makes the new test fail.
6. Run `make test` from a clean tree.
7. If generated bytes change, run `make reproducibility` as well.

Tests should explain *what property failed* and include the relevant observed and
expected values. They should not encode an implementation detail unless that
detail is itself a documented ABI or on-disk format.

## Claim discipline

The milestone and release gates in
[`COMPLETION_GATES.md`](COMPLETION_GATES.md) remain authoritative. A test name,
disabled test, planned matrix entry, or locally observed success is not public
evidence. Public claims must reference a passing, reproducible run against the
same source and image hashes.
