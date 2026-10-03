#ifndef XENOLITH_INTERNAL_H
#define XENOLITH_INTERNAL_H

#include <microhttpd.h>
#include <jansson.h>
#include <stddef.h>
#include <stdint.h>

#include "xenolith_server.h"

extern const xenolith_backend_t *g_backend;
extern int                       g_verbose;

struct request {
    const char *method;
    const char *url;
    char       *body;
    size_t      body_len;
    size_t      body_cap;
};

enum MHD_Result send_json_error(struct MHD_Connection *conn,
                                unsigned int status,
                                const char *type,
                                const char *message);

enum MHD_Result send_json(struct MHD_Connection *conn,
                          unsigned int status,
                          json_t *root /* steals ref */);

/* Extract role/content pairs from an OpenAI or Anthropic "messages"
 * array. Both role and content are strdup'd. Caller frees with
 * free_messages(). */
xenolith_message_t *extract_messages(json_t *messages,
                                     const char *system,
                                     size_t *count_out);
void free_messages(xenolith_message_t *m, size_t n);

/* Legacy flat-prompt fallback for backends without generate_chat. */
char *flatten_messages(const xenolith_message_t *m, size_t n);

/* Route handlers. */
enum MHD_Result openai_chat_completions(struct MHD_Connection *conn,
                                        struct request *req);
enum MHD_Result openai_list_models(struct MHD_Connection *conn);
enum MHD_Result anthropic_messages(struct MHD_Connection *conn,
                                   struct request *req);

int estimate_tokens(const char *text);

/* Shared streaming / buffered completion. */
enum MHD_Result run_streaming_completion(struct MHD_Connection *conn,
                                         const xenolith_message_t *messages,
                                         size_t n_messages,
                                         int max_tokens,
                                         float temperature,
                                         const char *model,
                                         int format);

int run_buffered_completion(const xenolith_message_t *messages,
                            size_t n_messages,
                            int max_tokens,
                            float temperature,
                            char **out_text,
                            int *out_prompt_tokens,
                            int *out_completion_tokens);

void make_uuid(char *out, size_t n);

#endif /* XENOLITH_INTERNAL_H */