#include <northstar_user.h>

#include <stdarg.h>

void *memset(void *destination, int value, size_t length)
{
    unsigned char *output = destination;
    while (length-- != 0)
        *output++ = (unsigned char)value;
    return destination;
}

void *memcpy(void *destination, const void *source, size_t length)
{
    unsigned char *output = destination;
    const unsigned char *input = source;
    while (length-- != 0)
        *output++ = *input++;
    return destination;
}

void *memmove(void *destination, const void *source, size_t length)
{
    unsigned char *output = destination;
    const unsigned char *input = source;
    if (output < input) {
        while (length-- != 0)
            *output++ = *input++;
    } else if (output > input) {
        while (length-- != 0)
            output[length] = input[length];
    }
    return destination;
}

int memcmp(const void *left, const void *right, size_t length)
{
    const unsigned char *a = left;
    const unsigned char *b = right;
    while (length-- != 0) {
        if (*a != *b)
            return *a < *b ? -1 : 1;
        ++a;
        ++b;
    }
    return 0;
}

size_t strlen(const char *string)
{
    size_t length = 0;
    while (string[length] != '\0')
        ++length;
    return length;
}

size_t strnlen(const char *string, size_t maximum)
{
    size_t length = 0;
    while (length < maximum && string[length] != '\0')
        ++length;
    return length;
}

int strcmp(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t length)
{
    while (length != 0 && *left != '\0' && *left == *right) {
        ++left;
        ++right;
        --length;
    }
    if (length == 0)
        return 0;
    return (unsigned char)*left - (unsigned char)*right;
}

char *strcpy(char *destination, const char *source)
{
    char *result = destination;
    do {
        *destination++ = *source;
    } while (*source++ != '\0');
    return result;
}

char *strncpy(char *destination, const char *source, size_t length)
{
    char *result = destination;
    while (length != 0 && *source != '\0') {
        *destination++ = *source++;
        --length;
    }
    while (length-- != 0)
        *destination++ = '\0';
    return result;
}

char *strchr(const char *string, int character)
{
    for (;;) {
        if (*string == (char)character)
            return (char *)string;
        if (*string == '\0')
            return NULL;
        ++string;
    }
}

static int digit_value(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'z')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'Z')
        return character - 'A' + 10;
    return -1;
}

long strtol(const char *string, char **end, int base)
{
    const char *cursor = string;
    unsigned long value = 0;
    int negative = 0;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n')
        ++cursor;
    if (*cursor == '+' || *cursor == '-') {
        negative = *cursor == '-';
        ++cursor;
    }
    if ((base == 0 || base == 16) && cursor[0] == '0' &&
        (cursor[1] == 'x' || cursor[1] == 'X')) {
        base = 16;
        cursor += 2;
    } else if (base == 0) {
        base = cursor[0] == '0' ? 8 : 10;
    }
    if (base < 2 || base > 36) {
        errno = NS_EINVAL;
        if (end != NULL)
            *end = (char *)string;
        return 0;
    }
    const char *digits = cursor;
    for (;;) {
        int digit = digit_value(*cursor);
        if (digit < 0 || digit >= base)
            break;
        value = value * (unsigned)base + (unsigned)digit;
        ++cursor;
    }
    if (end != NULL)
        *end = (char *)(cursor == digits ? string : cursor);
    return negative ? -(long)value : (long)value;
}

#define ALLOC_MAGIC 0x4e53544152414c4cull
#define ALLOC_GROWTH (64u * 1024u)

struct allocation {
    uint64_t magic;
    size_t size;
    struct allocation *previous;
    struct allocation *next;
    int free;
};

static struct allocation *allocation_head;
static struct allocation *allocation_tail;

static size_t align_size(size_t value)
{
    if (value > SIZE_MAX - 15u)
        return 0;
    return (value + 15u) & ~(size_t)15u;
}

static void split_allocation(struct allocation *block, size_t size)
{
    struct allocation *tail;
    if (block->size < size + sizeof(*block) + 32u)
        return;
    tail = (struct allocation *)((unsigned char *)(block + 1) + size);
    tail->magic = ALLOC_MAGIC;
    tail->size = block->size - size - sizeof(*block);
    tail->previous = block;
    tail->next = block->next;
    tail->free = 1;
    if (tail->next != NULL)
        tail->next->previous = tail;
    else
        allocation_tail = tail;
    block->next = tail;
    block->size = size;
}

