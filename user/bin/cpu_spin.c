#include <northstar_user.h>

int main(int argc, char **argv)
{
    const char *label = argc > 1 ? argv[1] : "?";
    volatile uint64_t state = 0x9e3779b97f4a7c15ull ^ (uint64_t)getpid() ^
                              (uint64_t)(unsigned char)label[0];

    /* No system call or voluntary yield occurs in this interval.  The kernel
       can make all four workers progress only by timer preemption. */
    for (uint64_t iteration = 0; iteration < 8000000ull; ++iteration) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
    }
    return state == 0;
}
