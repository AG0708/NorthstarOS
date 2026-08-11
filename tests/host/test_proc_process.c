#include <northstar/proc_process.h>

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
    int spaces_destroyed;
    int files_destroyed;
    int wakeups;
    uint64_t next_entry;
};

static void *allocate_memory(void *opaque, size_t size, size_t alignment)
{
    (void)opaque;
    (void)alignment;
    return calloc(1, size);
}

static void release_memory(void *opaque, void *pointer, size_t size)
{
    (void)opaque;
    (void)size;
    free(pointer);
}

static void destroy_space(void *opaque, void *space)
{
    struct fixture *fixture = opaque;
    ++fixture->spaces_destroyed;
    free(space);
}

static int build_image(void *opaque, const char *path,
                       const char *const *argv, const char *const *envp,
                       struct ns_process_image *out)
{
    struct fixture *fixture = opaque;
    (void)path;
    (void)argv;
    (void)envp;
    out->address_space = malloc(1);
    if (out->address_space == NULL)
        return -NS_ENOMEM;
    out->entry = ++fixture->next_entry;
    out->stack_pointer = 0x700000;
    out->initial_brk = 0x500000;
    out->brk_limit = 0x600000;
    return 0;
}

static int create_files(void *opaque, void **out)
{
    (void)opaque;
    *out = malloc(1);
    return *out == NULL ? -NS_ENOMEM : 0;
}

static int clone_files(void *opaque, void *parent, void **out)
{
    (void)parent;
    return create_files(opaque, out);
}

static void destroy_files(void *opaque, void *files)
{
    struct fixture *fixture = opaque;
    ++fixture->files_destroyed;
    free(files);
}

static int initialize_user(void *opaque, struct ns_arch_context *context,
                           uint64_t entry, uint64_t stack,
                           uint64_t kernel_stack, void *space)
{
    (void)opaque;
    (void)kernel_stack;
    (void)space;
    context->rip = entry;
    context->rsp = stack;
    return 0;
}

static int initialize_kernel(void *opaque, struct ns_arch_context *context,
                             void (*entry)(void *), void *argument,
                             uint64_t kernel_stack)
{
    (void)opaque;
    (void)argument;
    context->rip = (uint64_t)(uintptr_t)entry;
    context->rsp = kernel_stack;
    return 0;
}

static void make_ready(void *opaque, struct ns_thread *thread)
{
    struct fixture *fixture = opaque;
    ++fixture->wakeups;
    thread->state = NS_THREAD_READY;
}

static void kernel_entry(void *argument) { (void)argument; }

int main(void)
{
    struct fixture fixture = {.next_entry = 0x400000};
    struct ns_process_ops ops = {
        .context = &fixture,
        .allocate = allocate_memory,
        .deallocate = release_memory,
        .destroy_address_space = destroy_space,
        .build_process_image = build_image,
        .create_fd_table = create_files,
        .clone_fd_table = clone_files,
        .destroy_fd_table = destroy_files,
        .initialize_user_context = initialize_user,
        .initialize_kernel_context = initialize_kernel,
        .make_thread_ready = make_ready,
    };
    struct ns_process_manager manager;
    struct ns_process *kernel;
    struct ns_thread *kernel_thread;
    struct ns_process *init;
    struct ns_thread *init_thread;
    struct ns_process *child;
    struct ns_thread *child_thread;
    const char *arguments[] = {"program", NULL};
    struct ns_process_spawn_spec spec = {
        .path = "/bin/program", .argv = arguments};

    CHECK(ns_proc_manager_init(&manager, &ops, 16384) == 0);
    CHECK(ns_proc_create_kernel(&manager, "init-kernel", kernel_entry, NULL, 0,
                                &kernel, &kernel_thread) == 0);
    CHECK(kernel->pid == NS_PID_NONE && manager.init_process == NULL);
    CHECK(ns_proc_spawn(&manager, kernel, &spec, &init, &init_thread) == 0);
    CHECK(init->pid == 1 && manager.init_process == init &&
          init->parent == kernel && kernel->child_count == 1);
    CHECK(ns_proc_spawn(&manager, init, &spec, &child, &child_thread) == 0);
    CHECK(child->parent == init && init->child_count == 1);
    CHECK(child->user_brk_base == 0x500000 &&
          child_thread->context.rip == 0x400002);
    CHECK(ns_proc_wait(&manager, init_thread, child->pid, NS_WNOHANG, NULL) ==
          0);

    void *old_space = child->address_space;
    CHECK(ns_proc_exec(&manager, child_thread, &spec) == 0);
    CHECK(child->address_space != old_space && fixture.spaces_destroyed == 1);
    CHECK(child_thread->context.rip == 0x400003);
    CHECK(ns_proc_exit(&manager, child_thread, 37) == 0);
    CHECK(child->state == NS_PROCESS_ZOMBIE);
    int status = 0;
    CHECK(ns_proc_wait(&manager, init_thread, child->pid, 0, &status) > 0);
    CHECK(status == 37 && init->child_count == 0);

    CHECK(ns_proc_spawn(&manager, init, &spec, &child, &child_thread) == 0);
    CHECK(ns_proc_wait(&manager, init_thread, child->pid, 0, &status) ==
          -NS_EAGAIN);
    init_thread->state = NS_THREAD_BLOCKED;
    CHECK(ns_proc_exit(&manager, child_thread, 9) == 0);
    CHECK(fixture.wakeups == 1 && init_thread->state == NS_THREAD_READY);
    CHECK(ns_proc_wait(&manager, init_thread, child->pid, 0, &status) > 0);
    CHECK(status == 9);

    CHECK(ns_proc_exit(&manager, init_thread, 0) == 0);
    CHECK(ns_proc_wait(&manager, kernel_thread, init->pid, 0, &status) > 0);
    CHECK(status == 0 && manager.init_process == NULL &&
          kernel->child_count == 0);

    ns_proc_destroy_all(&manager);
    CHECK(manager.all_processes == NULL && manager.all_threads == NULL);
    puts("ok - spawn, exec replacement, wait, exit, wake, and reap");
    return 0;
}
