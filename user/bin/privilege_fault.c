#include <northstar_user.h>

int main(void)
{
    /* CLI is privileged at CPL3 and must raise #GP without reaching the next
       instruction or affecting any other process. */
    __asm__ volatile("cli" ::: "memory");
    dprintf(STDERR_FILENO,
            "privilege_fault: privileged instruction unexpectedly survived\n");
    return 1;
}
