#define _GNU_SOURCE
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

enum MHD_Result openai_chat_completions(struct MHD_Connection *conn,
                                        struct request *req)
{
    json_error_t err;
    json_t *root = json_loadb(req->body ? req->body : "",
                              req->body_len, 0, &err);
    if (!root)
        return send_json_error(conn, 400, "invalid_request_error", err.text);

    char model_buf[256]; model_buf[0] = 0;
    {
        const char *m = json_string_value(json_object_get(root, "model"));
        if (m) {
            strncpy(model_buf, m, sizeof model_buf - 1);
            model_buf[sizeof model_buf - 1] = 0;
        }
    }

    int max_tokens    = (int)json_integer_value(json_object_get(root, "max_tokens"));
    json_t *t         = json_object_get(root, "temperature");
    float temperature = json_is_number(t) ? (float)json_number_value(t) : 0.7f;
    int stream        = json_is_true(json_object_get(root, "stream"));
    if (max_tokens <= 0) max_tokens = 512;

    json_t *messages_j = json_object_get(root, "messages");
    if (!json_is_array(messages_j)) {
        json_decref(root);
        return send_json_error(conn, 400, "invalid_request_error",
                               "messages must be an array");
    }

    size_t n = 0;
    xenolith_message_t *msgs = extract_messages(messages_j, NULL, &n);
    if (!msgs || n == 0) {
        free_messages(msgs, n);
        json_decref(root);
        return send_json_error(conn, 400, "invalid_request_error",
                               "no usable messages");
    }
    json_decref(root);

    const char *model = model_buf[0] ? model_buf : g_backend->default_model();

    if (stream) {
        enum MHD_Result r = run_streaming_completion(
            conn, msgs, n, max_tokens, temperature, model, 0);
        free_messages(msgs, n);
        return r;
    }

    char *text = NULL;
    int   pt = 0, ct = 0;
    if (run_buffered_completion(msgs, n, max_tokens, temperature,
                                &text, &pt, &ct) != 0) {
        free_messages(msgs, n);
        return send_json_error(conn, 500, "internal_error", "generation failed");
    }
    free_messages(msgs, n);

    char id[80];
    make_uuid(id, sizeof id);

    json_t *message = json_object();
    json_object_set_new(message, "role",    json_lit("assistant"));
    json_object_set_new(message, "content", json_str(text));

    json_t *choice = json_object();
    json_object_set_new(choice, "index",         json_integer(0));
    json_object_set_new(choice, "message",       message);
    json_object_set_new(choice, "finish_reason", json_lit("stop"));

    json_t *choices = json_array();
    json_array_append_new(choices, choice);

    json_t *usage = json_object();
    json_object_set_new(usage, "prompt_tokens",     json_integer(pt));
    json_object_set_new(usage, "completion_tokens", json_integer(ct));
    json_object_set_new(usage, "total_tokens",      json_integer(pt + ct));

    json_t *resp = json_object();
    json_object_set_new(resp, "id",      json_str(id));
    json_object_set_new(resp, "object",  json_lit("chat.completion"));
    json_object_set_new(resp, "created", json_integer((json_int_t)time(NULL)));
    json_object_set_new(resp, "model",   json_str(model));
    json_object_set_new(resp, "choices", choices);
    json_object_set_new(resp, "usage",   usage);

    free(text);
    return send_json(conn, 200, resp);
}

enum MHD_Result openai_list_models(struct MHD_Connection *conn)
{
    char **names = NULL;
    size_t count = 0;

    if (g_backend->list_models)
        g_backend->list_models(&names, &count);

    if (count == 0) {
        const char *d = g_backend->default_model
                      ? g_backend->default_model() : "xenolith";
        names = malloc(sizeof *names);
        names[0] = strdup(d);
        count = 1;
    }

    json_t *arr = json_array();
    for (size_t i = 0; i < count; i++) {
        json_t *m = json_object();
        json_object_set_new(m, "id",       json_str(names[i]));
        json_object_set_new(m, "object",   json_lit("model"));
        json_object_set_new(m, "created",  json_integer((json_int_t)time(NULL)));
        json_object_set_new(m, "owned_by", json_lit("xenolith"));
        json_array_append_new(arr, m);
        free(names[i]);
    }
    free(names);

    json_t *resp = json_object();
    json_object_set_new(resp, "object", json_lit("list"));
    json_object_set_new(resp, "data",   arr);
    return send_json(conn, 200, resp);
}