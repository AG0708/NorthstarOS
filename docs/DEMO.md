# NorthstarOS Demonstration

The canonical demonstration is a reproducible acceptance test with a human-readable
narrative. It is not a substitute for the automated suite, and a recording is not
evidence for behavior that cannot be reproduced from the displayed source revision.

This document describes the full completion demo. Run only the sections supported
by the current passing milestone, label omitted sections explicitly, and never
simulate an unavailable guest feature with host output.

## What the full demo proves

The completed sequence must demonstrate that a clean checkout:

1. builds the repository-owned BIOS boot path and x86-64 kernel;
2. boots the resulting disk image without a third-party bootloader;
3. runs isolated, preempted Ring-3 ELF processes;
4. contains a deliberate user-process fault;
5. performs real filesystem I/O through the emulated ATA device;
6. preserves exact file content across a complete VM power cycle;
7. exchanges real packets through the emulated RTL8139; and
8. retrieves and verifies host HTTP content from a Ring-3 process over TCP.

No single screenshot proves these properties. The uncut transcript, serial logs,
test artifacts, source revision, and image digest form the evidence set.

The human-facing shell demo is `make run`. The automated counterpart is
`make test-interactive`, which opens QEMU's real serial PTY, types commands,
checks ATA-backed readback, exits, and waits for PID 1 to launch a second shell.
Networking is demonstrated separately by the M5 profile.

## Preflight

Use a clean clone or a clean release worktree. Record the environment before the
build:

```sh
git status --short
git rev-parse HEAD
: "${CROSS:=x86_64-elf-}"
"${CROSS}gcc" --version
"${CROSS}ld" --version
nasm -v
python3 --version
qemu-system-x86_64 --version
make --version
```

`git status --short` must be empty for release evidence. If the checkout does not
yet have a commit, identify it as a development snapshot rather than inventing a
revision.

Build and test through the canonical interfaces:

```sh
make clean
make image
shasum -a 256 build/northstar.img
make test
make reproducibility
```

On hosts without `shasum`, use `sha256sum`. Do not paste a digest from an earlier
build. Any failed command stops the demo.

## Canonical eight-minute sequence

The exact shell commands can evolve with the user-space interface, but the
observed properties and failure behavior below are stable requirements.

### 1. Identify the artifact

Show the source revision, image digest, bootloader/kernel artifact list, and QEMU
version. Explain in one sentence that stage 1 loads repository-owned stage 2,
which obtains E820 data, constructs bootstrap page tables, enters long mode, and
transfers a validated boot structure to the higher-half kernel.

Do not spend demo time scrolling through source. Link the boot protocol and
architecture documents alongside the recording.

### 2. Boot to user space

Launch the supported QEMU command or project run target. Keep serial output
visible. The boot transcript should show, in order:

- stage-2 handoff and boot-structure validation;
- available and reserved physical memory;
- interrupt controller and timer initialization;
- root filesystem mounts;
- PID 1 loading from an ELF file; and
- a Ring-3 shell prompt.

A kernel-linked "shell" function does not satisfy this step. The executable must
be loaded through the VFS into a private user address space.

### 3. Demonstrate preemption and process primitives

From the guest shell:

- display the process list and memory summary;
- start at least four CPU-bound programs without voluntary yields;
- show that every process counter advances under timer preemption;
- run a pipeline between two external programs;
- demonstrate redirection and a background job; and
- wait for children and show their exit statuses.

The evidence should identify process IDs and elapsed ticks so progress is
observable rather than inferred from interleaved text.

### 4. Contain a user fault

Start a dedicated test program that performs a prohibited user access or invalid
instruction. Show the decoded fault, the offending process termination, and
continued progress of the shell and an unrelated process. Then run a normal
command successfully.

Never trigger a kernel panic in place of this test. The point is privilege and
fault isolation.

### 5. Prove persistence

Create a nested directory and a file large enough to cross the filesystem's
direct/indirect allocation boundary. Compute and display its digest inside the
guest. Flush or unmount through the documented interface, then fully terminate
QEMU.

