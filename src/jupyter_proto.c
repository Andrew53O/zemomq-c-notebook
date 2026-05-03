#include "jupyter_proto.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zmq.h>

static void append_n(char **buf, size_t *len, size_t *cap, const char *text, size_t n) {
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap) {
            *cap = *cap ? *cap * 2 : 256;
        }
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, text, n);
    *len += n;
    (*buf)[*len] = '\0';
}

static void append_s(char **buf, size_t *len, size_t *cap, const char *text) {
    append_n(buf, len, cap, text, strlen(text));
}

void jp_msg_init(struct jp_message *msg) {
    memset(msg, 0, sizeof(*msg));
}

void jp_msg_free(struct jp_message *msg) {
    for (int i = 0; i < msg->ident_count; i++) {
        free(msg->idents[i]);
    }
    free(msg->header);
    free(msg->parent_header);
    free(msg->metadata);
    free(msg->content);
    jp_msg_init(msg);
}

void jp_make_msg_id(char *out, size_t out_size) {
    static unsigned long counter = 0;
    snprintf(out, out_size, "%ld-%ld-%lu", (long)getpid(), (long)time(NULL), ++counter);
}

char *jp_json_string(const char *text) {
    size_t len = 0, cap = 0;
    char *buf = NULL;
    jp_json_escape_append(&buf, &len, &cap, text ? text : "");
    return buf;
}

void jp_json_escape_append(char **buf, size_t *len, size_t *cap, const char *text) {
    append_s(buf, len, cap, "\"");
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        char esc[16];
        switch (*p) {
            case '\\': append_s(buf, len, cap, "\\\\"); break;
            case '"': append_s(buf, len, cap, "\\\""); break;
            case '\n': append_s(buf, len, cap, "\\n"); break;
            case '\r': append_s(buf, len, cap, "\\r"); break;
            case '\t': append_s(buf, len, cap, "\\t"); break;
            default:
                if (*p < 32) {
                    snprintf(esc, sizeof(esc), "\\u%04x", *p);
                    append_s(buf, len, cap, esc);
                } else {
                    append_n(buf, len, cap, (const char *)p, 1);
                }
        }
    }
    append_s(buf, len, cap, "\"");
}

static char *json_value_start(const char *json, const char *key) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    char *p = strstr((char *)json, pattern);
    if (!p) return NULL;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

char *jp_json_get_string(const char *json, const char *key) {
    char *p = json_value_start(json, key);
    if (!p || *p != '"') return NULL;
    p++;

    size_t len = 0, cap = 0;
    char *buf = NULL;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': append_s(&buf, &len, &cap, "\n"); break;
                case 'r': append_s(&buf, &len, &cap, "\r"); break;
                case 't': append_s(&buf, &len, &cap, "\t"); break;
                case '"': append_s(&buf, &len, &cap, "\""); break;
                case '\\': append_s(&buf, &len, &cap, "\\"); break;
                default:
                    if (*p) append_n(&buf, &len, &cap, p, 1);
                    break;
            }
            if (*p) p++;
        } else {
            append_n(&buf, &len, &cap, p, 1);
            p++;
        }
    }
    if (!buf) {
        buf = strdup("");
    }
    return buf;
}

int jp_json_get_int(const char *json, const char *key, int fallback) {
    char *p = json_value_start(json, key);
    return p ? (int)strtol(p, NULL, 10) : fallback;
}

int jp_json_get_bool(const char *json, const char *key, int fallback) {
    char *p = json_value_start(json, key);
    if (!p) return fallback;
    if (strncmp(p, "true", 4) == 0) return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    return fallback;
}

char *jp_make_header(const char *session, const char *msg_type) {
    char msg_id[64];
    char date[64];
    time_t now = time(NULL);
    struct tm tm_now;
    gmtime_r(&now, &tm_now);
    strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%SZ", &tm_now);
    jp_make_msg_id(msg_id, sizeof(msg_id));

    size_t len = 0, cap = 0;
    char *buf = NULL;
    append_s(&buf, &len, &cap, "{\"msg_id\":");
    jp_json_escape_append(&buf, &len, &cap, msg_id);
    append_s(&buf, &len, &cap, ",\"session\":");
    jp_json_escape_append(&buf, &len, &cap, session);
    append_s(&buf, &len, &cap, ",\"username\":\"user\",\"date\":");
    jp_json_escape_append(&buf, &len, &cap, date);
    append_s(&buf, &len, &cap, ",\"msg_type\":");
    jp_json_escape_append(&buf, &len, &cap, msg_type);
    append_s(&buf, &len, &cap, ",\"version\":\"5.5\"}");
    return buf;
}

static int send_frame(void *socket, const void *data, size_t len, int flags) {
    zmq_msg_t msg;
    if (zmq_msg_init_size(&msg, len) == -1) return -1;
    if (len) memcpy(zmq_msg_data(&msg), data, len);
    int rc = zmq_msg_send(&msg, socket, flags);
    zmq_msg_close(&msg);
    return rc;
}

