#include <northstar/kernel.h>

enum {
    COM1 = 0x3f8,
};

static inline void out8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void serial_init(void) {
    out8(COM1 + 1, 0x00);
    out8(COM1 + 3, 0x80);
    out8(COM1 + 0, 0x01);
    out8(COM1 + 1, 0x00);
    out8(COM1 + 3, 0x03);
    out8(COM1 + 2, 0xc7);
    out8(COM1 + 4, 0x0b);
}

void serial_putc(char c) {
    for (uint32_t spins = 0; spins < 1000000; ++spins) {
        if ((in8(COM1 + 5) & 0x20) != 0) {
            out8(COM1, (uint8_t)c);
            return;
        }
    }
}

int serial_getc_nonblocking(void) {
    if ((in8(COM1 + 5) & 0x01u) == 0) {
        return -1;
    }
    return (int)in8(COM1);
}

void serial_write(const char *text) {
    if (text == NULL) {
        text = "(null)";
    }
    while (*text != '\0') {
        if (*text == '\n') {
            serial_putc('\r');
        }
        serial_putc(*text++);
    }
}

void serial_flush(void) {
    /* Bit 6 means both the transmitter holding register and shift register are
       empty, so a subsequent debug-exit cannot truncate the final evidence. */
    for (uint32_t spins = 0; spins < 10000000u; ++spins) {
        if ((in8(COM1 + 5) & 0x40u) != 0) {
            return;
        }
        __asm__ volatile ("pause");
    }
}

void klog(const char *component, const char *message) {
    serial_putc('[');
    serial_write(component);
    serial_write("] ");
    serial_write(message);
    serial_putc('\n');
}

void klog_hex(const char *component, const char *label, uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    char encoded[19] = "0x0000000000000000";
    for (unsigned i = 0; i < 16; ++i) {
        encoded[17 - i] = digits[value & 0xf];
        value >>= 4;
    }
    serial_putc('[');
    serial_write(component);
    serial_write("] ");
    serial_write(label);
    serial_write(encoded);
    serial_putc('\n');
}

void panic(const char *message) {
    __asm__ volatile ("cli");
    klog("panic", message);
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
