#include <northstar_user.h>

extern long __ns_syscall6(long number, long first, long second, long third,
                          long fourth, long fifth, long sixth);

int errno;
char **environ;

static long invoke(long number, long first, long second, long third,
                   long fourth, long fifth, long sixth)
{
    long result = __ns_syscall6(number, first, second, third, fourth, fifth,
                                sixth);
    if (result < 0 && result >= -4095) {
        errno = (int)-result;
        return -1;
    }
    return result;
}

__attribute__((noreturn)) void _exit(int status)
{
    (void)__ns_syscall6(NS_SYS_EXIT, status, 0, 0, 0, 0, 0);
    for (;;)
        __asm__ volatile("ud2");
}

int abi_version(void)
{
    return (int)invoke(NS_SYS_ABI_VERSION, 0, 0, 0, 0, 0, 0);
}
int getpid(void) { return (int)invoke(NS_SYS_GETPID, 0, 0, 0, 0, 0, 0); }
int yield(void) { return (int)invoke(NS_SYS_YIELD, 0, 0, 0, 0, 0, 0); }
int sleep_ns(uint64_t ns)
{
    return (int)invoke(NS_SYS_SLEEP_NS, (long)ns, 0, 0, 0, 0, 0);
}
int clock_monotonic(struct ns_abi_timespec *time)
{
    return (int)invoke(NS_SYS_CLOCK_MONOTONIC, (long)time, 0, 0, 0, 0, 0);
}
int64_t read(int fd, void *buffer, size_t count)
{
    return invoke(NS_SYS_READ, fd, (long)buffer, (long)count, 0, 0, 0);
}
int64_t write(int fd, const void *buffer, size_t count)
{
    return invoke(NS_SYS_WRITE, fd, (long)buffer, (long)count, 0, 0, 0);
}
int close(int fd) { return (int)invoke(NS_SYS_CLOSE, fd, 0, 0, 0, 0, 0); }
int dup2(int old_fd, int new_fd)
{
    return (int)invoke(NS_SYS_DUP2, old_fd, new_fd, 0, 0, 0, 0);
}
int pipe(int descriptors[2])
{
    return (int)invoke(NS_SYS_PIPE, (long)descriptors, 0, 0, 0, 0, 0);
}
int open(const char *path, uint32_t flags, uint32_t mode)
{
    return (int)invoke(NS_SYS_OPEN, (long)path, flags, mode, 0, 0, 0);
}
int fstat(int fd, struct ns_abi_stat *stat)
{
    return (int)invoke(NS_SYS_FSTAT, fd, (long)stat, 0, 0, 0, 0);
}
int64_t readdir(int fd, struct ns_abi_dirent *entries, size_t capacity)
{
    return invoke(NS_SYS_READDIR, fd, (long)entries, (long)capacity, 0, 0, 0);
}
int chdir(const char *path)
{
    return (int)invoke(NS_SYS_CHDIR, (long)path, 0, 0, 0, 0, 0);
}
int getcwd(char *buffer, size_t size)
{
    long result = invoke(NS_SYS_GETCWD, (long)buffer, (long)size, 0, 0, 0, 0);
    return result < 0 ? -1 : 0;
}
int spawn(const struct ns_spawn_args *arguments)
{
    return (int)invoke(NS_SYS_SPAWN, (long)arguments, 0, 0, 0, 0, 0);
}
int exec(const struct ns_spawn_args *arguments)
{
    return (int)invoke(NS_SYS_EXEC, (long)arguments, 0, 0, 0, 0, 0);
}
int waitpid(int pid, int *status, uint32_t options)
{
    return (int)invoke(NS_SYS_WAITPID, pid, (long)status, options, 0, 0, 0);
}
void *sbrk(intptr_t increment)
{
    long result = invoke(NS_SYS_SBRK, (long)increment, 0, 0, 0, 0, 0);
    return result < 0 ? (void *)-1 : (void *)result;
}
int debug_log(const void *message, size_t length)
{
    return (int)invoke(NS_SYS_DEBUG_LOG, (long)message, (long)length, 0, 0, 0,
                       0);
}

int socket_open(uint32_t domain, uint32_t type, uint32_t protocol)
{
    return (int)invoke(NS_SYS_SOCKET, domain, type, protocol, 0, 0, 0);
}

int socket_connect(int descriptor, const struct ns_abi_socket_address *peer)
{
    return (int)invoke(NS_SYS_SOCKET_CONNECT, descriptor, (long)peer, 0, 0, 0,
                       0);
}

int64_t socket_send(int descriptor, const void *buffer, size_t length)
{
    return invoke(NS_SYS_SOCKET_SEND, descriptor, (long)buffer, (long)length,
                  0, 0, 0);
}

int64_t socket_recv(int descriptor, void *buffer, size_t capacity)
{
    return invoke(NS_SYS_SOCKET_RECV, descriptor, (long)buffer,
                  (long)capacity, 0, 0, 0);
}

int64_t socket_sendto(int descriptor, const void *buffer, size_t length,
                      const struct ns_abi_socket_address *destination)
{
    return invoke(NS_SYS_SOCKET_SENDTO, descriptor, (long)buffer,
                  (long)length, (long)destination, 0, 0);
}

int64_t socket_recvfrom(int descriptor, void *buffer, size_t capacity,
                        struct ns_abi_socket_address *source)
{
    return invoke(NS_SYS_SOCKET_RECVFROM, descriptor, (long)buffer,
                  (long)capacity, (long)source, 0, 0);
}

int socket_set_timeouts(int descriptor, uint64_t receive_timeout_ns,
                        uint64_t send_timeout_ns)
{
    return (int)invoke(NS_SYS_SOCKET_SET_TIMEOUTS, descriptor,
                       (long)receive_timeout_ns, (long)send_timeout_ns, 0, 0,
                       0);
}

int socket_close(int descriptor)
{
    return (int)invoke(NS_SYS_SOCKET_CLOSE, descriptor, 0, 0, 0, 0, 0);
}
