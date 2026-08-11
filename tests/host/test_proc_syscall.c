#include <northstar/syscall_dispatch.h>

#include <northstar/vfs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,           \
                    #expression);                                               \
            return 1;                                                           \
        }                                                                       \
    } while (0)

struct fixture {
    unsigned char user[4096];
    uint64_t base;
    char written[64];
    size_t written_length;
    char opened[64];
    char debug[64];
    size_t debug_length;
    char socket_sent[64];
    size_t socket_sent_length;
    struct ns_socket_address socket_peer;
    unsigned socket_creates;
    unsigned socket_closes;
};

static struct fixture *active_fixture;

static int valid(void *opaque, void *space, uint64_t address, size_t length,
                 int write_access)
{
    struct fixture *fixture = opaque;
    (void)space;
    (void)write_access;
    if (address < fixture->base || address - fixture->base > sizeof(fixture->user))
        return -1;
    return length <= sizeof(fixture->user) - (size_t)(address - fixture->base)
               ? 0
               : -1;
}

static int copy_from(void *opaque, void *space, void *destination,
                     uint64_t source, size_t length)
{
    struct fixture *fixture = opaque;
    (void)space;
    memcpy(destination, fixture->user + (source - fixture->base), length);
    return 0;
}

static int copy_to(void *opaque, void *space, uint64_t destination,
                   const void *source, size_t length)
{
    struct fixture *fixture = opaque;
    (void)space;
    memcpy(fixture->user + (destination - fixture->base), source, length);
    return 0;
}

static void *allocate_memory(void *opaque, size_t size, size_t alignment)
{
    (void)opaque;
    (void)alignment;
    return malloc(size);
}

static void release_memory(void *opaque, void *pointer, size_t size,
                           size_t alignment)
{
    (void)opaque;
    (void)size;
    (void)alignment;
    free(pointer);
}

static uint64_t monotonic(void *opaque)
{
    (void)opaque;
    return 12345678901ull;
}

static int socket_create(void *opaque, uint32_t domain, uint32_t type,
                         uint32_t protocol, uintptr_t *object_out)
{
    struct fixture *fixture = opaque;
    CHECK(domain == NS_AF_INET);
    CHECK(type == NS_SOCK_STREAM || type == NS_SOCK_DGRAM);
    CHECK(protocol == NS_IPPROTO_TCP || protocol == NS_IPPROTO_UDP);
    *object_out = ++fixture->socket_creates;
    return NS_SOCKET_OK;
}

static int socket_connect_backend(void *opaque, uintptr_t object,
                                  const struct ns_socket_address *peer)
{
    struct fixture *fixture = opaque;
    CHECK(object != 0 && peer != NULL);
    fixture->socket_peer = *peer;
    return NS_SOCKET_OK;
}

static int socket_send_backend(void *opaque, uintptr_t object,
                               const struct ns_socket_address *destination,
                               const void *buffer, size_t length,
                               size_t *sent_out)
{
    struct fixture *fixture = opaque;
    CHECK(object != 0 && buffer != NULL && sent_out != NULL);
    CHECK(length <= sizeof(fixture->socket_sent));
    memcpy(fixture->socket_sent, buffer, length);
    fixture->socket_sent_length = length;
    if (destination != NULL)
        fixture->socket_peer = *destination;
    *sent_out = length;
    return NS_SOCKET_OK;
}

static int socket_receive_backend(void *opaque, uintptr_t object, void *buffer,
                                  size_t capacity, size_t *received_out,
                                  struct ns_socket_address *source_out)
{
    static const char response[] = "net-data";
    (void)opaque;
    CHECK(object != 0 && buffer != NULL && received_out != NULL);
    CHECK(capacity >= sizeof(response) - 1u);
    memcpy(buffer, response, sizeof(response) - 1u);
    *received_out = sizeof(response) - 1u;
    if (source_out != NULL) {
        source_out->address = 0x0a000202u;
        source_out->port = 9000;
    }
    return NS_SOCKET_OK;
}

static int socket_close_backend(void *opaque, uintptr_t object)
{
    struct fixture *fixture = opaque;
    CHECK(object != 0);
    ++fixture->socket_closes;
    return NS_SOCKET_OK;
}

static const struct ns_socket_backend_ops socket_backend_ops = {
    .create = socket_create,
    .connect = socket_connect_backend,
    .send = socket_send_backend,
    .receive = socket_receive_backend,
    .close = socket_close_backend,
};

static void debug_log(void *opaque, const char *message, size_t length)
{
    struct fixture *fixture = opaque;
    memcpy(fixture->debug, message, length);
    fixture->debug_length = length;
}

