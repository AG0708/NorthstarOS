#include <northstar_user.h>

int main(void)
{
    struct ns_abi_timespec time;
    if (clock_monotonic(&time) != 0) {
        dprintf(STDERR_FILENO, "sysinfo: clock: errno %d\n", errno);
        return 1;
    }
    printf("NorthstarOS ABI %d\npid: %d\nuptime: %ld.%09ld s\n",
           abi_version(), getpid(), (long)time.tv_sec, (long)time.tv_nsec);
    return 0;
}
