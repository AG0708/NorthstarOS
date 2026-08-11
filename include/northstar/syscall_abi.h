#ifndef NORTHSTAR_SYSCALL_ABI_H
#define NORTHSTAR_SYSCALL_ABI_H

/*
 * Stable userspace ABI for NorthstarOS/x86-64.
 *
 * System-call number: rax
 * Arguments:          rdi, rsi, rdx, r10, r8, r9
 * Result:             rax (non-negative value or -NS_E*)
 *
 * Pointers are always userspace virtual addresses.  Kernel handlers must not
 * dereference them directly; copyin/copyout is part of the syscall contract.
 */

#include <stddef.h>
#include <stdint.h>
#include <northstar/errno.h>

#define NS_ABI_VERSION 1u
#define NS_PATH_MAX 4096u
#define NS_ARG_MAX 64u
#define NS_PIPE_READ 0u
#define NS_PIPE_WRITE 1u

enum ns_syscall_number {
    NS_SYS_ABI_VERSION = 0,
    NS_SYS_EXIT,
    NS_SYS_GETPID,
    NS_SYS_YIELD,
    NS_SYS_SLEEP_NS,
    NS_SYS_CLOCK_MONOTONIC,
    NS_SYS_READ,
    NS_SYS_WRITE,
    NS_SYS_CLOSE,
    NS_SYS_DUP2,
    NS_SYS_PIPE,
    NS_SYS_OPEN,
    NS_SYS_FSTAT,
    NS_SYS_READDIR,
    NS_SYS_CHDIR,
    NS_SYS_GETCWD,
    NS_SYS_SPAWN,
    NS_SYS_EXEC,
    NS_SYS_WAITPID,
    NS_SYS_SBRK,
    NS_SYS_DEBUG_LOG,
    NS_SYS_SOCKET,
    NS_SYS_SOCKET_CONNECT,
    NS_SYS_SOCKET_SEND,
    NS_SYS_SOCKET_RECV,
    NS_SYS_SOCKET_SENDTO,
    NS_SYS_SOCKET_RECVFROM,
    NS_SYS_SOCKET_SET_TIMEOUTS,
    NS_SYS_SOCKET_CLOSE,
    NS_SYS_COUNT
};

enum ns_abi_socket_domain {
    NS_ABI_AF_INET = 2
};

enum ns_abi_socket_type {
    NS_ABI_SOCK_STREAM = 1,
    NS_ABI_SOCK_DGRAM = 2
};

enum ns_abi_socket_protocol {
    NS_ABI_IPPROTO_DEFAULT = 0,
    NS_ABI_IPPROTO_TCP = 6,
    NS_ABI_IPPROTO_UDP = 17
};

/* IPv4 addresses use the same a.b.c.d packed integer convention as the
 * kernel network stack.  Ports are ordinary host-order unsigned integers. */
#define NS_ABI_IPV4_ADDRESS(a, b, c, d)                                      \
    ((((uint32_t)(a) & 0xffu) << 24) | (((uint32_t)(b) & 0xffu) << 16) |     \
     (((uint32_t)(c) & 0xffu) << 8) | ((uint32_t)(d) & 0xffu))

struct ns_abi_socket_address {
    uint32_t address;
    uint16_t port;
    uint16_t reserved;
};

enum ns_open_flags {
    NS_O_RDONLY = 0x0000,
    NS_O_WRONLY = 0x0001,
    NS_O_RDWR = 0x0002,
    NS_O_CREAT = 0x0040,
    NS_O_EXCL = 0x0080,
    NS_O_TRUNC = 0x0200,
    NS_O_APPEND = 0x0400,
    NS_O_DIRECTORY = 0x10000,
    NS_O_CLOEXEC = 0x80000
};

enum ns_seek_whence {
    NS_SEEK_SET = 0,
    NS_SEEK_CUR = 1,
    NS_SEEK_END = 2
};

enum ns_wait_options {
    NS_WNOHANG = 1u << 0
};

enum ns_file_type {
    NS_FT_UNKNOWN = 0,
    NS_FT_REGULAR,
    NS_FT_DIRECTORY,
    NS_FT_CHAR,
    NS_FT_BLOCK,
    NS_FT_PIPE,
    NS_FT_SOCKET
};

struct ns_abi_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct ns_abi_stat {
    uint64_t inode;
    uint64_t size;
    uint64_t blocks;
    uint64_t mtime_ns;
    uint32_t mode;
    uint32_t type;
};

struct ns_abi_dirent {
    uint64_t inode;
    uint16_t record_size;
    uint8_t type;
    uint8_t name_length;
    char name[256];
};

enum ns_spawn_action_type {
    NS_SPAWN_DUP2 = 1,
    NS_SPAWN_CLOSE = 2,
    NS_SPAWN_OPEN = 3
};

struct ns_spawn_action {
    uint32_t type;
    int32_t fd;
    int32_t source_fd;
    uint32_t flags;
    uint64_t path; /* const char * in the caller's address space */
};

struct ns_spawn_args {
    uint64_t path;        /* const char * */
    uint64_t argv;        /* const char *const * */
    uint64_t envp;        /* const char *const * */
    uint64_t actions;     /* const struct ns_spawn_action * */
    uint32_t action_count;
    uint32_t flags;
};

#endif
