#include <northstar/proc_pipe.h>

#include <northstar/vfs.h>

#include <stddef.h>
#include <stdint.h>

struct ns_pipe {
    struct ns_pipe_runtime runtime;
    unsigned char *buffer;
    size_t capacity;
    size_t head;
    size_t used;
    uint32_t readers;
    uint32_t writers;
};

struct ns_pipe_endpoint {
    struct ns_pipe *pipe;
    uint8_t writable;
};

static uintptr_t pipe_lock(struct ns_pipe *pipe)
{
    if (pipe->runtime.lock != NULL)
        return pipe->runtime.lock(pipe->runtime.context);
    return 0;
}

static void pipe_unlock(struct ns_pipe *pipe, uintptr_t token)
{
    if (pipe->runtime.unlock != NULL)
        pipe->runtime.unlock(pipe->runtime.context, token);
}

static void pipe_wake(struct ns_pipe *pipe, enum ns_wait_kind kind,
                      size_t maximum)
{
    if (pipe->runtime.wake != NULL)
        (void)pipe->runtime.wake(pipe->runtime.context, kind,
                                 (uintptr_t)pipe, maximum);
}

static void pipe_free(struct ns_pipe *pipe)
{
    struct ns_pipe_runtime runtime = pipe->runtime;
    runtime.deallocate(runtime.context, pipe->buffer, pipe->capacity, 1u);
    runtime.deallocate(runtime.context, pipe, sizeof(*pipe),
                       _Alignof(struct ns_pipe));
}

static int64_t pipe_read(void *context, uint64_t *offset, void *buffer,
                         size_t count, uint32_t status_flags)
{
    struct ns_pipe_endpoint *endpoint = context;
    struct ns_pipe *pipe;
    unsigned char *output = buffer;
    size_t copied;
    uintptr_t token;
    (void)offset;
    (void)status_flags;
    if (endpoint == NULL || endpoint->writable || buffer == NULL)
        return -NS_EBADF;
    if (count == 0)
        return 0;
    pipe = endpoint->pipe;
    for (;;) {
        token = pipe_lock(pipe);
        if (pipe->used != 0)
            break;
        if (pipe->writers == 0) {
            pipe_unlock(pipe, token);
            return 0;
        }
        if (pipe->runtime.prepare_block == NULL ||
            pipe->runtime.commit_block == NULL) {
            pipe_unlock(pipe, token);
            return -NS_EAGAIN;
        }
        pipe->runtime.prepare_block(pipe->runtime.context, NS_WAIT_PIPE_READ,
                                    (uintptr_t)pipe);
        pipe_unlock(pipe, token);
        pipe->runtime.commit_block(pipe->runtime.context);
    }
    copied = count < pipe->used ? count : pipe->used;
    for (size_t index = 0; index < copied; ++index)
        output[index] = pipe->buffer[(pipe->head + index) % pipe->capacity];
    pipe->head = (pipe->head + copied) % pipe->capacity;
    pipe->used -= copied;
    pipe_unlock(pipe, token);
    pipe_wake(pipe, NS_WAIT_PIPE_WRITE, (size_t)-1);
    return (int64_t)copied;
}

