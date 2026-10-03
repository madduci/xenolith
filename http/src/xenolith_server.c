#define _GNU_SOURCE
#include "internal.h"
#include "sse_stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>

static void *ping_thread(void *arg)
{
    sse_stream_t *s = arg;
    int ticks = 0;
    const char *ping =
        "event: ping\n"
        "data: {\"type\":\"ping\"}\n\n";

    while (!s->aborted) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 500 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (s->aborted) break;

        if (++ticks >= 50) {          /* 50 * 500ms = 25s */
            ticks = 0;
            if (sse_stream_write(s, ping, strlen(ping)) != 0) {
                s->aborted = 1;
                break;
            }
        }
    }
    return NULL;
}

const xenolith_backend_t *g_backend = NULL;
int                       g_verbose = 0;

static struct MHD_Daemon *g_daemon = NULL;

/* ------------------------------------------------------------------ */
/* message extraction & lifetime                                       */
/* ------------------------------------------------------------------ */

/* Concatenate all text parts of a content field into a single string.
 * Handles both the OpenAI and Anthropic shapes:
 *   "content": "plain string"
 *   "content": [ {"type":"text","text":"..."}, ... ]
 * Returns a newly-allocated NUL-terminated string (never NULL). */
static char *content_to_text(json_t *content)
{
    if (!content) return strdup("");

    if (json_is_string(content))
        return strdup(json_string_value(content));

    if (json_is_array(content)) {
        char  *buf = NULL;
        size_t len = 0, cap = 0;

        size_t idx;
        json_t *part;
        json_array_foreach(content, idx, part) {
            if (!json_is_object(part)) continue;
            const char *t = json_string_value(json_object_get(part, "text"));
            if (!t) continue;

            size_t tl = strlen(t);
            if (len + tl + 2 > cap) {
                size_t nc = cap ? cap * 2 : 256;
                while (nc < len + tl + 2) nc *= 2;
                buf = realloc(buf, nc);
                cap = nc;
            }
            if (len) buf[len++] = '\n';
            memcpy(buf + len, t, tl);
            len += tl;
            buf[len] = 0;
        }
        return buf ? buf : strdup("");
    }

    return strdup("");
}

/* Extract role/content pairs from an OpenAI or Anthropic "messages"
 * array. If `system` is non-NULL and non-empty it is prepended as a
 * system message. Both role and content are strdup'd; the caller is
 * responsible for releasing them with free_messages(). */
xenolith_message_t *extract_messages(json_t *messages,
                                     const char *system,
                                     size_t *count_out)
{
    if (count_out) *count_out = 0;
    if (!json_is_array(messages)) return NULL;

    size_t n = json_array_size(messages) + (system && *system ? 1 : 0);
    if (n == 0) return NULL;

    xenolith_message_t *out = calloc(n, sizeof *out);
    if (!out) return NULL;

    size_t k = 0;

    if (system && *system) {
        out[k].role    = strdup("system");
        out[k].content = strdup(system);
        k++;
    }

    size_t idx;
    json_t *m;
    json_array_foreach(messages, idx, m) {
        if (!json_is_object(m)) continue;
        const char *role = json_string_value(json_object_get(m, "role"));
        if (!role) continue;

        char *text = content_to_text(json_object_get(m, "content"));
        if (!text) continue;
        if (!*text) { free(text); continue; }  /* skip empty content */

        out[k].role    = strdup(role);
        out[k].content = text;
        k++;
    }

    if (k == 0) {
        free(out);
        return NULL;
    }

    if (count_out) *count_out = k;
    return out;
}

