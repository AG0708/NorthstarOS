#include <northstar_user.h>

static uint64_t pointer(const void *value) { return (uint64_t)(uintptr_t)value; }

static int spawn_simple(const char *path, const char *const *argv,
                        const struct ns_spawn_action *actions, uint32_t count)
{
    struct ns_spawn_args arguments = {
        .path = pointer(path), .argv = pointer(argv), .envp = pointer(environ),
        .actions = pointer(actions), .action_count = count};
    return spawn(&arguments);
}

int main(void)
{
    const char *labels[] = {"A", "B", "C", "D"};
    int pids[4];
    errno = 0;
    if (write(STDOUT_FILENO, (const void *)0xffffffff80000000ull, 1) != -1 ||
        errno != NS_EFAULT) {
        puts("not ok 1 - ring3-entry");
        return 1;
    }
    for (int index = 0; index < 4; ++index) {
        const char *argv[] = {"cpu_spin", labels[index], NULL};
        pids[index] = spawn_simple("/bin/cpu_spin", argv, NULL, 0);
        if (pids[index] < 0) {
            dprintf(STDERR_FILENO, "not ok 2 - preemptive-progress\n");
            return 1;
        }
    }
    for (int index = 0; index < 4; ++index) {
        int status;
        if (waitpid(pids[index], &status, 0) < 0 || status != 0) {
            puts("not ok 2 - preemptive-progress");
            return 1;
        }
    }
    int descriptors[2];
    if (pipe(descriptors) != 0)
        return 1;
    const char *echo_argv[] = {"echo", "pipe-roundtrip", NULL};
    struct ns_spawn_action actions[] = {
        {.type = NS_SPAWN_DUP2, .fd = STDOUT_FILENO,
         .source_fd = descriptors[1]},
        {.type = NS_SPAWN_CLOSE, .fd = descriptors[0]},
        {.type = NS_SPAWN_CLOSE, .fd = descriptors[1]},
    };
    int echo_pid = spawn_simple("/bin/echo", echo_argv, actions, 3);
    (void)close(descriptors[1]);
    char buffer[32];
    size_t used = 0;
    int read_error = 0;
    while (used < sizeof(buffer) - 1u) {
        int64_t length =
            read(descriptors[0], buffer + used, sizeof(buffer) - 1u - used);
        if (length < 0) {
            read_error = errno;
            break;
        }
        if (length == 0)
            break;
        used += (size_t)length;
    }
    (void)close(descriptors[0]);
    int echo_status = -1;
    int wait_result = echo_pid < 0 ? -1 : waitpid(echo_pid, &echo_status, 0);
    if (echo_pid < 0 || wait_result < 0 || echo_status != 0 ||
        read_error != 0 || used == 0) {
        dprintf(STDERR_FILENO,
                "pipe diagnostic pid=%d wait=%d status=%d read_errno=%d "
                "bytes=%u\n",
                echo_pid, wait_result, echo_status, read_error, (unsigned)used);
        puts("not ok 3 - pipe-roundtrip");
        return 1;
    }
    buffer[used] = '\0';
    if (strcmp(buffer, "pipe-roundtrip\n") != 0) {
        dprintf(STDERR_FILENO, "pipe diagnostic unexpected bytes=%u\n",
                (unsigned)used);
        puts("not ok 3 - pipe-roundtrip");
        return 1;
    }
    const char *fault_argv[] = {"fault", NULL};
    int fault_pid = spawn_simple("/bin/fault", fault_argv, NULL, 0);
    int fault_status = 0;
    if (fault_pid < 0 || waitpid(fault_pid, &fault_status, 0) < 0 ||
        fault_status == 0) {
        puts("not ok 4 - user-fault-contained");
        return 1;
    }

    const char *privilege_argv[] = {"privilege_fault", NULL};
    int privilege_pid =
        spawn_simple("/bin/privilege_fault", privilege_argv, NULL, 0);
    int privilege_status = 0;
    if (privilege_pid < 0 ||
        waitpid(privilege_pid, &privilege_status, 0) < 0 ||
        privilege_status == 0) {
        puts("not ok 5 - privileged-fault-contained");
        return 1;
    }

    /* Keep the externally parsed test protocol contiguous after every
       concurrent and fault-injection phase has completed. */
    puts("TAP version 13");
    puts("1..5");
    puts("ok 1 - ring3-entry");
    puts("ok 2 - preemptive-progress");
    puts("ok 3 - pipe-roundtrip");
    puts("ok 4 - user-fault-contained");
    puts("ok 5 - privileged-fault-contained");
    puts("NORTHSTAR:M3:PASS");
    return 0;
}
