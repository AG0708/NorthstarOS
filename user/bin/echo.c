#include <northstar_user.h>

int main(int argc, char **argv)
{
    int newline = 1;
    int first = 1;
    for (int index = 1; index < argc; ++index) {
        if (index == 1 && strcmp(argv[index], "-n") == 0) {
            newline = 0;
            continue;
        }
        if (!first && write(STDOUT_FILENO, " ", 1) != 1)
            return 1;
        size_t length = strlen(argv[index]);
        if (write(STDOUT_FILENO, argv[index], length) != (int64_t)length)
            return 1;
        first = 0;
    }
    if (newline && write(STDOUT_FILENO, "\n", 1) != 1)
        return 1;
    return 0;
}
