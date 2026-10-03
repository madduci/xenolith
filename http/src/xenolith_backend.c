/* src/xenolith_backend.c
 *
 * Adapter between the xenolith-server HTTP module and the Xenolith
 * inference engine. Link this into the Xenolith binary, not the other
 * way around: Xenolith does not build a library.
 */
#ifndef XE_WITH_HTTP
#error "xenolith_backend.c requires -DXE_WITH_HTTP=1"
#endif
#define _GNU_SOURCE
#include "xenolith_server.h"
#include "xenolith.h"
#include "profile.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- engine singleton --------------------------------------------- */

static xe_engine *g_engine  = NULL;
static profile   *g_profile = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

int xenolith_backend_init(xenolith_backend_t *out, xe_engine *engine);

/* --- helpers ------------------------------------------------------ */

static void push_render(xe_tokens *t, const profile_render *r)
{
    for (uint32_t i = 0; i < r->token_count; i++)
        xe_tokens_push(t, r->tokens[i]);
}

/* Find the first system message, if any. */
static const char *find_system(const xenolith_message_t *msgs, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(msgs[i].role, "system"))
            return msgs[i].content;
    return NULL;
}

/* --- the actual generation --------------------------------------- */


typedef struct {
    const xenolith_message_t *messages;
    size_t                    n_messages;
    int                       max_tokens;
    float                     temperature;
    xenolith_token_cb         cb;
    void                     *ud;

    int                       rc;
    int                       done;
    pthread_mutex_t           lock;
    pthread_cond_t            cond;
} engine_job_t;

static engine_job_t    *g_pending = NULL;
static pthread_mutex_t  g_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_queue_cond = PTHREAD_COND_INITIALIZER;

/* Forward declaration of the real implementation that must run on the
 * engine-owner thread. */
static int xe_generate_chat_impl(const xenolith_message_t *messages,
                                 size_t n_messages,
                                 int max_tokens, float temperature,
                                 xenolith_token_cb cb, void *ud);


static int xe_generate_chat_impl(const xenolith_message_t *messages, size_t n_messages,
                            int max_tokens, float temperature,
                            xenolith_token_cb cb, void *ud)
{
    pthread_mutex_lock(&g_lock);

    int ctx = xe_context_size(g_engine);
    if (max_tokens <= 0 || max_tokens > ctx - 8)
        max_tokens = ctx - 8;

    xe_tokens transcript = {0};
    profile_render render;

    /* 1. system prompt */
    if (profile_render_system(g_profile, find_system(messages, n_messages),
                              NULL, 0, 0, &render) != PROFILE_OK)
        goto fail;
    push_render(&transcript, &render);

    /* 2. replay user / assistant turns.
     *    The turn machine mirrors cmd_chat(): the first user turn is
     *    PADDED, everything after a completed turn is BARE. Assistant
     *    turns are rendered as completed (turn_complete = 1). */
    uint32_t turn = PROFILE_TURN_PADDED;
    for (size_t i = 0; i < n_messages; i++) {
        const char *role = messages[i].role;
        if (!strcmp(role, "system")) continue;

        if (!strcmp(role, "user")) {
            if (profile_render_user(g_profile, messages[i].content,
                                    turn, &render) != PROFILE_OK)
                goto fail;
            push_render(&transcript, &render);
            turn = PROFILE_TURN_BARE;
        } else if (!strcmp(role, "assistant")) {
            if (profile_render_assistant(g_profile, NULL, 1,
                                         messages[i].content, NULL, 0,
                                         PROFILE_TURN_OPEN, 1,
                                         &render) != PROFILE_OK)
                goto fail;
            push_render(&transcript, &render);
        }
    }

    /* 3. open the assistant reply */
    if (profile_render_reply_open(g_profile, PROFILE_TURN_PADDED,
                                  0, 0, &render) != PROFILE_OK)
        goto fail;
    push_render(&transcript, &render);

    if (transcript.len >= ctx - 1) {
        /* Context overflow: keep the tail. Not ideal, but it lets the
         * request complete instead of failing. */
        int keep = ctx - 2;
        memmove(transcript.v, transcript.v + (transcript.len - keep),
                (size_t)keep * sizeof(*transcript.v));
        transcript.len = keep;
    }

    /* 4. generate */
    {
        xe_session *s = xe_session_new(g_engine);
        xe_sampler sp = { temperature, 64, 0.95f, (uint64_t)time(NULL) };
        int32_t eos = xe_eos_id(g_engine);
        int32_t eot = xe_eot_id(g_engine);

        for (int i = 0; i < max_tokens && transcript.len < ctx - 1; i++) {
            xe_session_sync(s, &transcript);
            int32_t t = xe_session_next(s, &sp);
            if (t == eos || t == eot) break;

            char buf[256];
            int nb = xe_detokenize(g_engine, t, buf, sizeof buf);
            if (nb > 0 && cb(buf, (size_t)nb, ud)) {
                /* client aborted mid-stream */
                xe_tokens_push(&transcript, t);
                break;
            }
            xe_tokens_push(&transcript, t);
        }
        xe_session_free(s);
    }

    xe_tokens_free(&transcript);
    pthread_mutex_unlock(&g_lock);
    return 0;

fail:
    xe_tokens_free(&transcript);
    pthread_mutex_unlock(&g_lock);
    return -1;
}

