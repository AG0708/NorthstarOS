#include <northstar_user.h>

static char type_character(uint8_t type)
{
    switch (type) {
    case NS_FT_DIRECTORY: return 'd';
    case NS_FT_CHAR: return 'c';
    case NS_FT_BLOCK: return 'b';
    case NS_FT_PIPE: return 'p';
    case NS_FT_SOCKET: return 's';
    default: return '-';
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : ".";
    int fd = open(path, NS_O_RDONLY | NS_O_DIRECTORY, 0);
    struct ns_abi_dirent entries[16];
    if (fd < 0) {
        dprintf(STDERR_FILENO, "ls: %s: errno %d\n", path, errno);
        return 1;
    }
    for (;;) {
        int64_t count = readdir(fd, entries, 16);
        if (count < 0) {
            dprintf(STDERR_FILENO, "ls: readdir: errno %d\n", errno);
            (void)close(fd);
            return 1;
        }
        if (count == 0)
            break;
        for (int64_t index = 0; index < count; ++index)
            printf("%c %8lu %s\n", type_character(entries[index].type),
                   (unsigned long)entries[index].inode, entries[index].name);
    }
    (void)close(fd);
    return 0;
}
