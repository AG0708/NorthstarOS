#include <northstar_user.h>

static int count_fd(int fd, const char *name)
{
    unsigned char buffer[4096];
    uint64_t bytes = 0, lines = 0, words = 0;
    int in_word = 0;
    for (;;) {
        int64_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0) {
            dprintf(STDERR_FILENO, "wc: %s: errno %d\n", name, errno);
            return 1;
        }
        if (count == 0)
            break;
        bytes += (uint64_t)count;
        for (int64_t index = 0; index < count; ++index) {
            unsigned char byte = buffer[index];
            int space = byte == ' ' || byte == '\t' || byte == '\n' ||
                        byte == '\r';
            if (byte == '\n')
                ++lines;
            if (!space && !in_word)
                ++words;
            in_word = !space;
        }
    }
    printf("%8lu %8lu %8lu %s\n", (unsigned long)lines,
           (unsigned long)words, (unsigned long)bytes, name);
    return 0;
}

int main(int argc, char **argv)
{
    int status = 0;
    if (argc == 1)
        return count_fd(STDIN_FILENO, "-");
    for (int index = 1; index < argc; ++index) {
        int fd = open(argv[index], NS_O_RDONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "wc: %s: errno %d\n", argv[index], errno);
            status = 1;
            continue;
        }
        status |= count_fd(fd, argv[index]);
        (void)close(fd);
    }
    return status;
}
