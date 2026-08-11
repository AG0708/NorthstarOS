#include <northstar/block_mem.h>
#include <northstar/nsfs.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Cross-implementation compatibility oracle.  The input is produced by the
 * independent host formatter.  This program mounts it through the kernel
 * driver, performs persistent mutations (including an indirect-block file),
 * and writes the resulting raw image for fsck.northstar to validate.
 */

static void *host_allocate(void *context, size_t size) {
    (void)context;
    return calloc(1u, size == 0u ? 1u : size);
}

static void host_deallocate(void *context, void *pointer) {
    (void)context;
    free(pointer);
}

static int read_image(const char *path, uint8_t **data, size_t *size) {
    FILE *stream = fopen(path, "rb");
    long length;

    if (stream == NULL) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(stream, 0, SEEK_END) != 0 || (length = ftell(stream)) <= 0 ||
        fseek(stream, 0, SEEK_SET) != 0 ||
        (uint64_t)length > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "cannot determine image size for %s\n", path);
        fclose(stream);
        return -1;
    }
    *size = (size_t)length;
    *data = malloc(*size);
    if (*data == NULL || fread(*data, 1u, *size, stream) != *size ||
        fgetc(stream) != EOF) {
        fprintf(stderr, "cannot read complete image %s\n", path);
        free(*data);
        *data = NULL;
        fclose(stream);
        return -1;
    }
    if (fclose(stream) != 0) {
        fprintf(stderr, "cannot close input image %s\n", path);
        free(*data);
        *data = NULL;
        return -1;
    }
    return 0;
}

static int write_image(const char *path, const uint8_t *data, size_t size) {
    FILE *stream = fopen(path, "wb");
    int result = 0;

    if (stream == NULL) {
        fprintf(stderr, "cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fwrite(data, 1u, size, stream) != size || fflush(stream) != 0) {
        fprintf(stderr, "cannot write complete image %s\n", path);
        result = -1;
    }
    if (fclose(stream) != 0) {
        fprintf(stderr, "cannot close output image %s\n", path);
        result = -1;
    }
    return result;
}

static uint8_t pattern_byte(size_t index) {
    uint64_t value = (uint64_t)index * UINT64_C(0x9e3779b97f4a7c15);
    value ^= value >> 29;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    return (uint8_t)(value ^ (value >> 32));
}

static int exercise_kernel_driver(uint8_t *image, size_t image_size) {
    struct ns_mem_block memory = {0};
    const struct nsfs_runtime runtime = {
        .context = NULL,
        .allocate = host_allocate,
        .deallocate = host_deallocate,
        .now_ns = NULL,
    };
    struct nsfs *filesystem = NULL;
    struct nsfs_statfs statfs;
    uint8_t *written = NULL;
    uint8_t *readback = NULL;
    uint32_t file_inode = 0u;
    uint32_t directory_inode = 0u;
    uint32_t nested_inode = 0u;
    const size_t file_size =
        ((size_t)NSFS_DIRECT_BLOCKS + 3u) * NSFS_BLOCK_SIZE + 137u;
    int result = -1;

    if (image_size % 512u != 0u ||
        ns_mem_block_init_borrowed(&memory, image, image_size, 512u, 0u) != 0 ||
        nsfs_mount(&memory.device, &runtime, 0u, &filesystem) != 0 ||
        nsfs_statfs(filesystem, &statfs) != 0 ||
        statfs.block_size != NSFS_BLOCK_SIZE ||
        nsfs_create(filesystem, NSFS_ROOT_INODE, "kernel-written", 14u,
                    0644u, &file_inode) != 0) {
        fprintf(stderr, "kernel driver could not mount/mutate host image\n");
        goto cleanup;
    }

    written = malloc(file_size);
    readback = malloc(file_size);
    if (written == NULL || readback == NULL) {
        fprintf(stderr, "out of memory allocating compatibility payload\n");
        goto cleanup;
    }
    for (size_t index = 0u; index < file_size; ++index) {
        written[index] = pattern_byte(index);
    }
    memset(readback, 0, file_size);
    if (nsfs_write(filesystem, file_inode, 0u, written, file_size) !=
            (int64_t)file_size ||
        nsfs_read(filesystem, file_inode, 0u, readback, file_size) !=
            (int64_t)file_size ||
        memcmp(written, readback, file_size) != 0 ||
        nsfs_mkdir(filesystem, NSFS_ROOT_INODE, "kernel-dir", 10u, 0755u,
                   &directory_inode) != 0 ||
        nsfs_create(filesystem, directory_inode, "nested", 6u, 0600u,
                    &nested_inode) != 0 ||
        nsfs_write(filesystem, nested_inode, 0u, "cross-compatible", 16u) !=
            16) {
        fprintf(stderr, "kernel driver compatibility mutation failed\n");
        goto cleanup;
    }
    if (nsfs_unmount(filesystem) != 0) {
        filesystem = NULL;
        fprintf(stderr, "kernel driver could not cleanly unmount image\n");
        goto cleanup;
    }
    filesystem = NULL;
    result = 0;

cleanup:
    if (filesystem != NULL) {
        nsfs_abandon(filesystem);
    }
    ns_mem_block_destroy(&memory);
    free(readback);
    free(written);
    return result;
}

int main(int argc, char **argv) {
    uint8_t *image = NULL;
    size_t image_size = 0u;
    int result = 1;

    if (argc != 3) {
        fprintf(stderr, "usage: %s MKFS_IMAGE OUTPUT_IMAGE\n", argv[0]);
        return 64;
    }
    if (read_image(argv[1], &image, &image_size) != 0 ||
        exercise_kernel_driver(image, image_size) != 0 ||
        write_image(argv[2], image, image_size) != 0) {
        goto cleanup;
    }
    puts("NorthstarFS host/kernel compatibility: pass");
    result = 0;

cleanup:
    free(image);
    return result;
}
