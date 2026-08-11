#include <northstar_user.h>

int main(void)
{
    volatile uint64_t *kernel = (volatile uint64_t *)0xffffffff80000000ull;
    *kernel = 0xdeadbeef;
    dprintf(STDERR_FILENO, "fault: kernel write unexpectedly survived\n");
    return 1;
}
