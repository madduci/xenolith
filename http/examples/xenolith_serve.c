/* examples/xenolith_serve.c
 *
 * Standalone demo of the xenolith-server module with a mock backend.
 * Builds and runs without the Xenolith engine so you can validate the
 * HTTP/SSE layer end-to-end.
 */
#include "xenolith_server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* --- mock backend ------------------------------------------------- */

static int mock_generate_chat(const xenolith_message_t *msgs, size_t n,
                              int max_tokens, float temperature,
                              xenolith_token_cb cb, void *ud)
{
    (void)temperature;
    if (n == 0) return -1;

    const char *last_user = NULL;
    for (size_t i = n; i-- > 0; ) {
        if (!strcmp(msgs[i].role, "user")) {
            last_user = msgs[i].content;
            break;
        }
    }
    if (!last_user) last_user = "(no user message)";

    char echo[512];
    int elen = snprintf(echo, sizeof echo, "You said: \"%.*s\". ",
                        (int)(strlen(last_user) > 200 ? 200 : strlen(last_user)),
                        last_user);
    if (elen > 0 && cb(echo, (size_t)elen, ud)) return 1;

    static const char *toks[] = {
        "This ", "is ", "the ", "mock ", "backend. ",
        "Wire ", "the ", "real ", "engine ", "to ", "get ", "real ", "output."
    };
    size_t nt = sizeof toks / sizeof toks[0];
    for (int i = 0; i < max_tokens && i < (int)nt; i++) {
        if (cb(toks[i], strlen(toks[i]), ud)) return 1;
        usleep(50000); /* simulate per-token latency */
    }
    return 0;
}

static int mock_list_models(char ***names, size_t *count)
{
    static const char *list[] = { "gemma-4-26b", "qwen3-8b", "llama-3.1-8b" };
    size_t n = sizeof list / sizeof list[0];
    *names = malloc(n * sizeof **names);
    if (!*names) return -1;
    for (size_t i = 0; i < n; i++) (*names)[i] = strdup(list[i]);
    *count = n;
    return 0;
}

static const char *mock_default_model(void) { return "gemma-4-26b"; }

/* --- CLI ---------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [--host HOST] [--port PORT] [--threads N]\n"
        "  defaults: host=127.0.0.1  port=8080  threads=4\n", prog);
}

int main(int argc, char **argv)
{
    const char *host    = "127.0.0.1";
    int         port    = 8080;
    int         threads = 4;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--host")    && i + 1 < argc) host    = argv[++i];
        else if (!strcmp(argv[i], "--port")    && i + 1 < argc) port    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]); return 0;
        } else {
            usage(argv[0]); return 2;
        }
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    static const xenolith_backend_t backend = {
        .generate      = NULL,                /* mock uses chat path */
        .generate_chat = mock_generate_chat,
        .list_models   = mock_list_models,
        .default_model = mock_default_model,
        .priv          = NULL,
    };

    xenolith_server_config_t cfg = {
        .host = host, .port = port, .threads = threads, .verbose = 1,
    };
    if (xenolith_server_start(&cfg, &backend) != 0) {
        fprintf(stderr, "failed to start server on %s:%d\n", host, port);
        return 1;
    }

    fprintf(stderr, "[serve] ready on http://%s:%d (Ctrl+C to stop)\n",
            host, port);
    while (!g_stop) pause();

    fprintf(stderr, "[serve] shutting down\n");
    xenolith_server_stop();
    return 0;
}