static int adjust_break(void *opaque, struct ns_process *process,
                        uint64_t old_break, uint64_t new_break)
{
    (void)opaque;
    (void)process;
    (void)old_break;
    (void)new_break;
    return 0;
}

int64_t ns_vfs_read(struct ns_vfs_fdtable *table, int fd, void *buffer,
                    size_t count)
{
    static const char value[] = "read-data";
    (void)table;
    (void)fd;
    if (count > sizeof(value) - 1u)
        count = sizeof(value) - 1u;
    memcpy(buffer, value, count);
    return (int64_t)count;
}

int64_t ns_vfs_write(struct ns_vfs_fdtable *table, int fd,
                     const void *buffer, size_t count)
{
    (void)table;
    (void)fd;
    if (count > sizeof(active_fixture->written))
        count = sizeof(active_fixture->written);
    memcpy(active_fixture->written, buffer, count);
    active_fixture->written_length = count;
    return (int64_t)count;
}

int ns_vfs_close(struct ns_vfs_fdtable *table, int fd)
{ (void)table; (void)fd; return 0; }
int ns_vfs_dup2(struct ns_vfs_fdtable *table, int old_fd, int new_fd,
                bool close_on_exec)
{ (void)table; (void)old_fd; (void)close_on_exec; return new_fd; }
int ns_vfs_open(struct ns_vfs_fdtable *table, const char *path,
                uint32_t flags, uint32_t mode)
{
    (void)table; (void)flags; (void)mode;
    strncpy(active_fixture->opened, path, sizeof(active_fixture->opened));
    return 7;
}
int ns_vfs_fstat(struct ns_vfs_fdtable *table, int fd,
                 struct ns_vfs_node_info *result)
{ (void)table; (void)fd; memset(result, 0, sizeof(*result)); return 0; }
int64_t ns_vfs_getdents(struct ns_vfs_fdtable *table, int fd,
                        struct ns_abi_dirent *entries, size_t capacity)
{ (void)table; (void)fd; (void)entries; (void)capacity; return 0; }
int ns_vfs_chdir(struct ns_vfs_fdtable *table, const char *path)
{ (void)table; (void)path; return 0; }
int ns_vfs_getcwd(const struct ns_vfs_fdtable *table, char *buffer, size_t size)
{ (void)table; if (size < 2) return -NS_ERANGE; strcpy(buffer, "/"); return 0; }

int ns_pipe_create(const struct ns_pipe_runtime *runtime,
                   struct ns_vfs_fdtable *table, int descriptors[2])
{ (void)runtime; (void)table; descriptors[0] = 3; descriptors[1] = 4; return 0; }
int ns_proc_exit(struct ns_process_manager *manager, struct ns_thread *caller,
                 int status)
{ (void)manager; (void)caller; (void)status; return 0; }
int ns_proc_spawn(struct ns_process_manager *manager, struct ns_process *parent,
                  const struct ns_process_spawn_spec *spec,
                  struct ns_process **out_process, struct ns_thread **out_thread)
{ (void)manager; (void)parent; (void)spec; (void)out_process; (void)out_thread; return -NS_ENOSYS; }
int ns_proc_exec(struct ns_process_manager *manager, struct ns_thread *caller,
                 const struct ns_process_spawn_spec *spec)
{ (void)manager; (void)caller; (void)spec; return -NS_ENOSYS; }
int64_t ns_proc_wait(struct ns_process_manager *manager,
                     struct ns_thread *caller, ns_pid_t pid, uint32_t options,
                     int *out_status)
{ (void)manager; (void)caller; (void)pid; (void)options; (void)out_status; return -NS_ECHILD; }
void ns_sched_terminate_current(struct ns_scheduler *scheduler) { (void)scheduler; }
void ns_sched_yield(struct ns_scheduler *scheduler) { (void)scheduler; }
void ns_sched_sleep_until(struct ns_scheduler *scheduler, uint64_t deadline_ns)
{ (void)scheduler; (void)deadline_ns; }
int ns_sched_add(struct ns_scheduler *scheduler, struct ns_thread *thread)
{ (void)scheduler; (void)thread; return 0; }
void ns_sched_block(struct ns_scheduler *scheduler, enum ns_wait_kind kind,
                    uintptr_t key)
{ (void)scheduler; (void)kind; (void)key; }

