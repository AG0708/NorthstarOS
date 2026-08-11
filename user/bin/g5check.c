#include <northstar_user.h>

struct expected_file {
    const char *path;
    const char *contents;
    const char *label;
};

static int verify_file(const struct expected_file *expected)
{
    char buffer[128];
    size_t length = strlen(expected->contents);
    int fd = open(expected->path, NS_O_RDONLY, 0);
    int64_t count;

    if (fd < 0) {
        dprintf(STDERR_FILENO, "g5check: open %s failed: errno %d\n",
                expected->path, errno);
        return -1;
    }
    count = read(fd, buffer, sizeof(buffer));
    if (count != (int64_t)length ||
        memcmp(buffer, expected->contents, length) != 0 ||
        read(fd, buffer, 1) != 0) {
        dprintf(STDERR_FILENO, "g5check: %s has unexpected contents\n",
                expected->path);
        (void)close(fd);
        return -1;
    }
    if (close(fd) != 0) {
        dprintf(STDERR_FILENO, "g5check: close %s failed: errno %d\n",
                expected->path, errno);
        return -1;
    }
    return 0;
}

static int write_manifest(void)
{
    static const char manifest[] =
        "pipeline=ok\n"
        "redirection=ok\n"
        "background-wait=0\n"
        "fault-status=142\n"
        "persistent-read=ok\n"
        "fault-recovery=ok\n";
    int fd = open("/persist/g5-proof.manifest",
                  NS_O_WRONLY | NS_O_CREAT | NS_O_TRUNC, 0644u);
    size_t offset = 0;

    if (fd < 0)
        return -1;
    while (offset < sizeof(manifest) - 1u) {
        int64_t count = write(fd, manifest + offset,
                              sizeof(manifest) - 1u - offset);
        if (count <= 0) {
            (void)close(fd);
            return -1;
        }
        offset += (size_t)count;
    }
    return close(fd);
}

int main(void)
{
    static const struct expected_file files[] = {
        {"/persist/pipeline.out", "PIPELINE_OK\n", "pipeline"},
        {"/persist/redirection.out", "REDIRECTION_OK\n", "redirection"},
        {"/persist/wait-status.out", "0\n", "background"},
        {"/persist/fault-status.out", "142\n", "exit-status"},
        {"/persist/persist-read.out", "PERSISTED_FROM_BOOT_ONE\n",
         "persistent-read"},
        {"/persist/survived.out", "SHELL_SURVIVED_FAULT\n",
         "fault-recovery"},
    };

    for (size_t index = 0; index < sizeof(files) / sizeof(files[0]); ++index) {
        if (verify_file(&files[index]) != 0)
            return 1;
        dprintf(STDOUT_FILENO, "# NS_TEST shell.%s PASS\n",
                files[index].label);
    }
    if (write_manifest() != 0) {
        dprintf(STDERR_FILENO,
                "g5check: could not persist proof manifest: errno %d\n",
                errno);
        return 1;
    }
    puts("NORTHSTAR:G5:USERLAND:PASS");
    return 0;
}
