#ifndef NORTHSTAR_SYSCALL_USERCOPY_H
#define NORTHSTAR_SYSCALL_USERCOPY_H

#include <stddef.h>
#include <stdint.h>

#include <northstar/proc_process.h>

struct ns_user_memory_ops {
    void *context;
    /* Validate every page and required permission without faulting. */
    int (*range_valid)(void *context, void *address_space, uint64_t address,
                       size_t length, int write_access);
    /* Implementations must contain/fix up faults caused by concurrent unmaps. */
    int (*copy_from_user)(void *context, void *address_space, void *kernel_dst,
                          uint64_t user_src, size_t length);
    int (*copy_to_user)(void *context, void *address_space, uint64_t user_dst,
                        const void *kernel_src, size_t length);
};

int ns_user_range_check(const struct ns_user_memory_ops *ops,
                        const struct ns_process *process, uint64_t address,
                        size_t length, int write_access);
int ns_copy_from_user(const struct ns_user_memory_ops *ops,
                      const struct ns_process *process, void *destination,
                      uint64_t source, size_t length);
int ns_copy_to_user(const struct ns_user_memory_ops *ops,
                    const struct ns_process *process, uint64_t destination,
                    const void *source, size_t length);
/* Copies a NUL-terminated string and returns its length excluding NUL. */
int ns_copy_user_string(const struct ns_user_memory_ops *ops,
                        const struct ns_process *process, char *destination,
                        size_t capacity, uint64_t source, size_t *out_length);

#endif
