#define _GNU_SOURCE
#include "sse_stream.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PIPE_BUF_BYTES (1024 * 1024)

sse_stream_t *sse_stream_create(void)
{
    sse_stream_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;

    int fds[2];
    if (pipe(fds) != 0) { free(s); return NULL; }

    s->read_fd  = fds[0];
    s->write_fd = fds[1];

#ifdef F_SETPIPE_SZ
    /* Best-effort: grow the pipe buffer to reduce writer stalls. */
    (void)fcntl(s->write_fd, F_SETPIPE_SZ, PIPE_BUF_BYTES);
#endif

    return s;
}

void sse_stream_destroy(sse_stream_t *s)
{
    if (!s) return;
    if (s->read_fd  >= 0) close(s->read_fd);
    if (s->write_fd >= 0) close(s->write_fd);
    free(s);
}

int sse_stream_write(sse_stream_t *s, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        if (s->aborted) return -1;
        ssize_t n = write(s->write_fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) return -1;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

void sse_stream_finish(sse_stream_t *s)
{
    if (s && s->write_fd >= 0) {
        close(s->write_fd);
        s->write_fd = -1;
    }
}

ssize_t sse_stream_read(sse_stream_t *s, char *buf, size_t max)
{
    ssize_t n;
    do {
        n = read(s->read_fd, buf, max);
    } while (n < 0 && errno == EINTR);
    return n;
}

void sse_stream_abort(sse_stream_t *s)
{
    if (!s) return;
    s->aborted = 1;
    if (s->read_fd >= 0)  { close(s->read_fd);  s->read_fd  = -1; }
    if (s->write_fd >= 0) { close(s->write_fd); s->write_fd = -1; }
}