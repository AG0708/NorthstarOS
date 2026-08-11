#ifndef NORTHSTAR_KERNEL_H
#define NORTHSTAR_KERNEL_H

#include <northstar/base.h>

void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void kfree(void *pointer);

void serial_init(void);
void serial_putc(char c);
/* Returns the next COM1 byte, or -1 when the receive register is empty. */
int serial_getc_nonblocking(void);
void serial_write(const char *text);
void serial_flush(void);
void klog(const char *component, const char *message);
void klog_hex(const char *component, const char *label, uint64_t value);
void kernel_debug_exit(uint8_t value) NS_NORETURN;
void panic(const char *message) NS_NORETURN;

void *memcpy(void *destination, const void *source, size_t count);
void *memmove(void *destination, const void *source, size_t count);
void *memset(void *destination, int value, size_t count);
int memcmp(const void *left, const void *right, size_t count);
size_t strlen(const char *string);
size_t strnlen(const char *string, size_t maximum);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t count);
char *strncpy(char *destination, const char *source, size_t count);

#endif
