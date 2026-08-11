#include <northstar_user.h>

static int copy_fd(int fd, const char *name)
{
    unsigned char buffer[4096];
    for (;;) {
        int64_t count = read(fd, buffer, sizeof(buffer));
        if (count == 0)
            return 0;
        if (count < 0) {
            dprintf(STDERR_FILENO, "cat: %s: read error %d\n", name, errno);
            return 1;
        }
        size_t offset = 0;
        while (offset < (size_t)count) {
            int64_t written = write(STDOUT_FILENO, buffer + offset,
                                    (size_t)count - offset);
            if (written <= 0) {
                dprintf(STDERR_FILENO, "cat: write error %d\n", errno);
                return 1;
            }
            offset += (size_t)written;
        }
    }
}

int main(int argc, char **argv)
{
    int status = 0;
    if (argc == 1)
        return copy_fd(STDIN_FILENO, "stdin");
    for (int index = 1; index < argc; ++index) {
        int fd;
        if (strcmp(argv[index], "-") == 0) {
            status |= copy_fd(STDIN_FILENO, "stdin");
            continue;
        }
        fd = open(argv[index], NS_O_RDONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "cat: %s: errno %d\n", argv[index], errno);
            status = 1;
            continue;
        }
        status |= copy_fd(fd, argv[index]);
        (void)close(fd);
    }
    return status;
}