static int64_t pipe_write(void *context, uint64_t *offset, const void *buffer,
                          size_t count, uint32_t status_flags)
{
    struct ns_pipe_endpoint *endpoint = context;
    struct ns_pipe *pipe;
    const unsigned char *input = buffer;
    size_t written = 0;
    uintptr_t token;
    (void)offset;
    (void)status_flags;
    if (endpoint == NULL || !endpoint->writable || buffer == NULL)
        return -NS_EBADF;
    if (count == 0)
        return 0;
    pipe = endpoint->pipe;
    while (written < count) {
        size_t available;
        size_t chunk;
        token = pipe_lock(pipe);
        if (pipe->readers == 0) {
            pipe_unlock(pipe, token);
            return written != 0 ? (int64_t)written : -NS_EPIPE;
        }
        available = pipe->capacity - pipe->used;
        if (count <= NS_PIPE_ATOMIC_MAX && available < count) {
            if (pipe->runtime.prepare_block == NULL ||
                pipe->runtime.commit_block == NULL) {
                pipe_unlock(pipe, token);
                return -NS_EAGAIN;
            }
            pipe->runtime.prepare_block(pipe->runtime.context,
                                        NS_WAIT_PIPE_WRITE, (uintptr_t)pipe);
            pipe_unlock(pipe, token);
            pipe->runtime.commit_block(pipe->runtime.context);
            continue;
        }
        if (available == 0) {
            pipe_unlock(pipe, token);
            if (written != 0)
                return (int64_t)written;
            if (pipe->runtime.prepare_block == NULL ||
                pipe->runtime.commit_block == NULL)
                return -NS_EAGAIN;
            token = pipe_lock(pipe);
            if (pipe->capacity != pipe->used) {
                pipe_unlock(pipe, token);
                continue;
            }
            pipe->runtime.prepare_block(pipe->runtime.context,
                                        NS_WAIT_PIPE_WRITE, (uintptr_t)pipe);
            pipe_unlock(pipe, token);
            pipe->runtime.commit_block(pipe->runtime.context);
            continue;
        }
        chunk = count - written;
        if (chunk > available)
            chunk = available;
        for (size_t index = 0; index < chunk; ++index) {
            size_t tail = (pipe->head + pipe->used + index) % pipe->capacity;
            pipe->buffer[tail] = input[written + index];
        }
        pipe->used += chunk;
        written += chunk;
        pipe_unlock(pipe, token);
        pipe_wake(pipe, NS_WAIT_PIPE_READ, (size_t)-1);
    }
    return (int64_t)written;
}

static int pipe_stat(void *context, struct ns_vfs_node_info *result)
{
    struct ns_pipe_endpoint *endpoint = context;
    struct ns_pipe *pipe;
    uintptr_t token;
    if (endpoint == NULL || result == NULL)
        return -NS_EINVAL;
    pipe = endpoint->pipe;
    token = pipe_lock(pipe);
    result->inode = (uint64_t)(uintptr_t)pipe;
    result->size = pipe->used;
    result->blocks = 0;
    result->mtime_ns = 0;
    result->mode = endpoint->writable ? 0200u : 0400u;
    result->type = NS_FT_PIPE;
    pipe_unlock(pipe, token);
    return 0;
}

static void pipe_close(void *context)
{
    struct ns_pipe_endpoint *endpoint = context;
    struct ns_pipe *pipe;
    struct ns_pipe_runtime runtime;
    uintptr_t token;
    int release;
    if (endpoint == NULL)
        return;
    pipe = endpoint->pipe;
    runtime = pipe->runtime;
    token = pipe_lock(pipe);
    if (endpoint->writable) {
        if (pipe->writers != 0)
            --pipe->writers;
    } else if (pipe->readers != 0) {
        --pipe->readers;
    }
    release = pipe->readers == 0 && pipe->writers == 0;
    pipe_unlock(pipe, token);
    if (endpoint->writable)
        pipe_wake(pipe, NS_WAIT_PIPE_READ, (size_t)-1);
    else
        pipe_wake(pipe, NS_WAIT_PIPE_WRITE, (size_t)-1);
    runtime.deallocate(runtime.context, endpoint, sizeof(*endpoint),
                       _Alignof(struct ns_pipe_endpoint));
    if (release)
        pipe_free(pipe);
}

static const struct ns_vfs_file_ops read_ops = {
    .read = pipe_read,
    .write = NULL,
    .seek = NULL,
    .stat = pipe_stat,
    .readdir = NULL,
    .close = pipe_close,
};