int jp_send_message(void *socket, const struct jp_message *reply_to, const char *session,
                    const char *msg_type, const char *metadata, const char *content) {
    char *header = jp_make_header(session, msg_type);
    const char *parent = reply_to && reply_to->header ? reply_to->header : "{}";
    const char *meta = metadata ? metadata : "{}";
    const char *body = content ? content : "{}";

    if (reply_to) {
        for (int i = 0; i < reply_to->ident_count; i++) {
            if (send_frame(socket, reply_to->idents[i], reply_to->ident_lens[i], ZMQ_SNDMORE) == -1) {
                free(header);
                return -1;
            }
        }
    }

    int rc =
        send_frame(socket, JP_DELIM, strlen(JP_DELIM), ZMQ_SNDMORE) == -1 ||
        send_frame(socket, "", 0, ZMQ_SNDMORE) == -1 ||
        send_frame(socket, header, strlen(header), ZMQ_SNDMORE) == -1 ||
        send_frame(socket, parent, strlen(parent), ZMQ_SNDMORE) == -1 ||
        send_frame(socket, meta, strlen(meta), ZMQ_SNDMORE) == -1 ||
        send_frame(socket, body, strlen(body), 0) == -1 ? -1 : 0;
    free(header);
    return rc;
}

int jp_send_iopub(void *socket, const char *topic, const struct jp_message *parent,
                  const char *session, const char *msg_type, const char *metadata,
                  const char *content) {
    if (send_frame(socket, topic, strlen(topic), ZMQ_SNDMORE) == -1) {
        return -1;
    }
    return jp_send_message(socket, parent, session, msg_type, metadata, content);
}

int jp_recv_message(void *socket, struct jp_message *msg, int flags) {
    jp_msg_init(msg);
    int seen_delim = 0;
    int part = 0;

    while (1) {
        zmq_msg_t frame;
        if (zmq_msg_init(&frame) == -1) return -1;
        int n = zmq_msg_recv(&frame, socket, flags);
        if (n == -1) {
            zmq_msg_close(&frame);
            return -1;
        }
        size_t len = zmq_msg_size(&frame);
        char *data = calloc(len + 1, 1);
        if (!data) {
            zmq_msg_close(&frame);
            jp_msg_free(msg);
            return -1;
        }
        memcpy(data, zmq_msg_data(&frame), len);
        data[len] = '\0';

        int more = 0;
        size_t more_size = sizeof(more);
        zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &more_size);
        zmq_msg_close(&frame);

        if (!seen_delim) {
            if (strcmp(data, JP_DELIM) == 0) {
                seen_delim = 1;
                free(data);
            } else if (msg->ident_count < JP_MAX_IDENTITIES) {
                msg->idents[msg->ident_count] = data;
                msg->ident_lens[msg->ident_count] = len;
                msg->ident_count++;
            } else {
                free(data);
            }
        } else {
            if (part == 0) {
                free(data);
            } else if (part == 1) {
                msg->header = data;
            } else if (part == 2) {
                msg->parent_header = data;
            } else if (part == 3) {
                msg->metadata = data;
            } else if (part == 4) {
                msg->content = data;
            } else {
                free(data);
            }
            part++;
        }

        if (!more) break;
    }

    if (!msg->header) msg->header = strdup("{}");
    if (!msg->parent_header) msg->parent_header = strdup("{}");
    if (!msg->metadata) msg->metadata = strdup("{}");
    if (!msg->content) msg->content = strdup("{}");

    char *msg_id = jp_json_get_string(msg->header, "msg_id");
    char *msg_type = jp_json_get_string(msg->header, "msg_type");
    snprintf(msg->msg_id, sizeof(msg->msg_id), "%s", msg_id ? msg_id : "");
    snprintf(msg->msg_type, sizeof(msg->msg_type), "%s", msg_type ? msg_type : "");
    free(msg_id);
    free(msg_type);
    return 0;
}

int jp_write_connection_file(const char *path) {
    FILE *file = fopen(path, "wb");
    if (!file) return -1;
    int rc = fprintf(file,
        "{\n"
        "  \"shell_port\": %d,\n"
        "  \"iopub_port\": %d,\n"
        "  \"stdin_port\": %d,\n"
        "  \"control_port\": %d,\n"
        "  \"hb_port\": %d,\n"
        "  \"ip\": \"127.0.0.1\",\n"
        "  \"transport\": \"tcp\",\n"
        "  \"key\": \"\",\n"
        "  \"signature_scheme\": \"hmac-sha256\",\n"
        "  \"kernel_name\": \"zmqbook-c\"\n"
        "}\n",
        JP_SHELL_PORT, JP_IOPUB_PORT, JP_STDIN_PORT, JP_CONTROL_PORT, JP_HB_PORT);
    fclose(file);
    return rc < 0 ? -1 : 0;
}
