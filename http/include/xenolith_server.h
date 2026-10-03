#ifndef XENOLITH_SERVER_H
#define XENOLITH_SERVER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called for each generated token (UTF-8, may be multi-byte).
 * Return non-zero to abort generation. */
typedef int (*xenolith_token_cb)(const char *token, size_t len, void *userdata);

int xenolith_backend_run_owner_loop(void);

/* A single chat message in role/content form. */
typedef struct {
    const char *role;     /* "system" | "user" | "assistant" | "tool" */
    const char *content;
} xenolith_message_t;

/* Backend bridge implemented by the Xenolith core or a mock. */
typedef struct {
    /* Flat-prompt path (mock, or models with no chat template). */
    int (*generate)(const char *prompt,
                    int max_tokens,
                    float temperature,
                    xenolith_token_cb token_cb,
                    void *userdata);

    /* Structured chat path. If non-NULL the server prefers it.
     * Only used once you wire the real engine in. */
    int (*generate_chat)(const xenolith_message_t *messages,
                         size_t n_messages,
                         int max_tokens,
                         float temperature,
                         xenolith_token_cb token_cb,
                         void *userdata);

    int (*list_models)(char ***names, size_t *count);
    const char *(*default_model)(void);

    void *priv;
} xenolith_backend_t;

typedef struct {
    const char *host;    /* default "127.0.0.1" */
    int         port;    /* default 8080        */
    int         threads; /* default 4           */
    int         verbose; /* default 0           */
} xenolith_server_config_t;

int  xenolith_server_start(const xenolith_server_config_t *cfg,
                           const xenolith_backend_t       *backend);
void xenolith_server_stop(void);

#ifdef __cplusplus
}
#endif
#endif /* XENOLITH_SERVER_H */