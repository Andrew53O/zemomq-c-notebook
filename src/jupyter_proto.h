#ifndef JUPYTER_PROTO_H
#define JUPYTER_PROTO_H

#include <stddef.h>

#define JP_MAX_IDENTITIES 8
#define JP_DELIM "<IDS|MSG>"
#define JP_SHELL_PORT 7010
#define JP_IOPUB_PORT 7011
#define JP_STDIN_PORT 7012
#define JP_CONTROL_PORT 7013
#define JP_HB_PORT 7014

struct jp_message {
    char *idents[JP_MAX_IDENTITIES];
    size_t ident_lens[JP_MAX_IDENTITIES];
    int ident_count;
    char *header;
    char *parent_header;
    char *metadata;
    char *content;
    char msg_id[64];
    char msg_type[64];
};

void jp_msg_init(struct jp_message *msg);
void jp_msg_free(struct jp_message *msg);

void jp_make_msg_id(char *out, size_t out_size);
char *jp_make_header(const char *session, const char *msg_type);
int jp_send_message(void *socket, const struct jp_message *reply_to, const char *session,
                    const char *msg_type, const char *metadata, const char *content);
int jp_send_iopub(void *socket, const char *topic, const struct jp_message *parent,
                  const char *session, const char *msg_type, const char *metadata,
                  const char *content);
int jp_recv_message(void *socket, struct jp_message *msg, int flags);

char *jp_json_get_string(const char *json, const char *key);
int jp_json_get_int(const char *json, const char *key, int fallback);
int jp_json_get_bool(const char *json, const char *key, int fallback);
void jp_json_escape_append(char **buf, size_t *len, size_t *cap, const char *text);
char *jp_json_string(const char *text);
int jp_write_connection_file(const char *path);

#endif