static struct allocation *grow_heap(size_t requested)
{
    size_t total = requested + sizeof(struct allocation);
    size_t growth = total < ALLOC_GROWTH ? ALLOC_GROWTH : total;
    struct allocation *block;
    growth = (growth + 4095u) & ~(size_t)4095u;
    block = sbrk((intptr_t)growth);
    if (block == (void *)-1)
        return NULL;
    block->magic = ALLOC_MAGIC;
    block->size = growth - sizeof(*block);
    block->previous = allocation_tail;
    block->next = NULL;
    block->free = 1;
    if (allocation_tail != NULL)
        allocation_tail->next = block;
    else
        allocation_head = block;
    allocation_tail = block;
    return block;
}

void *malloc(size_t size)
{
    struct allocation *block;
    if (size == 0)
        size = 1;
    size = align_size(size);
    if (size == 0) {
        errno = NS_ENOMEM;
        return NULL;
    }
    block = allocation_head;
    while (block != NULL && (!block->free || block->size < size))
        block = block->next;
    if (block == NULL)
        block = grow_heap(size);
    if (block == NULL)
        return NULL;
    split_allocation(block, size);
    block->free = 0;
    return block + 1;
}

static int physically_adjacent(const struct allocation *left,
                               const struct allocation *right)
{
    return (const unsigned char *)(left + 1) + left->size ==
           (const unsigned char *)right;
}

static void merge_next(struct allocation *block)
{
    struct allocation *next = block->next;
    if (next == NULL || !next->free || !physically_adjacent(block, next))
        return;
    block->size += sizeof(*next) + next->size;
    block->next = next->next;
    if (block->next != NULL)
        block->next->previous = block;
    else
        allocation_tail = block;
    next->magic = 0;
}

void free(void *pointer)
{
    struct allocation *block;
    if (pointer == NULL)
        return;
    block = (struct allocation *)pointer - 1;
    if (block->magic != ALLOC_MAGIC || block->free) {
        debug_log("allocator corruption\n", 21);
        _exit(127);
    }
    block->free = 1;
    merge_next(block);
    if (block->previous != NULL && block->previous->free &&
        physically_adjacent(block->previous, block)) {
        block = block->previous;
        merge_next(block);
    }
}

void *calloc(size_t count, size_t size)
{
    void *pointer;
    if (count != 0 && size > SIZE_MAX / count) {
        errno = NS_ENOMEM;
        return NULL;
    }
    size *= count;
    pointer = malloc(size);
    if (pointer != NULL)
        memset(pointer, 0, size);
    return pointer;
}

void *realloc(void *pointer, size_t size)
{
    struct allocation *block;
    void *replacement;
    if (pointer == NULL)
        return malloc(size);
    if (size == 0) {
        free(pointer);
        return NULL;
    }
    block = (struct allocation *)pointer - 1;
    if (block->magic != ALLOC_MAGIC || block->free) {
        errno = NS_EINVAL;
        return NULL;
    }
    size = align_size(size);
    if (size == 0)
        return NULL;
    if (block->size >= size) {
        split_allocation(block, size);
        return pointer;
    }
    if (block->next != NULL && block->next->free &&
        physically_adjacent(block, block->next) &&
        block->size + sizeof(*block) + block->next->size >= size) {
        merge_next(block);
        split_allocation(block, size);
        block->free = 0;
        return pointer;
    }
    replacement = malloc(size);
    if (replacement == NULL)
        return NULL;
    memcpy(replacement, pointer, block->size);
    free(pointer);
    return replacement;
}

struct format_sink {
    int fd;
    char *buffer;
    size_t capacity;
    size_t position;
    int failed;
};

static void sink_character(struct format_sink *sink, char character)
{
    if (sink->buffer != NULL) {
        if (sink->capacity != 0 && sink->position + 1u < sink->capacity)
            sink->buffer[sink->position] = character;
    } else if (!sink->failed) {
        int64_t result = write(sink->fd, &character, 1);
        if (result != 1)
            sink->failed = 1;
    }
    ++sink->position;
}

static void sink_string(struct format_sink *sink, const char *string,
                        size_t length)
{
    for (size_t index = 0; index < length; ++index)
        sink_character(sink, string[index]);
}

static void format_unsigned(struct format_sink *sink, uint64_t value,
                            unsigned base, int width, char padding,
                            int uppercase)
{
    char digits[32];
    size_t count = 0;
    const char *alphabet = uppercase ? "0123456789ABCDEF" :
                                       "0123456789abcdef";
    do {
        digits[count++] = alphabet[value % base];
        value /= base;
    } while (value != 0);
    while ((int)count < width) {
        sink_character(sink, padding);
        --width;
    }
    while (count != 0)
        sink_character(sink, digits[--count]);
}

