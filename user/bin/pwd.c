#include <northstar_user.h>

int main(void)
{
    char path[NS_PATH_MAX];
    if (getcwd(path, sizeof(path)) != 0) {
        dprintf(STDERR_FILENO, "pwd: errno %d\n", errno);
        return 1;
    }
    puts(path);
    return 0;
}