void free_messages(xenolith_message_t *m, size_t n)
{
    if (!m) return;
    for (size_t i = 0; i < n; i++) {
        free((char *)m[i].role);
        free((char *)m[i].content);
    }
    free(m);
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

void make_uuid(char *out, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[16];
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { (void)fread(raw, 1, sizeof raw, f); fclose(f); }
    else {
        unsigned s = (unsigned)time(NULL) ^ (unsigned)getpid();
        for (size_t i = 0; i < sizeof raw; i++)
            raw[i] = (unsigned char)(s = s * 1103515245u + 12345u);
    }
    size_t j = 0;
    for (size_t i = 0; i < sizeof raw && j + 2 < n; i++) {
        out[j++] = hex[raw[i] >> 4];
        out[j++] = hex[raw[i] & 0xf];
        if (i == 3 || i == 5 || i == 7 || i == 9) {
            if (j + 1 < n) out[j++] = '-';
        }
    }
    out[j < n ? j : n - 1] = 0;
}

int estimate_tokens(const char *text)
{
    if (!text) return 0;
    size_t n = strlen(text);
    return (int)((n + 3) / 4);
}

enum MHD_Result send_json(struct MHD_Connection *conn,
                          unsigned int status,
                          json_t *root)
{
    char *body = json_dumps(root, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(root);
    if (!body) return MHD_NO;

    struct MHD_Response *resp =
        MHD_create_response_from_buffer(strlen(body), body, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(resp, "Content-Type", "application/json");
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    enum MHD_Result ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

enum MHD_Result send_json_error(struct MHD_Connection *conn,
                                unsigned int status,
                                const char *type,
                                const char *message)
{
    json_t *root = json_pack("{s:{s:s, s:s}}",
                             "error",
                             "type",    type    ? type    : "error",
                             "message", message ? message : "");
    return send_json(conn, status, root);
}

/* ------------------------------------------------------------------ */
/* message copying & flattening                                        */
/* ------------------------------------------------------------------ */

/* Deep copy: both role and content are strdup'd. */
static xenolith_message_t *copy_messages(const xenolith_message_t *src, size_t n)
{
    if (!src || n == 0) return NULL;
    xenolith_message_t *dst = calloc(n, sizeof *dst);
    if (!dst) return NULL;
    for (size_t i = 0; i < n; i++) {
        dst[i].role    = strdup(src[i].role    ? src[i].role    : "");
        dst[i].content = strdup(src[i].content ? src[i].content : "");
    }
    return dst;
}

static void free_copied_messages(xenolith_message_t *m, size_t n)
{
    if (!m) return;
    for (size_t i = 0; i < n; i++) {
        free((char *)m[i].role);
        free((char *)m[i].content);
    }
    free(m);
}

/* Fallback for backends without generate_chat: "role: content\n" */
char *flatten_messages(const xenolith_message_t *m, size_t n)
{
    char  *out = NULL;
    size_t len = 0, cap = 0;
    for (size_t i = 0; i < n; i++) {
        const char *r = m[i].role ? m[i].role : "";
        const char *c = m[i].content ? m[i].content : "";
        size_t rl = strlen(r), cl = strlen(c);
        if (len + rl + cl + 3 > cap) {
            size_t nc = cap ? cap * 2 : 256;
            while (nc < len + rl + cl + 3) nc *= 2;
            out = realloc(out, nc);
            cap = nc;
        }
        memcpy(out + len, r, rl); len += rl;
        out[len++] = ':';
        out[len++] = ' ';
        memcpy(out + len, c, cl); len += cl;
        out[len++] = '\n';
        out[len]   = 0;
    }
    if (!out) out = strdup("");
    return out;
}

/* Dispatch: prefer structured, fall back to flattened. */
static int run_generate(const xenolith_backend_t  *be,
                        const xenolith_message_t  *msgs,
                        size_t                     n,
                        int                        max_tokens,
                        float                      temperature,
                        xenolith_token_cb          cb,
                        void                      *ud)
{
    if (be->generate_chat)
        return be->generate_chat(msgs, n, max_tokens, temperature, cb, ud);

    if (!be->generate) return -1;
    char *prompt = flatten_messages(msgs, n);
    int rc = be->generate(prompt, max_tokens, temperature, cb, ud);
    free(prompt);
    return rc;
}

/* ------------------------------------------------------------------ */
/* buffered completion                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t len, cap;
} collect_t;

static int collect_cb(const char *tok, size_t len, void *ud)
{
    collect_t *c = ud;
    if (c->len + len + 1 > c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 256;
        while (nc < c->len + len + 1) nc *= 2;
        c->buf = realloc(c->buf, nc);
        c->cap = nc;
    }
    memcpy(c->buf + c->len, tok, len);
    c->len += len;
    c->buf[c->len] = 0;
    return 0;
}

int run_buffered_completion(const xenolith_message_t *messages,
                            size_t                    n_messages,
                            int                       max_tokens,
                            float                     temperature,
                            char                    **out_text,
                            int                      *out_prompt_tokens,
                            int                      *out_completion_tokens)
{
    collect_t c = {0};
    int rc = run_generate(g_backend, messages, n_messages,
                          max_tokens, temperature, collect_cb, &c);
    if (rc != 0) { free(c.buf); return rc; }
    if (!c.buf) c.buf = strdup("");

    *out_text = c.buf;

    if (out_prompt_tokens) {
        char *flat = flatten_messages(messages, n_messages);
        *out_prompt_tokens = estimate_tokens(flat);
        free(flat);
    }
    if (out_completion_tokens)
        *out_completion_tokens = estimate_tokens(c.buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* streaming completion                                                */
/* ------------------------------------------------------------------ */

/* Job handed to the worker thread. Owns a deep copy of the messages. */
typedef struct {
    sse_stream_t             *stream;
    const xenolith_backend_t *backend;
    int                       format;         /* 0 = OpenAI, 1 = Anthropic */
    int                       max_tokens;
    float                     temperature;
    int                       prompt_tokens;
    char                      model[128];
    xenolith_message_t       *messages;
    size_t                    n_messages;
} stream_job_t;

/* Per-stream state shared between worker and token callbacks. */
typedef struct {
    sse_stream_t *stream;
    const char   *id;
    const char   *model;
    int           first;
    int           any;
    int           token_count;
} stream_state_t;

static void write_all(sse_stream_t *s, const char *data, size_t len)
{
    if (sse_stream_write(s, data, len) != 0)
        s->aborted = 1;
}

/* --- OpenAI emit helpers ----------------------------------------- */

static int stream_emit_openai(const char *tok, size_t len, void *ud)
{
    stream_state_t *st = ud;
    if (st->stream->aborted) return 1;

    json_t *delta = json_object();
    json_object_set_new(delta, "content", json_stringn(tok, len));

    json_t *choice = json_object();
    json_object_set_new(choice, "index",         json_integer(0));
    json_object_set_new(choice, "delta",         delta);
    json_object_set_new(choice, "finish_reason", json_null());

    json_t *choices = json_array();
    json_array_append_new(choices, choice);

    json_t *frame = json_object();
    json_object_set_new(frame, "id",      json_str(st->id));
    json_object_set_new(frame, "object",  json_lit("chat.completion.chunk"));
    json_object_set_new(frame, "created", json_integer((json_int_t)time(NULL)));
    json_object_set_new(frame, "model",   json_str(st->model));
    json_object_set_new(frame, "choices", choices);

    char *body = json_dumps(frame, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(frame);
    if (body) {
        char *s = NULL;
        if (asprintf(&s, "data: %s\n\n", body) > 0) {
            write_all(st->stream, s, strlen(s));
            free(s);
        }
        free(body);
    }
    st->any = 1;
    st->token_count++;
    return st->stream->aborted ? 1 : 0;
}

static void emit_openai_header(stream_state_t *st)
{
    json_t *delta = json_object();
    json_object_set_new(delta, "role", json_lit("assistant"));

    json_t *choice = json_object();
    json_object_set_new(choice, "index",         json_integer(0));
    json_object_set_new(choice, "delta",         delta);
    json_object_set_new(choice, "finish_reason", json_null());

    json_t *choices = json_array();
    json_array_append_new(choices, choice);

    json_t *frame = json_object();
    json_object_set_new(frame, "id",      json_str(st->id));
    json_object_set_new(frame, "object",  json_lit("chat.completion.chunk"));
    json_object_set_new(frame, "created", json_integer((json_int_t)time(NULL)));
    json_object_set_new(frame, "model",   json_str(st->model));
    json_object_set_new(frame, "choices", choices);

    char *body = json_dumps(frame, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(frame);
    if (body) {
        char *s = NULL;
        if (asprintf(&s, "data: %s\n\n", body) > 0) {
            write_all(st->stream, s, strlen(s));
            free(s);
        }
        free(body);
    }
}

static void emit_openai_footer(stream_state_t *st, const char *reason)
{
    json_t *choice = json_object();
    json_object_set_new(choice, "index",         json_integer(0));
    json_object_set_new(choice, "delta",         json_object());
    json_object_set_new(choice, "finish_reason", json_str(reason));

    json_t *choices = json_array();
    json_array_append_new(choices, choice);

    json_t *frame = json_object();
    json_object_set_new(frame, "id",      json_str(st->id));
    json_object_set_new(frame, "object",  json_lit("chat.completion.chunk"));
    json_object_set_new(frame, "created", json_integer((json_int_t)time(NULL)));
    json_object_set_new(frame, "model",   json_str(st->model));
    json_object_set_new(frame, "choices", choices);

    char *body = json_dumps(frame, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(frame);
    if (body) {
        char *s = NULL;
        if (asprintf(&s, "data: %s\n\ndata: [DONE]\n\n", body) > 0) {
            write_all(st->stream, s, strlen(s));
            free(s);
        }
        free(body);
    }
}

/* --- Anthropic emit helpers --------------------------------------- */

static int stream_emit_anthropic(const char *tok, size_t len, void *ud)
{
    stream_state_t *st = ud;
    if (st->stream->aborted) return 1;

    json_t *delta = json_object();
    json_object_set_new(delta, "type", json_lit("text_delta"));
    json_object_set_new(delta, "text", json_stringn(tok, len));

    json_t *frame = json_object();
    json_object_set_new(frame, "type",  json_lit("content_block_delta"));
    json_object_set_new(frame, "index", json_integer(0));
    json_object_set_new(frame, "delta", delta);

    char *body = json_dumps(frame, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(frame);
    if (body) {
        char *s = NULL;
        if (asprintf(&s, "event: content_block_delta\ndata: %s\n\n", body) > 0) {
            write_all(st->stream, s, strlen(s));
            free(s);
        }
        free(body);
    }
    st->any = 1;
    st->token_count++;
    return st->stream->aborted ? 1 : 0;
}

static void emit_anthropic_start(stream_state_t *st, int input_tokens)
{
    json_t *msg = json_object();
    json_object_set_new(msg, "id",            json_str(st->id));
    json_object_set_new(msg, "type",          json_lit("message"));
    json_object_set_new(msg, "role",          json_lit("assistant"));
    json_object_set_new(msg, "model",         json_str(st->model));
    json_object_set_new(msg, "content",       json_array());
    json_object_set_new(msg, "stop_reason",   json_null());
    json_object_set_new(msg, "stop_sequence", json_null());

    json_t *usage = json_object();
    json_object_set_new(usage, "input_tokens",  json_integer(input_tokens));
    json_object_set_new(usage, "output_tokens", json_integer(0));
    json_object_set_new(msg, "usage", usage);

    json_t *frame = json_object();
    json_object_set_new(frame, "type",    json_lit("message_start"));
    json_object_set_new(frame, "message", msg);

    char *body = json_dumps(frame, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(frame);
    if (body) {
        char *s = NULL;
        if (asprintf(&s, "event: message_start\ndata: %s\n\n", body) > 0) {
            write_all(st->stream, s, strlen(s));
            free(s);
        }
        free(body);
    }

    const char *cbs =
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
    write_all(st->stream, cbs, strlen(cbs));
}

static void emit_anthropic_end(stream_state_t *st, int out_tokens)
{
    const char *cbe =
        "event: content_block_stop\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n";
    write_all(st->stream, cbe, strlen(cbe));

    json_t *delta = json_object();
    json_object_set_new(delta, "stop_reason",   json_lit("end_turn"));
    json_object_set_new(delta, "stop_sequence", json_null());

    json_t *usage = json_object();
    json_object_set_new(usage, "output_tokens", json_integer(out_tokens));

    json_t *frame = json_object();
    json_object_set_new(frame, "type",  json_lit("message_delta"));
    json_object_set_new(frame, "delta", delta);
    json_object_set_new(frame, "usage", usage);

    char *body = json_dumps(frame, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(frame);
    if (body) {
        char *s = NULL;
        if (asprintf(&s, "event: message_delta\ndata: %s\n\n", body) > 0) {
            write_all(st->stream, s, strlen(s));
            free(s);
        }
        free(body);
    }

    const char *stop =
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    write_all(st->stream, stop, strlen(stop));
}

/* --- worker thread ------------------------------------------------ */

static void *stream_worker(void *arg)
{
    stream_job_t *job = arg;
    sse_stream_t *s   = job->stream;

    char raw[64];
    char id_buf[80];
    make_uuid(raw, sizeof raw);
    snprintf(id_buf, sizeof id_buf,
             job->format == 0 ? "chatcmpl-%s" : "msg_%s", raw);

    stream_state_t st = {
        .stream      = s,
        .id          = id_buf,
        .model       = job->model[0] ? job->model : "xenolith",
        .first       = 1,
        .any         = 0,
        .token_count = 0,
    };

    fprintf(stderr,
        "[worker] fmt=%d id='%s' (len=%zu) model='%s' (len=%zu) job=%p\n",
        job->format,
        st.id,    strlen(st.id),
        st.model, strlen(st.model),
        (void *)job);

    if (job->format == 0) {
        emit_openai_header(&st);
    } else {
        emit_anthropic_start(&st, job->prompt_tokens);
    }

    if (job->format == 0) {
        fprintf(stderr, "[worker] calling emit_openai_header\n");
        emit_openai_header(&st);
        fprintf(stderr, "[worker] returned from emit_openai_header\n");
    } else {
        fprintf(stderr, "[worker] calling emit_anthropic_start\n");
        emit_anthropic_start(&st, job->prompt_tokens);
        fprintf(stderr, "[worker] returned from emit_anthropic_start\n");
    }

    int (*emit)(const char *, size_t, void *) =
        job->format == 0 ? stream_emit_openai : stream_emit_anthropic;

    int rc = run_generate(job->backend,
                          job->messages, job->n_messages,
                          job->max_tokens, job->temperature,
                          emit, &st);

    if (job->format == 0)
        emit_openai_footer(&st, rc == 0 ? "stop" : "stop");
    else
        emit_anthropic_end(&st, st.token_count);

    free_copied_messages(job->messages, job->n_messages);
    sse_stream_finish(s);
    free(job);
    return NULL;
}

/* --- MHD plumbing ------------------------------------------------- */

typedef struct {
    sse_stream_t *stream;
    pthread_t     worker;
    pthread_t     pinger;
    int           pinger_started;
} mhd_stream_ctx_t;

static ssize_t mhd_stream_reader(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    mhd_stream_ctx_t *ctx = cls;
    ssize_t n = sse_stream_read(ctx->stream, buf, max);
    if (n > 0)  return n;
    if (n == 0) return MHD_CONTENT_READER_END_OF_STREAM;
    return MHD_CONTENT_READER_END_WITH_ERROR;
}

static void mhd_stream_cleanup(void *cls)
{
    mhd_stream_ctx_t *ctx = cls;
    sse_stream_abort(ctx->stream);            /* signals both threads   */
    pthread_join(ctx->worker, NULL);          /* wait for generator     */
    if (ctx->pinger_started)
        pthread_join(ctx->pinger, NULL);      /* wait for keepalive     */
    sse_stream_destroy(ctx->stream);          /* now safe to free       */
    free(ctx);
}

enum MHD_Result run_streaming_completion(struct MHD_Connection *conn,
                                         const xenolith_message_t *messages,
                                         size_t                    n_messages,
                                         int                       max_tokens,
                                         float                     temperature,
                                         const char               *model,
                                         int                       format)
{
    sse_stream_t *s = sse_stream_create();
    if (!s)
        return send_json_error(conn, 500, "internal_error", "pipe failed");

    stream_job_t *job = calloc(1, sizeof *job);
    if (!job) {
        sse_stream_destroy(s);
        return send_json_error(conn, 500, "internal_error", "oom");
    }

    job->stream      = s;
    job->backend     = g_backend;
    job->format      = format;
    job->max_tokens  = max_tokens;
    job->temperature = temperature;
    job->messages    = copy_messages(messages, n_messages);
    job->n_messages  = n_messages;
    const char *src = model ? model : g_backend->default_model();
    strncpy(job->model, src, sizeof job->model - 1);
    job->model[sizeof job->model - 1] = 0;
    {
        char *flat = flatten_messages(messages, n_messages);
        job->prompt_tokens = estimate_tokens(flat);
        free(flat);
    }

    mhd_stream_ctx_t *ctx = calloc(1, sizeof *ctx);
    if (!ctx) {
        free_copied_messages(job->messages, job->n_messages);
        free(job);
        sse_stream_destroy(s);
        return send_json_error(conn, 500, "internal_error", "oom");
    }
    ctx->stream = s;

    if (pthread_create(&ctx->worker, NULL, stream_worker, job) != 0) {
        free_copied_messages(job->messages, job->n_messages);
        free(job);
        free(ctx);
        sse_stream_destroy(s);
        return send_json_error(conn, 500, "internal_error", "thread");
    }
    if (pthread_create(&ctx->pinger, NULL, ping_thread, s) == 0) {
        ctx->pinger_started = 1;
    }

    struct MHD_Response *resp = MHD_create_response_from_callback(
        MHD_SIZE_UNKNOWN, 4096, mhd_stream_reader, ctx, mhd_stream_cleanup);
    if (!resp) {
        sse_stream_abort(s);
        pthread_join(ctx->worker, NULL);
        sse_stream_destroy(s);
        free(ctx);
        return MHD_NO;
    }

    MHD_add_response_header(resp, "Content-Type", "text/event-stream; charset=utf-8");
    MHD_add_response_header(resp, "Cache-Control", "no-cache");
    MHD_add_response_header(resp, "Connection", "keep-alive");
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(resp, "X-Accel-Buffering", "no");

    enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* ------------------------------------------------------------------ */
/* routing                                                             */
/* ------------------------------------------------------------------ */

static enum MHD_Result route(struct MHD_Connection *conn, struct request *req)
{
    const char *m = req->method;
    const char *u = req->url;

    if (strcmp(m, "OPTIONS") == 0) {
        struct MHD_Response *r =
            MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(r, "Access-Control-Allow-Origin", "*");
        MHD_add_response_header(r, "Access-Control-Allow-Methods", "POST, GET, OPTIONS");
        MHD_add_response_header(r, "Access-Control-Allow-Headers", "*");
        enum MHD_Result ret = MHD_queue_response(conn, 204, r);
        MHD_destroy_response(r);
        return ret;
    }

    if (strcmp(m, "GET") == 0 && strcmp(u, "/health") == 0) {
        struct MHD_Response *r =
            MHD_create_response_from_buffer(2, (void *)"ok", MHD_RESPMEM_PERSISTENT);
        enum MHD_Result ret = MHD_queue_response(conn, 200, r);
        MHD_destroy_response(r);
        return ret;
    }
    if (strcmp(m, "GET") == 0 && strcmp(u, "/v1/models") == 0)
        return openai_list_models(conn);
    if (strcmp(m, "POST") == 0 && strcmp(u, "/v1/chat/completions") == 0)
        return openai_chat_completions(conn, req);
    if (strcmp(m, "POST") == 0 && strcmp(u, "/v1/messages") == 0)
        return anthropic_messages(conn, req);

    return send_json_error(conn, 404, "not_found", "unknown route");
}

static enum MHD_Result access_handler(void *cls,
                                      struct MHD_Connection *conn,
                                      const char *url,
                                      const char *method,
                                      const char *version,
                                      const char *upload_data,
                                      size_t *upload_data_size,
                                      void **con_cls)
{
    (void)cls; (void)version;
    struct request *req = *con_cls;

    if (req == NULL) {
        req = calloc(1, sizeof *req);
        req->method = method;
        req->url    = url;
        *con_cls    = req;
        return MHD_YES;
    }

    if (*upload_data_size > 0) {
        size_t need = req->body_len + *upload_data_size + 1;
        if (need > req->body_cap) {
            size_t nc = req->body_cap ? req->body_cap * 2 : 4096;
            while (nc < need) nc *= 2;
            req->body = realloc(req->body, nc);
            req->body_cap = nc;
        }
        memcpy(req->body + req->body_len, upload_data, *upload_data_size);
        req->body_len += *upload_data_size;
        req->body[req->body_len] = 0;
        *upload_data_size = 0;
        return MHD_YES;
    }

    enum MHD_Result ret = route(conn, req);
    free(req->body);
    free(req);
    *con_cls = NULL;
    return ret;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

int xenolith_server_start(const xenolith_server_config_t *cfg,
                          const xenolith_backend_t       *backend)
{
    if (!backend) {
        fprintf(stderr, "[xenolith-server] backend is NULL\n");
        return -1;
    }
    if (!backend->generate && !backend->generate_chat) {
        fprintf(stderr, "[xenolith-server] backend has neither "
                        "generate nor generate_chat\n");
        return -1;
    }
    g_backend = backend;
    g_verbose = cfg && cfg->verbose;

    signal(SIGPIPE, SIG_IGN);

    const char *host    = (cfg && cfg->host)    ? cfg->host    : "127.0.0.1";
    int         port    = (cfg && cfg->port)    ? cfg->port    : 8080;
    int         threads = (cfg && cfg->threads) ? cfg->threads : 4;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (strcmp(host, "0.0.0.0") == 0 || strcmp(host, "*") == 0)
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "localhost") == 0)
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* fallback */

    unsigned int flags = MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG;

    g_daemon = MHD_start_daemon(flags,
                                (uint16_t)port,
                                NULL, NULL,
                                &access_handler, NULL,
                                MHD_OPTION_THREAD_POOL_SIZE, (unsigned)threads,
                                MHD_OPTION_SOCK_ADDR, (struct sockaddr *)&addr,
                                MHD_OPTION_END);
    if (!g_daemon) {
        fprintf(stderr, "[xenolith-server] MHD_start_daemon failed "
                        "on %s:%d\n", host, port);
        return -1;
    }

    if (g_verbose)
        fprintf(stderr, "[xenolith-server] listening on %s:%d\n", host, port);
    return 0;
}

void xenolith_server_stop(void)
{
    if (g_daemon) {
        MHD_stop_daemon(g_daemon);
        g_daemon = NULL;
    }
}