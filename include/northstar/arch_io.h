#ifndef NORTHSTAR_ARCH_IO_H
#define NORTHSTAR_ARCH_IO_H

#include <northstar/base.h>

/* x86 port I/O.  The compiler barrier is intentional: device accesses may
 * order ordinary memory owned by the same device protocol. */
static inline uint8_t arch_in8(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline uint16_t arch_in16(uint16_t port)
{
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline uint32_t arch_in32(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline void arch_out8(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline void arch_out16(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline void arch_out32(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline void arch_io_wait(void)
{
    arch_out8(0x80u, 0);
}

static inline void arch_ins16(uint16_t port, void *destination, size_t words)
{
    __asm__ volatile("cld; rep insw"
                     : "+D"(destination), "+c"(words)
                     : "d"(port)
                     : "memory");
}

static inline void arch_outs16(uint16_t port, const void *source, size_t words)
{
    __asm__ volatile("cld; rep outsw"
                     : "+S"(source), "+c"(words)
                     : "d"(port)
                     : "memory");
}

#endif
