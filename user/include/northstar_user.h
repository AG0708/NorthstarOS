#ifndef NORTHSTAR_USER_H
#define NORTHSTAR_USER_H

#include <stddef.h>
#include <stdint.h>

#include <northstar/syscall_abi.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define EOF (-1)

extern int errno;
extern char **environ;

__attribute__((noreturn)) void _exit(int status);
int abi_version(void);
int getpid(void);
int yield(void);
int sleep_ns(uint64_t nanoseconds);
int clock_monotonic(struct ns_abi_timespec *time);
int64_t read(int fd, void *buffer, size_t count);
int64_t write(int fd, const void *buffer, size_t count);
int close(int fd);
int dup2(int old_fd, int new_fd);
int pipe(int descriptors[2]);
int open(const char *path, uint32_t flags, uint32_t mode);
int fstat(int fd, struct ns_abi_stat *stat);
int64_t readdir(int fd, struct ns_abi_dirent *entries, size_t capacity);
int chdir(const char *path);
int getcwd(char *buffer, size_t size);
int spawn(const struct ns_spawn_args *arguments);
int exec(const struct ns_spawn_args *arguments);
int waitpid(int pid, int *status, uint32_t options);
void *sbrk(intptr_t increment);
int debug_log(const void *message, size_t length);
int socket_open(uint32_t domain, uint32_t type, uint32_t protocol);
int socket_connect(int descriptor,
                   const struct ns_abi_socket_address *peer);
int64_t socket_send(int descriptor, const void *buffer, size_t length);
int64_t socket_recv(int descriptor, void *buffer, size_t capacity);
int64_t socket_sendto(int descriptor, const void *buffer, size_t length,
                      const struct ns_abi_socket_address *destination);
int64_t socket_recvfrom(int descriptor, void *buffer, size_t capacity,
                        struct ns_abi_socket_address *source);
int socket_set_timeouts(int descriptor, uint64_t receive_timeout_ns,
                        uint64_t send_timeout_ns);
int socket_close(int descriptor);

void *memset(void *destination, int value, size_t length);
void *memcpy(void *destination, const void *source, size_t length);
void *memmove(void *destination, const void *source, size_t length);
int memcmp(const void *left, const void *right, size_t length);
size_t strlen(const char *string);
size_t strnlen(const char *string, size_t maximum);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t length);
char *strcpy(char *destination, const char *source);
char *strncpy(char *destination, const char *source, size_t length);
char *strchr(const char *string, int character);
long strtol(const char *string, char **end, int base);

void *malloc(size_t size);
void free(void *pointer);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);

int putchar(int character);
int puts(const char *string);
int dprintf(int fd, const char *format, ...);
int printf(const char *format, ...);
int snprintf(char *buffer, size_t size, const char *format, ...);

const char *getenv(const char *name);

#endif
