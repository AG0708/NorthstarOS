#ifndef NORTHSTAR_BASE_H
#define NORTHSTAR_BASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS_PACKED __attribute__((packed))
#define NS_ALIGNED(n) __attribute__((aligned(n)))
#define NS_NORETURN __attribute__((noreturn))
#define NS_UNUSED __attribute__((unused))
#define NS_WEAK __attribute__((weak))

#define NS_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define NS_MIN(a, b) ((a) < (b) ? (a) : (b))
#define NS_MAX(a, b) ((a) > (b) ? (a) : (b))
#define NS_ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((a) - 1))
#define NS_ALIGN_DOWN(v, a) ((v) & ~((a) - 1))

#define NS_STATIC_ASSERT(c, m) _Static_assert((c), m)

#endif