static int format(struct format_sink *sink, const char *format_string,
                  va_list arguments)
{
    while (*format_string != '\0') {
        int width = 0;
        char padding = ' ';
        int long_value = 0;
        if (*format_string != '%') {
            sink_character(sink, *format_string++);
            continue;
        }
        ++format_string;
        if (*format_string == '0') {
            padding = '0';
            ++format_string;
        }
        while (*format_string >= '0' && *format_string <= '9') {
            width = width * 10 + *format_string - '0';
            ++format_string;
        }
        if (*format_string == 'l') {
            long_value = 1;
            ++format_string;
            if (*format_string == 'l')
                ++format_string;
        } else if (*format_string == 'z') {
            long_value = 1;
            ++format_string;
        }
        switch (*format_string) {
        case '%':
            sink_character(sink, '%');
            break;
        case 'c':
            sink_character(sink, (char)va_arg(arguments, int));
            break;
        case 's': {
            const char *string = va_arg(arguments, const char *);
            if (string == NULL)
                string = "(null)";
            sink_string(sink, string, strlen(string));
            break;
        }
        case 'd':
        case 'i': {
            int64_t value = long_value ? va_arg(arguments, long) :
                                         va_arg(arguments, int);
            uint64_t magnitude;
            if (value < 0) {
                sink_character(sink, '-');
                magnitude = (uint64_t)(-(value + 1)) + 1u;
            } else {
                magnitude = (uint64_t)value;
            }
            format_unsigned(sink, magnitude, 10, width, padding, 0);
            break;
        }
        case 'u':
            format_unsigned(sink,
                            long_value ? va_arg(arguments, unsigned long) :
                                         va_arg(arguments, unsigned),
                            10, width, padding, 0);
            break;
        case 'x':
        case 'X':
            format_unsigned(sink,
                            long_value ? va_arg(arguments, unsigned long) :
                                         va_arg(arguments, unsigned),
                            16, width, padding, *format_string == 'X');
            break;
        case 'p':
            sink_string(sink, "0x", 2);
            format_unsigned(sink, (uintptr_t)va_arg(arguments, void *), 16,
                            (int)(sizeof(void *) * 2u), '0', 0);
            break;
        case '\0':
            --format_string;
            break;
        default:
            sink_character(sink, '%');
            sink_character(sink, *format_string);
            break;
        }
        if (*format_string != '\0')
            ++format_string;
    }
    if (sink->buffer != NULL && sink->capacity != 0) {
        size_t terminator = sink->position < sink->capacity
                                ? sink->position
                                : sink->capacity - 1u;
        sink->buffer[terminator] = '\0';
    }
    return sink->failed ? -1 : (int)sink->position;
}

int dprintf(int fd, const char *format_string, ...)
{
    struct format_sink sink = {.fd = fd};
    va_list arguments;
    int result;
    va_start(arguments, format_string);
    result = format(&sink, format_string, arguments);
    va_end(arguments);
    return result;
}

int printf(const char *format_string, ...)
{
    struct format_sink sink = {.fd = STDOUT_FILENO};
    va_list arguments;
    int result;
    va_start(arguments, format_string);
    result = format(&sink, format_string, arguments);
    va_end(arguments);
    return result;
}

int snprintf(char *buffer, size_t size, const char *format_string, ...)
{
    struct format_sink sink = {.buffer = buffer, .capacity = size};
    va_list arguments;
    int result;
    va_start(arguments, format_string);
    result = format(&sink, format_string, arguments);
    va_end(arguments);
    return result;
}

int putchar(int character)
{
    char byte = (char)character;
    return write(STDOUT_FILENO, &byte, 1) == 1 ? character : EOF;
}

int puts(const char *string)
{
    size_t length = strlen(string);
    if (write(STDOUT_FILENO, string, length) != (int64_t)length ||
        write(STDOUT_FILENO, "\n", 1) != 1)
        return EOF;
    return 0;
}

const char *getenv(const char *name)
{
    size_t length = strlen(name);
    if (environ == NULL)
        return NULL;
    for (size_t index = 0; environ[index] != NULL; ++index) {
        if (strncmp(environ[index], name, length) == 0 &&
            environ[index][length] == '=')
            return environ[index] + length + 1u;
    }
    return NULL;
}
