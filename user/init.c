#include <northstar_user.h>

static uint64_t pointer(const void *value) { return (uint64_t)(uintptr_t)value; }

int main(void)
{
    static const char *const arguments[] = {"sh", NULL};
    static const char *const environment[] = {
        "PATH=/bin", "HOME=/", "TERM=northstar", NULL};
    const char *shell = "/bin/sh";
    int test_mode = getenv("NORTHSTAR_TEST") != NULL;

    dprintf(STDOUT_FILENO, "NORTHSTAR:INIT:READY pid=%d\n", getpid());
    for (;;) {
        struct ns_spawn_args spawn_arguments = {
            .path = pointer(shell),
            .argv = pointer(arguments),
            .envp = pointer(environment),
        };
        int pid = spawn(&spawn_arguments);
        int status = 0;
        if (pid < 0) {
            dprintf(STDERR_FILENO, "init: cannot start %s: errno %d\n", shell,
                    errno);
            if (strcmp(shell, "/bin/sh") == 0) {
                shell = "/bin/shell";
                continue;
            }
            sleep_ns(1000000000ull);
            continue;
        }
        if (waitpid(pid, &status, 0) < 0) {
            dprintf(STDERR_FILENO, "init: wait failed: errno %d\n", errno);
            if (test_mode)
                return 125;
        } else if (test_mode) {
            if (status == 0)
                puts("NORTHSTAR:M4:SHELL:PASS");
            else
                dprintf(STDERR_FILENO,
                        "NORTHSTAR:M4:SHELL:FAIL status=%d\n", status);
            return status;
        } else {
            dprintf(STDERR_FILENO,
                    "init: shell exited with status %d; restarting\n", status);
        }
        sleep_ns(250000000ull);
    }
}
