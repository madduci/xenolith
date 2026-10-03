#ifndef XENOLITH_SSE_STREAM_H
#define XENOLITH_SSE_STREAM_H

#include <stddef.h>
#include <sys/types.h>

/* A pipe-backed byte stream used to bridge the inference worker thread
 * (producer) to the libmicrohttpd response callback (consumer). */
typedef struct {
    int read_fd;
    int write_fd;
    volatile int aborted;
} sse_stream_t;

sse_stream_t *sse_stream_create(void);
void          sse_stream_destroy(sse_stream_t *s);

/* Producer side. Returns 0 on success, -1 if the consumer closed the stream. */
int  sse_stream_write(sse_stream_t *s, const char *data, size_t len);

/* Producer side: signal end of stream (do not free here). */
void sse_stream_finish(sse_stream_t *s);

/* Consumer side: read a chunk. Returns >0 bytes, 0 on EOF, -1 on error. */
ssize_t sse_stream_read(sse_stream_t *s, char *buf, size_t max);

/* Consumer side: abort and release fds (used from MHD cleanup callback). */
void sse_stream_abort(sse_stream_t *s);

#endif