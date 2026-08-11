#include <northstar_user.h>

static int dump(int fd)
{
    unsigned char buffer[16];
    uint64_t offset = 0;
    for (;;) {
        int64_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0)
            return 1;
        if (count == 0)
            break;
        printf("%08lx  ", (unsigned long)offset);
        for (int index = 0; index < 16; ++index) {
            if (index < count)
                printf("%02x ", buffer[index]);
            else
                printf("   ");
            if (index == 7)
                putchar(' ');
        }
        printf(" |");
        for (int64_t index = 0; index < count; ++index)
            putchar(buffer[index] >= 32 && buffer[index] < 127 ? buffer[index] :
                                                                  '.');
        puts("|");
        offset += (uint64_t)count;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int fd = STDIN_FILENO;
    int result;
    if (argc > 2) {
        dprintf(STDERR_FILENO, "usage: hexdump [FILE]\n");
        return 2;
    }
    if (argc == 2) {
        fd = open(argv[1], NS_O_RDONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "hexdump: %s: errno %d\n", argv[1], errno);
            return 1;
        }
    }
    result = dump(fd);
    if (fd != STDIN_FILENO)
        (void)close(fd);
    return result;
}
