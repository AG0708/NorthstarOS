# ADR 0002: Versioned single-core QEMU correctness target

- Status: accepted
- Date: 2026-08-11

## Context

Results from an implicit QEMU default machine are not reproducible across host
versions. Developing APIC, SMP, AHCI, and modern PCIe support simultaneously
would also obscure failures in the first correctness milestones.

## Decision

The canonical test target is:

- `pc-i440fx-7.2`, `qemu64`, one CPU, 128 MiB memory;
- single-threaded TCG acceleration;
- explicit PIIX IDE raw disk and RTL8139 network interface;
- 16550 serial output, no GUI, no monitor, and no automatic reboot;
- ISA debug-exit at port `0xf4` as the test-control channel.

The exact QEMU command and version are captured with each evidence run. Serial
TAP records can explain a failure, but external host checks remain authoritative
for disk persistence and network interoperability.

## Consequences

- CI and local runs have a stable hardware contract.
- Passing tests do not imply SMP, APIC, q35/AHCI, acceleration, or physical-
  hardware compatibility.
- Those additional targets can be added later as distinct compatibility gates.