int main(void)
{
    struct fixture fixture = {.base = 0x10000};
    struct ns_socket_table sockets;
    const struct ns_socket_config socket_config = {
        .udp = {.ops = &socket_backend_ops, .context = &fixture},
        .tcp = {.ops = &socket_backend_ops, .context = &fixture},
        .clock_ns = monotonic,
        .wait_context = &fixture,
    };
    struct ns_process process = {
        .pid = 42, .address_space = &fixture, .files = (void *)(uintptr_t)1,
        .user_brk_base = 0x400000, .user_brk = 0x400000,
        .user_brk_limit = 0x500000};
    struct ns_thread thread = {.process = &process, .state = NS_THREAD_RUNNING};
    struct ns_scheduler scheduler = {.current = &thread};
    struct ns_process_manager manager = {
        .ops = {.context = &fixture, .monotonic_ns = monotonic}};
    struct ns_syscall_runtime runtime = {
        .context = &fixture,
        .processes = &manager,
        .scheduler = &scheduler,
        .sockets = &sockets,
        .user_memory = {.context = &fixture, .range_valid = valid,
                        .copy_from_user = copy_from, .copy_to_user = copy_to},
        .allocate = allocate_memory,
        .deallocate = release_memory,
        .adjust_break = adjust_break,
        .debug_log = debug_log,
    };
    struct ns_arch_syscall_frame frame;
    active_fixture = &fixture;
    ns_socket_table_init(&sockets, &socket_config);
    CHECK(ns_syscall_runtime_init(&runtime) == 0);

    memset(&frame, 0, sizeof(frame));
    frame.rax = NS_SYS_ABI_VERSION;
    CHECK(ns_syscall_dispatch(&runtime, &frame) == NS_ABI_VERSION);
    frame.rax = NS_SYS_GETPID;
    CHECK(ns_syscall_dispatch(&runtime, &frame) == 42);
    frame.rax = NS_SYS_COUNT + 99;
    CHECK(ns_syscall_dispatch(&runtime, &frame) == -NS_ENOSYS);

    memcpy(fixture.user, "hello", 5);
    frame = (struct ns_arch_syscall_frame){.rax = NS_SYS_WRITE, .rdi = 1,
                                           .rsi = fixture.base, .rdx = 5};
    CHECK(ns_syscall_dispatch(&runtime, &frame) == 5);
    CHECK(fixture.written_length == 5 && memcmp(fixture.written, "hello", 5) == 0);
    frame.rsi = fixture.base + sizeof(fixture.user) - 2;
    frame.rdx = 5;
    CHECK(ns_syscall_dispatch(&runtime, &frame) == -NS_EFAULT);

    frame = (struct ns_arch_syscall_frame){.rax = NS_SYS_READ, .rdi = 0,
                                           .rsi = fixture.base + 32, .rdx = 9};
    CHECK(ns_syscall_dispatch(&runtime, &frame) == 9);
    CHECK(memcmp(fixture.user + 32, "read-data", 9) == 0);
    strcpy((char *)fixture.user + 64, "/bin/test");
    frame = (struct ns_arch_syscall_frame){.rax = NS_SYS_OPEN,
                                           .rdi = fixture.base + 64,
                                           .rsi = NS_O_RDONLY};
    CHECK(ns_syscall_dispatch(&runtime, &frame) == 7);
    CHECK(strcmp(fixture.opened, "/bin/test") == 0);

    memcpy(fixture.user + 96, "debug", 5);
    frame = (struct ns_arch_syscall_frame){.rax = NS_SYS_DEBUG_LOG,
                                           .rdi = fixture.base + 96, .rsi = 5};
    CHECK(ns_syscall_dispatch(&runtime, &frame) == 5);
    CHECK(fixture.debug_length == 5 && memcmp(fixture.debug, "debug", 5) == 0);
    frame = (struct ns_arch_syscall_frame){.rax = NS_SYS_CLOCK_MONOTONIC,
                                           .rdi = fixture.base + 128};
    CHECK(ns_syscall_dispatch(&runtime, &frame) == 0);
    struct ns_abi_timespec *time = (struct ns_abi_timespec *)(fixture.user + 128);
    CHECK(time->tv_sec == 12 && time->tv_nsec == 345678901);

    frame = (struct ns_arch_syscall_frame){.rax = NS_SYS_SBRK, .rdi = 4096};
    CHECK(ns_syscall_dispatch(&runtime, &frame) == 0x400000);
    CHECK(process.user_brk == 0x401000);

    {
        struct ns_abi_socket_address *peer =
            (struct ns_abi_socket_address *)(fixture.user + 160);
        struct ns_abi_socket_address *source =
            (struct ns_abi_socket_address *)(fixture.user + 320);
        int32_t descriptor;
        peer->address = 0x0a000202u;
        peer->port = 8080;
        peer->reserved = 0;
        frame = (struct ns_arch_syscall_frame){
            .rax = NS_SYS_SOCKET,
            .rdi = NS_ABI_AF_INET,
            .rsi = NS_ABI_SOCK_STREAM,
            .rdx = NS_ABI_IPPROTO_TCP,
        };
        descriptor = (int32_t)ns_syscall_dispatch(&runtime, &frame);
        CHECK(descriptor > 0);
        frame = (struct ns_arch_syscall_frame){
            .rax = NS_SYS_SOCKET_CONNECT,
            .rdi = (uint32_t)descriptor,
            .rsi = fixture.base + 160,
        };
        CHECK(ns_syscall_dispatch(&runtime, &frame) == 0);
        CHECK(fixture.socket_peer.address == peer->address &&
              fixture.socket_peer.port == peer->port);

        frame = (struct ns_arch_syscall_frame){
            .rax = NS_SYS_SOCKET_CONNECT,
            .rdi = (uint32_t)descriptor,
            .rsi = fixture.base + sizeof(fixture.user) - 4u,
        };
        CHECK(ns_syscall_dispatch(&runtime, &frame) == -NS_EFAULT);
        memcpy(fixture.user + 200, "request", 7);
        frame = (struct ns_arch_syscall_frame){
            .rax = NS_SYS_SOCKET_SEND,
            .rdi = (uint32_t)descriptor,
            .rsi = fixture.base + 200,
            .rdx = 7,
        };
        CHECK(ns_syscall_dispatch(&runtime, &frame) == 7);
        CHECK(fixture.socket_sent_length == 7 &&
              memcmp(fixture.socket_sent, "request", 7) == 0);
        frame = (struct ns_arch_syscall_frame){
            .rax = NS_SYS_SOCKET_SEND,
            .rdi = (uint32_t)descriptor,
            .rsi = fixture.base + sizeof(fixture.user),
            .rdx = 1,
        };
        CHECK(ns_syscall_dispatch(&runtime, &frame) == -NS_EFAULT);
        CHECK(fixture.socket_sent_length == 7);

        frame = (struct ns_arch_syscall_frame){
            .rax = NS_SYS_SOCKET_RECV,
            .rdi = (uint32_t)descriptor,
            .rsi = fixture.base + 240,
            .rdx = 16,
        };
        CHECK(ns_syscall_dispatch(&runtime, &frame) == 8);
        CHECK(memcmp(fixture.user + 240, "net-data", 8) == 0);
        frame = (struct ns_arch_syscall_frame){
            .rax = NS_SYS_SOCKET_SET_TIMEOUTS,
            .rdi = (uint32_t)descriptor,
            .rsi = 1000,
            .rdx = 2000,
        };
        CHECK(ns_syscall_dispatch(&runtime, &frame) == 0);
        frame = (struct ns_arch_syscall_frame){
            .rax = NS_SYS_SOCKET_CLOSE,
            .rdi = (uint32_t)descriptor,
        };
        CHECK(ns_syscall_dispatch(&runtime, &frame) == 0);

        frame = (struct ns_arch_syscall_frame){
            .rax = NS_SYS_SOCKET,
            .rdi = NS_ABI_AF_INET,
            .rsi = NS_ABI_SOCK_DGRAM,
            .rdx = NS_ABI_IPPROTO_UDP,
        };
        descriptor = (int32_t)ns_syscall_dispatch(&runtime, &frame);
        CHECK(descriptor > 0);
        peer->port = 9000;
        memcpy(fixture.user + 280, "udp", 3);
        frame = (struct ns_arch_syscall_frame){
            .rax = NS_SYS_SOCKET_SENDTO,
            .rdi = (uint32_t)descriptor,
            .rsi = fixture.base + 280,
            .rdx = 3,
            .r10 = fixture.base + 160,
        };
        CHECK(ns_syscall_dispatch(&runtime, &frame) == 3);
        frame = (struct ns_arch_syscall_frame){
            .rax = NS_SYS_SOCKET_RECVFROM,
            .rdi = (uint32_t)descriptor,
            .rsi = fixture.base + 288,
            .rdx = 16,
            .r10 = fixture.base + 320,
        };
        CHECK(ns_syscall_dispatch(&runtime, &frame) == 8);
        CHECK(memcmp(fixture.user + 288, "net-data", 8) == 0);
        CHECK(source->address == 0x0a000202u && source->port == 9000 &&
              source->reserved == 0);
        frame = (struct ns_arch_syscall_frame){
            .rax = NS_SYS_SOCKET_CLOSE,
            .rdi = (uint32_t)descriptor,
        };
        CHECK(ns_syscall_dispatch(&runtime, &frame) == 0);
        CHECK(fixture.socket_creates == 2 && fixture.socket_closes == 2);
    }

    puts("1..3");
    puts("ok 1 - syscall ABI, files, and process memory");
    puts("ok 2 - invalid user pointers return EFAULT");
    puts("ok 3 - socket syscalls validate copyin and copyout");
    return 0;
}
