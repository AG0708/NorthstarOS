#ifndef NORTHSTAR_SPINLOCK_H
#define NORTHSTAR_SPINLOCK_H

#include <northstar/base.h>

typedef struct {
    volatile uint32_t value;
} ns_spinlock_t;

#define NS_SPINLOCK_INIT {0}

static inline void ns_spin_pause(void) {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile ("pause");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ volatile ("yield");
#else
    __asm__ volatile ("" ::: "memory");
#endif
}

static inline void ns_spin_lock(ns_spinlock_t *lock) {
    while (__atomic_test_and_set(&lock->value, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&lock->value, __ATOMIC_RELAXED)) {
            ns_spin_pause();
        }
    }
}

static inline bool ns_spin_try_lock(ns_spinlock_t *lock) {
    return !__atomic_test_and_set(&lock->value, __ATOMIC_ACQUIRE);
}

static inline void ns_spin_unlock(ns_spinlock_t *lock) {
    __atomic_clear(&lock->value, __ATOMIC_RELEASE);
}

#endif
