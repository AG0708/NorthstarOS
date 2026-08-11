#include <northstar/syscall_usercopy.h>

#include <stddef.h>
#include <stdint.h>

static int bounds_overflow(uint64_t address, size_t length)
{
    return (uint64_t)length > UINT64_MAX - address;
}

int ns_user_range_check(const struct ns_user_memory_ops *ops,
                        const struct ns_process *process, uint64_t address,
                        size_t length, int write_access)
{
    if (ops == NULL || process == NULL || process->address_space == NULL ||
        ops->range_valid == NULL || bounds_overflow(address, length))
        return -NS_EFAULT;
    if (length == 0)
        return 0;
    if (address == 0)
        return -NS_EFAULT;
    return ops->range_valid(ops->context, process->address_space, address,
                            length, write_access) == 0
               ? 0
               : -NS_EFAULT;
}

int ns_copy_from_user(const struct ns_user_memory_ops *ops,
                      const struct ns_process *process, void *destination,
                      uint64_t source, size_t length)
{
    int error;
    if (length != 0 && destination == NULL)
        return -NS_EINVAL;
    error = ns_user_range_check(ops, process, source, length, 0);
    if (error != 0 || length == 0)
        return error;
    if (ops->copy_from_user == NULL)
        return -NS_EFAULT;
    return ops->copy_from_user(ops->context, process->address_space,
                               destination, source, length) == 0
               ? 0
               : -NS_EFAULT;
}

int ns_copy_to_user(const struct ns_user_memory_ops *ops,
                    const struct ns_process *process, uint64_t destination,
                    const void *source, size_t length)
{
    int error;
    if (length != 0 && source == NULL)
        return -NS_EINVAL;
    error = ns_user_range_check(ops, process, destination, length, 1);
    if (error != 0 || length == 0)
        return error;
    if (ops->copy_to_user == NULL)
        return -NS_EFAULT;
    return ops->copy_to_user(ops->context, process->address_space, destination,
                             source, length) == 0
               ? 0
               : -NS_EFAULT;
}

int ns_copy_user_string(const struct ns_user_memory_ops *ops,
                        const struct ns_process *process, char *destination,
                        size_t capacity, uint64_t source, size_t *out_length)
{
    size_t offset;
    if (destination == NULL || capacity == 0 || source == 0)
        return -NS_EINVAL;
    for (offset = 0; offset < capacity; ++offset) {
        char value;
        int error = ns_copy_from_user(ops, process, &value, source + offset, 1);
        if (error != 0)
            return error;
        destination[offset] = value;
        if (value == '\0') {
            if (out_length != NULL)
                *out_length = offset;
            return 0;
        }
    }
    destination[capacity - 1u] = '\0';
    return -NS_ENAMETOOLONG;
}
