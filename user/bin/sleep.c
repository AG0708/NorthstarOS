#include <northstar_user.h>

int main(int argc, char **argv)
{
    char *end;
    long seconds;
    if (argc != 2) {
        dprintf(STDERR_FILENO, "usage: sleep SECONDS\n");
        return 2;
    }
    seconds = strtol(argv[1], &end, 10);
    if (*end != '\0' || seconds < 0) {
        dprintf(STDERR_FILENO, "sleep: invalid duration\n");
        return 2;
    }
    while (seconds-- > 0) {
        if (sleep_ns(1000000000ull) != 0)
            return 1;
    }
    return 0;
}
