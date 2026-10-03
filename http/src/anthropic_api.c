#define _GNU_SOURCE
#include "internal.h"

#include <stdlib.h>
#include <string.h>

enum MHD_Result anthropic_messages(struct MHD_Connection *conn,
                                   struct request *req)
{
    json_error_t err;
    json_t *root = json_loadb(req->body ? req->body : "",
                              req->body_len, 0, &err);
    if (!root)
        return send_json_error(conn, 400, "invalid_request_error", err.text);

    /* --- copy every pointer we need out of the tree BEFORE decref --- */
    char model_buf[256];   model_buf[0]   = 0;
    char system_buf[4096]; system_buf[0]  = 0;
    {
        const char *m = json_string_value(json_object_get(root, "model"));
        if (m) {
            strncpy(model_buf, m, sizeof model_buf - 1);
            model_buf[sizeof model_buf - 1] = 0;
        }
    }
    {
        const char *s = json_string_value(json_object_get(root, "system"));
        if (s) {
            strncpy(system_buf, s, sizeof system_buf - 1);
            system_buf[sizeof system_buf - 1] = 0;
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
    xenolith_message_t *msgs = extract_messages(
        messages_j,
        system_buf[0] ? system_buf : NULL,
        &n);
    if (!msgs || n == 0) {
        free_messages(msgs, n);
        json_decref(root);
        return send_json_error(conn, 400, "invalid_request_error",
                               "no usable messages");
    }
    json_decref(root);

    const char *model = model_buf[0] ? model_buf : g_backend->default_model();

    /* ---- streaming ------------------------------------------------ */
    if (stream) {
        enum MHD_Result r = run_streaming_completion(
            conn, msgs, n, max_tokens, temperature, model, 1);
        free_messages(msgs, n);
        return r;
    }

    /* ---- buffered ------------------------------------------------- */
    char *text = NULL;
    int   pt = 0, ct = 0;
    if (run_buffered_completion(msgs, n, max_tokens, temperature,
                                &text, &pt, &ct) != 0) {
        free_messages(msgs, n);
        return send_json_error(conn, 500, "api_error", "generation failed");
    }
    free_messages(msgs, n);

    char id[80];
    make_uuid(id, sizeof id);

    json_t *content_item = json_object();
    json_object_set_new(content_item, "type", json_lit("text"));
    json_object_set_new(content_item, "text", json_str(text));

    json_t *content = json_array();
    json_array_append_new(content, content_item);

    json_t *usage = json_object();
    json_object_set_new(usage, "input_tokens",  json_integer(pt));
    json_object_set_new(usage, "output_tokens", json_integer(ct));

    json_t *resp = json_object();
    json_object_set_new(resp, "id",            json_str(id));
    json_object_set_new(resp, "type",          json_lit("message"));
    json_object_set_new(resp, "role",          json_lit("assistant"));
    json_object_set_new(resp, "model",         json_str(model));
    json_object_set_new(resp, "content",       content);
    json_object_set_new(resp, "stop_reason",   json_lit("end_turn"));
    json_object_set_new(resp, "stop_sequence", json_null());
    json_object_set_new(resp, "usage",         usage);

    free(text);
    return send_json(conn, 200, resp);
}