Boot the same disk image again, recompute the digest, and compare it with the
first value. Display free-space and filesystem-check results. A kernel reset
without closing the VM is not a full power-cycle test, and an initramfs file is
not persistence evidence.

### 6. Demonstrate network configuration

On an isolated deterministic test network, show:

- the RTL8139 device and MAC address;
- DHCP address, route, and DNS configuration;
- ARP resolution;
- an ICMP exchange with the host test peer; and
- DNS resolution from the guest.

Packets must traverse the emulated NIC. Serial or a host-shared socket is not a
network-stack substitute.

### 7. Exchange TCP data

Run `/bin/netcheck` as a Ring-3 process and have it fetch a deterministic HTTP
response from the independent host peer. Display the HTTP status, response
digest, and guest connection state. Then run the deterministic loss/reordering
scenario and show eventual delivery or the documented bounded failure behavior.

Do not expose the guest to the public Internet; this is an interoperability test
on an isolated link.

### 8. Tie the demo to test evidence

Exit the VM cleanly and show the archived serial log plus the same source and
image hashes displayed at the start. Summarize the passing completion gates and
name the non-goals: no production-hardening claim, no physical-hardware claim,
no SMP scalability claim, and no POSIX-conformance claim.

## Scripted demo and recording policy

The automated demo runner, when added, must orchestrate existing public build
and test interfaces rather than contain private shortcuts. It should:

- abort on the first failed command;
- enforce a timeout on each VM phase;
- record the complete QEMU command line and serial stream;
- use deterministic fixture content and network fault schedules;
- verify output structurally instead of sleeping for fixed periods;
- retain logs and hashes in a timestamped artifact directory; and
- leave the working tree and source files unchanged.

Publish two recordings only after the full gate passes:

- an uncut terminal recording from clean checkout through evidence summary; and
- an optional shorter edit for a portfolio page.

The edit must not splice outputs from different revisions or imply that omitted
wait time was successful execution. Display the release tag or source hash in the
video and link raw logs next to it.

## Milestone-safe shorter demos

Before the full completion gate, use the latest honest sequence:

| Milestone | Maximum supported demo claim |
| --- | --- |
| M0 | Reproducible image construction and stage-1 execution. |
| M1 | Independent BIOS-to-long-mode boot and validated higher-half C entry. |
| M2 | Interrupts, allocation, paging, heap, and preemptive kernel threads under self-test. |
| M3 | Isolated Ring-3 ELF processes, syscalls, preemption, IPC, and user-fault containment. |
| M4 | ATA-backed VFS/NorthstarFS, shell utilities, and verified power-cycle persistence. |
| M5 | RTL8139-backed host interoperability through the implemented TCP/IP stack. |
| M6 | Full adversarial, repeated, reproducible release evidence. |

Do not narrate later rows as implemented. Planned architecture belongs in the
roadmap, not the demo title or portfolio caption.

## Failure checklist

Stop the recording and preserve artifacts if any of these occur:

- dirty or unidentified source state;
- build warning, test failure, timeout, panic, or missing completion sentinel;
- digest mismatch across the persistence or reproducibility checks;
- test fixture unexpectedly comes from initramfs or the host filesystem;
- a process-isolation test stops the kernel or unrelated process;
- traffic bypasses the emulated NIC; or
- commands or logs contain secrets or unrelated local information.

Fix the defect, add a regression test, and rerun the complete sequence. Editing a
failure out of the recording does not produce valid evidence.

## Evidence manifest

The artifact directory for a release demo should contain:

```text
revision.txt
tool-versions.txt
image.sha256
qemu-command.txt
serial-first-boot.log
serial-second-boot.log
test-summary.txt
reproducibility.txt
network-capture.pcap
limitations.txt
```

The packet capture is optional until networking exists and required for the full
M5/M6 demonstration. Logs must be scanned for secrets and unstable absolute host
paths before publication.

The release can be described only by the gates actually demonstrated. See
[`COMPLETION_GATES.md`](COMPLETION_GATES.md) for the authoritative criteria and
[`SECURITY.md`](SECURITY.md) for trust boundaries and non-claims.