static const struct ns_vfs_file_ops write_ops = {
    .read = NULL,
    .write = pipe_write,
    .seek = NULL,
    .stat = pipe_stat,
    .readdir = NULL,
    .close = pipe_close,
};

int ns_pipe_create(const struct ns_pipe_runtime *runtime,
                   struct ns_vfs_fdtable *table, int descriptors[2])
{
    struct ns_pipe *pipe;
    struct ns_pipe_endpoint *reader;
    struct ns_pipe_endpoint *writer;
    struct ns_vfs_file *read_file = NULL;
    struct ns_vfs_file *write_file = NULL;
    size_t capacity;
    int read_fd = -1;
    int write_fd = -1;
    int error;
    if (runtime == NULL || runtime->vfs == NULL || runtime->allocate == NULL ||
        runtime->deallocate == NULL || table == NULL || descriptors == NULL)
        return -NS_EINVAL;
    capacity = runtime->capacity != 0 ? runtime->capacity
                                      : NS_PIPE_DEFAULT_CAPACITY;
    if (capacity < NS_PIPE_ATOMIC_MAX)
        return -NS_EINVAL;
    pipe = runtime->allocate(runtime->context, sizeof(*pipe),
                             _Alignof(struct ns_pipe));
    reader = runtime->allocate(runtime->context, sizeof(*reader),
                               _Alignof(struct ns_pipe_endpoint));
    writer = runtime->allocate(runtime->context, sizeof(*writer),
                               _Alignof(struct ns_pipe_endpoint));
    if (pipe == NULL || reader == NULL || writer == NULL) {
        error = -NS_ENOMEM;
        goto fail_alloc;
    }
    pipe->buffer = runtime->allocate(runtime->context, capacity, 1u);
    if (pipe->buffer == NULL) {
        error = -NS_ENOMEM;
        goto fail_alloc;
    }
    pipe->runtime = *runtime;
    pipe->capacity = capacity;
    pipe->head = 0;
    pipe->used = 0;
    pipe->readers = 1;
    pipe->writers = 1;
    reader->pipe = pipe;
    reader->writable = 0;
    writer->pipe = pipe;
    writer->writable = 1;
    error = ns_vfs_file_create(runtime->vfs, &read_ops, reader, 0, &read_file);
    if (error != 0)
        goto fail_pipe;
    error = ns_vfs_file_create(runtime->vfs, &write_ops, writer, NS_O_WRONLY,
                               &write_file);
    if (error != 0) {
        ns_vfs_file_release(read_file);
        /* Releasing both endpoint objects also releases the shared pipe. */
        pipe_close(writer);
        return error;
    }
    read_fd = ns_vfs_fd_install(table, read_file, 0, false);
    if (read_fd < 0) {
        error = read_fd;
        goto fail_files;
    }
    write_fd = ns_vfs_fd_install(table, write_file, 0, false);
    if (write_fd < 0) {
        error = write_fd;
        (void)ns_vfs_close(table, read_fd);
        read_fd = -1;
        goto fail_files;
    }
    ns_vfs_file_release(read_file);
    ns_vfs_file_release(write_file);
    descriptors[NS_PIPE_READ] = read_fd;
    descriptors[NS_PIPE_WRITE] = write_fd;
    return 0;

fail_files:
    ns_vfs_file_release(read_file);
    ns_vfs_file_release(write_file);
    return error;

fail_pipe:
    runtime->deallocate(runtime->context, pipe->buffer, capacity, 1u);
fail_alloc:
    if (writer != NULL)
        runtime->deallocate(runtime->context, writer, sizeof(*writer),
                            _Alignof(struct ns_pipe_endpoint));
    if (reader != NULL)
        runtime->deallocate(runtime->context, reader, sizeof(*reader),
                            _Alignof(struct ns_pipe_endpoint));
    if (pipe != NULL)
        runtime->deallocate(runtime->context, pipe, sizeof(*pipe),
                            _Alignof(struct ns_pipe));
    return error;
}
