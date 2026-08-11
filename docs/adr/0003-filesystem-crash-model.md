# ADR 0003: Gate crash-consistency claims on journal-cut evidence

- Status: accepted; bounded redo journal and four-boundary cut matrix implemented
- Date: 2026-08-11

## Context

Writing data before metadata is useful ordering, but it does not make a
multi-block filesystem update atomic across arbitrary power loss. QEMU process
termination also does not model every torn-sector or volatile-controller-cache
failure.

## Decision

NorthstarFS uses a bounded redo metadata journal with transaction identifiers,
record checksums, a durable commit marker, idempotent replay, and systematic
failpoints around its durable phases. ATA flush failures propagate to callers.
A host-side checker independently validates the raw image after guest writes and
after cold recovery.

## Consequences

- M4 can demonstrate real device I/O, persistent state, and bounded journal
  recovery without claiming torn-sector or faulty-media tolerance.
- A malformed or unrecoverable journal fails closed.
- Future journaling changes require an on-disk format version and migration or
  explicit incompatibility.
