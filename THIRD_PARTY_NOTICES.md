# Third-party boundary and references

NorthstarOS does not link a third-party bootloader, kernel, libc, filesystem,
network stack, or device-driver library into its runtime image.

The host build and verification process uses external tools including GCC and
GNU binutils for the `x86_64-elf` target, NASM, Make, Python, and QEMU. These
tools are not vendored or redistributed in this repository and retain their own
licenses.

Implementation work consults public specifications rather than copying a
reference implementation. Principal specifications include:

- Intel 64 and IA-32 Software Developer's Manual;
- System V AMD64 ABI and ELF specification;
- BIOS INT 13h extensions and PC-compatible E820 memory-map interface;
- PCI Local Bus, ATA/ATAPI, and Realtek RTL8139 programming documentation;
- SVR4 `newc` archive format; and
- IETF RFC 826 (ARP), RFC 791 (IPv4), RFC 792 (ICMP), RFC 768 (UDP), RFC 793
  and its updates (TCP), RFC 2131 (DHCP), and RFC 1035 (DNS).

Generated QEMU disk images, packet captures, logs, and test results contain only
repository-owned synthetic fixtures. A release audit must add a notice here
before any third-party runtime asset or corpus is introduced.
