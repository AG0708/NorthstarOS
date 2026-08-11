#ifndef NORTHSTAR_PROC_PROCESS_H
#define NORTHSTAR_PROC_PROCESS_H

#include <stddef.h>
#include <stdint.h>

#include <northstar/syscall_abi.h>

typedef uint32_t ns_pid_t;
typedef uint32_t ns_tid_t;

#define NS_PID_NONE ((ns_pid_t)0)
#define NS_TID_NONE ((ns_tid_t)0)
#define NS_PROC_NAME_MAX 32u
#define NS_PROC_WAIT_ANY ((ns_pid_t)UINT32_MAX)
#define NS_PROC_DEFAULT_QUANTUM 5u

enum ns_process_state {
    NS_PROCESS_EMBRYO = 0,
    NS_PROCESS_ALIVE,
    NS_PROCESS_EXITING,
    NS_PROCESS_ZOMBIE,
    NS_PROCESS_REAPED
};

enum ns_thread_state {
    NS_THREAD_EMBRYO = 0,
    NS_THREAD_READY,
    NS_THREAD_RUNNING,
    NS_THREAD_BLOCKED,
    NS_THREAD_SLEEPING,
    NS_THREAD_ZOMBIE
};

enum ns_thread_flags {
    NS_THREAD_KERNEL = 1u << 0,
    NS_THREAD_IDLE = 1u << 1
};

enum ns_wait_kind {
    NS_WAIT_NONE = 0,
    NS_WAIT_CHILD,
    NS_WAIT_PIPE_READ,
    NS_WAIT_PIPE_WRITE,
    NS_WAIT_IO
};

/* Callee-saved context. Interrupt/syscall frames remain on the kernel stack. */
struct ns_arch_context {
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t cr3;
    uint64_t fs_base;
};

struct ns_process;

struct ns_thread {
    ns_tid_t tid;
    enum ns_thread_state state;
    uint32_t flags;
    uint32_t quantum_left;
    uint64_t switches;
    uint64_t runtime_ticks;
    uint64_t wake_deadline_ns;
    enum ns_wait_kind wait_kind;
    uintptr_t wait_key;
    ns_pid_t wait_pid;
    struct ns_arch_context context;
    void *kernel_stack;
    size_t kernel_stack_size;
    struct ns_process *process;
    struct ns_thread *process_next;
    struct ns_thread *all_next;
    struct ns_thread *run_prev;
    struct ns_thread *run_next;
    struct ns_thread *wait_next;
    struct ns_thread *sleep_next;
    uint8_t on_run_queue;
};

struct ns_process {
    ns_pid_t pid;
    ns_pid_t parent_pid;
    enum ns_process_state state;
    int32_t exit_status;
    uint32_t live_threads;
    uint32_t child_count;
    uint64_t user_brk_base;
    uint64_t user_brk;
    uint64_t user_brk_limit;
    char name[NS_PROC_NAME_MAX];
    void *address_space;
    void *files;
    struct ns_thread *threads;
    struct ns_process *parent;
    struct ns_process *children;
    struct ns_process *sibling_next;
    struct ns_process *all_next;
};

struct ns_process_image {
    void *address_space;
    uint64_t entry;
    uint64_t stack_pointer;
    uint64_t initial_brk;
    uint64_t brk_limit;
};

struct ns_process_spawn_spec {
    const char *path;
    const char *const *argv;
    const char *const *envp;
    const struct ns_spawn_action *actions;
    uint32_t action_count;
    uint32_t flags;
};

struct ns_process_ops {
    void *context;
    void *(*allocate)(void *context, size_t size, size_t alignment);
    void (*deallocate)(void *context, void *pointer, size_t size);
    int (*create_address_space)(void *context, void **out_space);
    void (*destroy_address_space)(void *context, void *space);
    int (*build_process_image)(void *context, const char *path,
                               const char *const *argv,
                               const char *const *envp,
                               struct ns_process_image *out_image);
    int (*create_fd_table)(void *context, void **out_files);
    int (*clone_fd_table)(void *context, void *parent_files,
                          void **out_files);
    void (*destroy_fd_table)(void *context, void *files);
    int (*apply_spawn_actions)(void *context, void *files,
                               const struct ns_spawn_action *actions,
                               uint32_t count);
    void (*close_cloexec)(void *context, void *files);
    int (*initialize_user_context)(void *context, struct ns_arch_context *arch,
                                   uint64_t entry, uint64_t stack_pointer,
                                   uint64_t kernel_stack_top,
                                   void *address_space);
    int (*initialize_kernel_context)(void *context, struct ns_arch_context *arch,
                                     void (*entry)(void *), void *argument,
                                     uint64_t kernel_stack_top);
    uint64_t (*monotonic_ns)(void *context);
    void (*make_thread_ready)(void *context, struct ns_thread *thread);
};

struct ns_process_manager {
    struct ns_process_ops ops;
    struct ns_process *all_processes;
    struct ns_thread *all_threads;
    struct ns_process *init_process;
    ns_pid_t next_pid;
    ns_tid_t next_tid;
    size_t kernel_stack_size;
};

int ns_proc_manager_init(struct ns_process_manager *manager,
                         const struct ns_process_ops *ops,
                         size_t kernel_stack_size);
struct ns_process *ns_proc_find(struct ns_process_manager *manager,
                                ns_pid_t pid);
struct ns_thread *ns_proc_find_thread(struct ns_process_manager *manager,
                                      ns_tid_t tid);
int ns_proc_create_kernel(struct ns_process_manager *manager, const char *name,
                          void (*entry)(void *), void *argument,
                          uint32_t thread_flags,
                          struct ns_process **out_process,
                          struct ns_thread **out_thread);
int ns_proc_spawn(struct ns_process_manager *manager,
                  struct ns_process *parent,
                  const struct ns_process_spawn_spec *spec,
                  struct ns_process **out_process,
                  struct ns_thread **out_thread);
int ns_proc_exec(struct ns_process_manager *manager, struct ns_thread *caller,
                 const struct ns_process_spawn_spec *spec);
int ns_proc_exit(struct ns_process_manager *manager, struct ns_thread *caller,
                 int status);
/* Returns child pid, 0 for WNOHANG, -NS_EAGAIN after arming a blocking wait. */
int64_t ns_proc_wait(struct ns_process_manager *manager,
                     struct ns_thread *caller, ns_pid_t pid, uint32_t options,
                     int *out_status);
int ns_proc_reap(struct ns_process_manager *manager,
                 struct ns_process *parent, struct ns_process *child,
                 int *out_status);
void ns_proc_destroy_all(struct ns_process_manager *manager);

#endif
