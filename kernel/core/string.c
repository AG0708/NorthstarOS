#include <northstar/kernel.h>

void *memcpy(void *destination, const void *source, size_t count) {
    unsigned char *to = destination;
    const unsigned char *from = source;
    for (size_t i = 0; i < count; ++i) {
        to[i] = from[i];
    }
    return destination;
}
void *memmove(void *destination, const void *source, size_t count) {
    unsigned char *to = destination;
    const unsigned char *from = source;
    if (to < from) {
        return memcpy(destination, source, count);
    }
    for (size_t i = count; i > 0; --i) {
        to[i - 1] = from[i - 1];
    }
    return destination;
}

void *memset(void *destination, int value, size_t count) {
    unsigned char *to = destination;
    for (size_t i = 0; i < count; ++i) {
        to[i] = (unsigned char)value;
    }
    return destination;
}

int memcmp(const void *left, const void *right, size_t count) {
    const unsigned char *a = left;
    const unsigned char *b = right;
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return (int)a[i] - (int)b[i];
        }
    }
    return 0;
}

size_t strlen(const char *string) {
    size_t length = 0;
    while (string[length] != '\0') {
        ++length;
    }
    return length;
}

size_t strnlen(const char *string, size_t maximum) {
    size_t length = 0;
    while (length < maximum && string[length] != '\0') {
        ++length;
    }
    return length;
}

int strcmp(const char *left, const char *right) {
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];
        if (a != b || a == '\0') {
            return (int)a - (int)b;
        }
    }
    return 0;
}

char *strncpy(char *destination, const char *source, size_t count) {
    size_t i = 0;
    while (i < count && source[i] != '\0') {
        destination[i] = source[i];
        ++i;
    }
    while (i < count) {
        destination[i++] = '\0';
    }
    return destination;
}
