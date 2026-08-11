# Security policy

NorthstarOS is an experimental hobby operating system. It is not production
hardened and should not be used to protect real secrets or exposed to an
untrusted network.

Please report a suspected vulnerability through GitHub private vulnerability
reporting when it is enabled. Include the affected revision, canonical QEMU
configuration, a minimal reproducer, and the observed trust-boundary crossing.
Do not include credentials, private data, or destructive payloads.

Only the latest tagged release is supported for triage. There is no response
SLA. The complete threat model, supported environment, testable isolation
properties, and non-goals are in [`docs/SECURITY.md`](docs/SECURITY.md).