/* Called by HTTP worker threads. Blocks until the owner thread has run
 * the job. */
static int xe_generate_chat(const xenolith_message_t *messages, size_t n,
                            int max_tokens, float temperature,
                            xenolith_token_cb cb, void *ud)
{
    engine_job_t job = {
        .messages    = messages,
        .n_messages  = n,
        .max_tokens  = max_tokens,
        .temperature = temperature,
        .cb          = cb,
        .ud          = ud,
    };
    pthread_mutex_init(&job.lock, NULL);
    pthread_cond_init(&job.cond, NULL);

    /* Hand off to the owner thread. */
    pthread_mutex_lock(&g_queue_lock);
    while (g_pending != NULL)
        pthread_cond_wait(&g_queue_cond, &g_queue_lock);
    g_pending = &job;
    pthread_cond_broadcast(&g_queue_cond);
    pthread_mutex_unlock(&g_queue_lock);

    /* Wait for completion. */
    pthread_mutex_lock(&job.lock);
    while (!job.done)
        pthread_cond_wait(&job.cond, &job.lock);
    pthread_mutex_unlock(&job.lock);

    pthread_mutex_destroy(&job.lock);
    pthread_cond_destroy(&job.cond);
    return job.rc;
}

/* Public entry point. The engine-owner thread calls this and does not
 * return until the process exits. */
int xenolith_backend_run_owner_loop(void)
{
    for (;;) {
        pthread_mutex_lock(&g_queue_lock);
        while (g_pending == NULL)
            pthread_cond_wait(&g_queue_cond, &g_queue_lock);
        engine_job_t *job = g_pending;
        pthread_mutex_unlock(&g_queue_lock);

        job->rc = xe_generate_chat_impl(job->messages, job->n_messages,
                                        job->max_tokens, job->temperature,
                                        job->cb, job->ud);

        pthread_mutex_lock(&job->lock);
        job->done = 1;
        pthread_cond_broadcast(&job->cond);
        pthread_mutex_unlock(&job->lock);

        pthread_mutex_lock(&g_queue_lock);
        g_pending = NULL;
        pthread_cond_broadcast(&g_queue_cond);
        pthread_mutex_unlock(&g_queue_lock);
    }
    return 0;
}

/* --- model listing ------------------------------------------------ */

static int xe_list_models(char ***names, size_t *count)
{
    const char *m = g_profile ? profile_model(g_profile) : "xenolith";
    *names = malloc(sizeof(char *));
    (*names)[0] = strdup(m);
    *count = 1;
    return 0;
}

static const char *xe_default_model(void)
{
    return g_profile ? profile_model(g_profile) : "xenolith";
}

/* --- public init -------------------------------------------------- */

int xenolith_backend_init(xenolith_backend_t *out, xe_engine *engine)
{
    if (!out || !engine) return -1;
    if (!g_engine) {
        g_engine = engine;
        if (profile_open(&g_profile, engine) != PROFILE_OK)
            return -1;
    }
    *out = (xenolith_backend_t){
        .generate      = NULL,
        .generate_chat = xe_generate_chat,
        .list_models   = xe_list_models,
        .default_model = xe_default_model,
        .priv          = NULL,
    };
    return 0;
}