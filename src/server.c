#include "jupyter_proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zmq.h>

#define PORT 8080
#define REQ_LIMIT (1024 * 1024)
#define NOTEBOOK_PATH "data/notebook.json"
#define MAX_EVENTS 512

static volatile sig_atomic_t running = 1;
static const char *frontend_session = "zmqbook-c-browser";

struct buffer {
    char *data;
    size_t len;
};

struct app_state {
    void *context;
    void *shell;
    void *iopub;
    void *stdin_sock;
    void *control;
    void *heartbeat;
    char *events[MAX_EVENTS];
    int event_count;
    int next_event_id;
    char *last_stdin_header;
};

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

static char *read_file(const char *path, size_t *len_out) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    char *data = calloc((size_t)size + 1, 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    size_t read_count = fread(data, 1, (size_t)size, file);
    fclose(file);
    data[read_count] = '\0';
    if (len_out) *len_out = read_count;
    return data;
}

static int write_file(const char *path, const char *data, size_t len) {
    FILE *file = fopen(path, "wb");
    if (!file) return -1;
    size_t written = fwrite(data, 1, len, file);
    fclose(file);
    return written == len ? 0 : -1;
}

static int ensure_data_dir(void) {
    if (mkdir("data", 0755) == -1 && errno != EEXIST) return -1;
    return 0;
}

static const char *mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(ext, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(ext, ".js") == 0) return "application/javascript; charset=utf-8";
    if (strcmp(ext, ".json") == 0) return "application/json; charset=utf-8";
    return "application/octet-stream";
}

static void send_response(int client, int status, const char *status_text, const char *type,
                          const char *body, size_t len) {
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        status, status_text, type, len);
    send(client, header, (size_t)header_len, 0);
    if (len > 0) send(client, body, len, 0);
}

static void send_json_text(int client, int status, const char *status_text, const char *body) {
    send_response(client, status, status_text, "application/json; charset=utf-8", body, strlen(body));
}

static void send_not_found(int client) {
    send_json_text(client, 404, "Not Found", "{\"ok\":false,\"error\":\"not found\"}");
}

static char *extract_header_value(const char *request, const char *name) {
    const char *p = request;
    size_t name_len = strlen(name);
    while ((p = strstr(p, name)) != NULL) {
        if ((p == request || p[-1] == '\n') && strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            p += name_len + 1;
            while (*p == ' ') p++;
            const char *end = strstr(p, "\r\n");
            if (!end) return NULL;
            size_t len = (size_t)(end - p);
            char *value = calloc(len + 1, 1);
            memcpy(value, p, len);
            return value;
        }
        p += name_len;
    }
    return NULL;
}

static struct buffer receive_request(int client) {
    struct buffer result = {0};
    size_t cap = 8192;
    result.data = calloc(cap + 1, 1);
    if (!result.data) return result;

    ssize_t n;
    while ((n = recv(client, result.data + result.len, cap - result.len, 0)) > 0) {
        result.len += (size_t)n;
        result.data[result.len] = '\0';
        char *headers_end = strstr(result.data, "\r\n\r\n");
        if (headers_end) {
            char *value = extract_header_value(result.data, "Content-Length");
            size_t content_len = value ? (size_t)strtoul(value, NULL, 10) : 0;
            free(value);
            size_t header_len = (size_t)(headers_end + 4 - result.data);
            if (result.len >= header_len + content_len) break;
        }
        if (result.len >= REQ_LIMIT) break;
        if (cap - result.len < 4096) {
            cap *= 2;
            if (cap > REQ_LIMIT) cap = REQ_LIMIT;
            char *next = realloc(result.data, cap + 1);
            if (!next) break;
            result.data = next;
        }
    }
    return result;
}

static char *request_body(char *request) {
    char *body = strstr(request, "\r\n\r\n");
    return body ? body + 4 : request + strlen(request);
}

static int parse_request_line(const char *request, char *method, size_t method_size, char *path, size_t path_size) {
    return sscanf(request, "%15s %255s", method, path) == 2 &&
        strlen(method) < method_size && strlen(path) < path_size;
}

static int path_is(const char *path, const char *exact) {
    size_t len = strlen(exact);
    return strncmp(path, exact, len) == 0 && (path[len] == '\0' || path[len] == '?');
}

static int query_after(const char *path) {
    const char *p = strstr(path, "after=");
    return p ? (int)strtol(p + 6, NULL, 10) : 0;
}

static void serve_static(int client, const char *path) {
    char clean_path[256];
    snprintf(clean_path, sizeof(clean_path), "%s", path);
    char *query = strchr(clean_path, '?');
    if (query) *query = '\0';

    char file_path[512];
    if (strcmp(clean_path, "/") == 0) {
        snprintf(file_path, sizeof(file_path), "web/index.html");
    } else if (strstr(clean_path, "..")) {
        send_not_found(client);
        return;
    } else {
        snprintf(file_path, sizeof(file_path), "web%s", clean_path);
    }

    size_t len = 0;
    char *data = read_file(file_path, &len);
    if (!data) {
        send_not_found(client);
        return;
    }
    send_response(client, 200, "OK", mime_type(file_path), data, len);
    free(data);
}

static int connect_socket(void *socket, int port, const char *name) {
    char endpoint[64];
    snprintf(endpoint, sizeof(endpoint), "tcp://127.0.0.1:%d", port);
    if (zmq_connect(socket, endpoint) == -1) {
        fprintf(stderr, "%s connect failed on %s: %s\n", name, endpoint, zmq_strerror(zmq_errno()));
        return -1;
    }
    return 0;
}

static void queue_event(struct app_state *state, const char *channel, const struct jp_message *msg) {
    char *parent_id = jp_json_get_string(msg->parent_header, "msg_id");
    char *escaped_type = jp_json_string(msg->msg_type);
    char *escaped_channel = jp_json_string(channel);
    char *escaped_parent = jp_json_string(parent_id ? parent_id : "");
    free(parent_id);

    size_t len = strlen(msg->content) + strlen(escaped_type) + strlen(escaped_channel) + strlen(escaped_parent) + 256;
    char *event = calloc(len, 1);
    snprintf(event, len,
        "{\"id\":%d,\"channel\":%s,\"msg_type\":%s,\"parent_msg_id\":%s,\"content\":%s}",
        state->next_event_id++, escaped_channel, escaped_type, escaped_parent, msg->content);

    free(escaped_type);
    free(escaped_channel);
    free(escaped_parent);

    if (state->event_count == MAX_EVENTS) {
        free(state->events[0]);
        memmove(state->events, state->events + 1, sizeof(char *) * (MAX_EVENTS - 1));
        state->event_count--;
    }
    state->events[state->event_count++] = event;
}

static void drain_socket_events(struct app_state *state, void *socket, const char *channel) {
    while (1) {
        zmq_pollitem_t item = {socket, 0, ZMQ_POLLIN, 0};
        int rc = zmq_poll(&item, 1, 0);
        if (rc <= 0 || !(item.revents & ZMQ_POLLIN)) break;

        struct jp_message msg;
        if (jp_recv_message(socket, &msg, ZMQ_DONTWAIT) == -1) break;
        if (strcmp(channel, "stdin") == 0 && strcmp(msg.msg_type, "input_request") == 0) {
            free(state->last_stdin_header);
            state->last_stdin_header = strdup(msg.header);
        }
        queue_event(state, channel, &msg);
        jp_msg_free(&msg);
    }
}

static void drain_kernel_events(struct app_state *state) {
    drain_socket_events(state, state->shell, "shell");
    drain_socket_events(state, state->iopub, "iopub");
    drain_socket_events(state, state->stdin_sock, "stdin");
    drain_socket_events(state, state->control, "control");
}

static void send_events_response(struct app_state *state, int client, int after) {
    drain_kernel_events(state);

    size_t cap = 1024, len = 0;
    char *body = calloc(cap, 1);
    snprintf(body, cap, "{\"ok\":true,\"events\":[");
    len = strlen(body);
    int first = 1;
    for (int i = 0; i < state->event_count; i++) {
        const char *event = state->events[i];
        int id = 0;
        sscanf(event, "{\"id\":%d", &id);
        if (id <= after) continue;
        size_t need = strlen(event) + 3;
        if (len + need + 32 > cap) {
            while (len + need + 32 > cap) cap *= 2;
            body = realloc(body, cap);
        }
        if (!first) body[len++] = ',';
        first = 0;
        strcpy(body + len, event);
        len += strlen(event);
    }
    strcpy(body + len, "]}");
    send_response(client, 200, "OK", "application/json; charset=utf-8", body, strlen(body));
    free(body);
}

static void send_shell_execute(struct app_state *state, int client, const char *body) {
    int rc = jp_send_message(state->shell, NULL, frontend_session, "execute_request", "{}",
        body && *body ? body : "{\"code\":\"\",\"silent\":false,\"store_history\":true,\"allow_stdin\":true}");
    if (rc == -1) {
        send_json_text(client, 503, "Service Unavailable", "{\"ok\":false,\"error\":\"failed to send execute_request\"}");
        return;
    }
    send_json_text(client, 200, "OK", "{\"ok\":true,\"message\":\"execute_request sent\"}");
}

static void send_control_message(struct app_state *state, int client, const char *msg_type) {
    const char *content = strcmp(msg_type, "shutdown_request") == 0
        ? "{\"restart\":false}"
        : "{}";
    if (jp_send_message(state->control, NULL, frontend_session, msg_type, "{}", content) == -1) {
        send_json_text(client, 503, "Service Unavailable", "{\"ok\":false,\"error\":\"failed to send control message\"}");
        return;
    }
    send_json_text(client, 200, "OK", "{\"ok\":true}");
}

static void send_stdin_reply(struct app_state *state, int client, const char *body) {
    char *value = jp_json_get_string(body, "value");
    if (!value) value = strdup("");
    char *escaped = jp_json_string(value);
    size_t content_len = strlen(escaped) + 32;
    char *content = calloc(content_len, 1);
    snprintf(content, content_len, "{\"value\":%s}", escaped);

    struct jp_message parent;
    jp_msg_init(&parent);
    parent.header = strdup(state->last_stdin_header ? state->last_stdin_header : "{}");
    int rc = jp_send_message(state->stdin_sock, &parent, frontend_session, "input_reply", "{}", content);
    jp_msg_free(&parent);
    free(content);
    free(escaped);
    free(value);

    if (rc == -1) {
        send_json_text(client, 503, "Service Unavailable", "{\"ok\":false,\"error\":\"failed to send input_reply\"}");
    } else {
        send_json_text(client, 200, "OK", "{\"ok\":true}");
    }
}

static void send_heartbeat(struct app_state *state, int client) {
    const char *ping = "ping";
    int rc = zmq_send(state->heartbeat, ping, strlen(ping), 0);
    if (rc == -1) {
        send_json_text(client, 200, "OK", "{\"ok\":true,\"alive\":false}");
        return;
    }
    char reply[16] = {0};
    rc = zmq_recv(state->heartbeat, reply, sizeof(reply) - 1, 0);
    if (rc == -1) {
        send_json_text(client, 200, "OK", "{\"ok\":true,\"alive\":false}");
        return;
    }
    send_json_text(client, 200, "OK", "{\"ok\":true,\"alive\":true}");
}

static void handle_api(int client, const char *method, const char *path, char *body, struct app_state *state) {
    if (strcmp(method, "GET") == 0 && path_is(path, "/api/notebook")) {
        size_t len = 0;
        char *data = read_file(NOTEBOOK_PATH, &len);
        if (!data) {
            data = strdup("{\"title\":\"ZeroMQ Notebook Clone\",\"cells\":[{\"code\":\"printf(\\\"hello zeromq notebook\\\\n\\\");\",\"output\":\"\"}]}");
            len = strlen(data);
        }
        send_response(client, 200, "OK", "application/json; charset=utf-8", data, len);
        free(data);
        return;
    }

    if (strcmp(method, "POST") == 0 && path_is(path, "/api/notebook/save")) {
        if (ensure_data_dir() == -1 || write_file(NOTEBOOK_PATH, body, strlen(body)) == -1) {
            send_json_text(client, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"failed to save notebook\"}");
            return;
        }
        send_json_text(client, 200, "OK", "{\"ok\":true}");
        return;
    }

    if (strcmp(method, "POST") == 0 && (path_is(path, "/api/cell/run") || path_is(path, "/api/run-all"))) {
        send_shell_execute(state, client, body);
        return;
    }

    if (strcmp(method, "GET") == 0 && path_is(path, "/api/kernel/events")) {
        send_events_response(state, client, query_after(path));
        return;
    }

    if (strcmp(method, "POST") == 0 && path_is(path, "/api/stdin/reply")) {
        send_stdin_reply(state, client, body);
        return;
    }

    if (strcmp(method, "POST") == 0 && path_is(path, "/api/kernel/interrupt")) {
        send_control_message(state, client, "interrupt_request");
        return;
    }

    if (strcmp(method, "POST") == 0 && path_is(path, "/api/kernel/shutdown")) {
        send_control_message(state, client, "shutdown_request");
        return;
    }

    if (strcmp(method, "GET") == 0 && path_is(path, "/api/kernel/heartbeat")) {
        send_heartbeat(state, client);
        return;
    }

    send_not_found(client);
}

static int init_zmq_frontend(struct app_state *state) {
    state->context = zmq_ctx_new();
    if (!state->context) return -1;
    state->shell = zmq_socket(state->context, ZMQ_DEALER);
    state->iopub = zmq_socket(state->context, ZMQ_SUB);
    state->stdin_sock = zmq_socket(state->context, ZMQ_DEALER);
    state->control = zmq_socket(state->context, ZMQ_DEALER);
    state->heartbeat = zmq_socket(state->context, ZMQ_REQ);
    if (!state->shell || !state->iopub || !state->stdin_sock || !state->control || !state->heartbeat) return -1;

    int linger = 0;
    int timeout = 1000;
    const char *identity = "browser";
    zmq_setsockopt(state->shell, ZMQ_IDENTITY, identity, strlen(identity));
    zmq_setsockopt(state->stdin_sock, ZMQ_IDENTITY, identity, strlen(identity));
    zmq_setsockopt(state->control, ZMQ_IDENTITY, identity, strlen(identity));
    zmq_setsockopt(state->shell, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_setsockopt(state->stdin_sock, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_setsockopt(state->control, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_setsockopt(state->iopub, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_setsockopt(state->heartbeat, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_setsockopt(state->heartbeat, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_setsockopt(state->heartbeat, ZMQ_SNDTIMEO, &timeout, sizeof(timeout));
    zmq_setsockopt(state->iopub, ZMQ_SUBSCRIBE, "", 0);

    return connect_socket(state->shell, JP_SHELL_PORT, "shell") == -1 ||
        connect_socket(state->iopub, JP_IOPUB_PORT, "iopub") == -1 ||
        connect_socket(state->stdin_sock, JP_STDIN_PORT, "stdin") == -1 ||
        connect_socket(state->control, JP_CONTROL_PORT, "control") == -1 ||
        connect_socket(state->heartbeat, JP_HB_PORT, "heartbeat") == -1 ? -1 : 0;
}

static void close_zmq_frontend(struct app_state *state) {
    if (state->shell) zmq_close(state->shell);
    if (state->iopub) zmq_close(state->iopub);
    if (state->stdin_sock) zmq_close(state->stdin_sock);
    if (state->control) zmq_close(state->control);
    if (state->heartbeat) zmq_close(state->heartbeat);
    if (state->context) zmq_ctx_destroy(state->context);
    for (int i = 0; i < state->event_count; i++) free(state->events[i]);
    free(state->last_stdin_header);
}

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    ensure_data_dir();

    struct app_state state = {0};
    if (init_zmq_frontend(&state) == -1) {
        fprintf(stderr, "warning: ZeroMQ frontend did not fully connect; start kernel_worker before using execution APIs\n");
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("bind");
        return 1;
    }
    if (listen(server_fd, 16) == -1) {
        perror("listen");
        return 1;
    }

    printf("Notebook clone server: http://127.0.0.1:%d\n", PORT);
    printf("WSL/network access: http://<wsl-ip>:%d\n", PORT);
    printf("Connected as frontend to Jupyter-style ZeroMQ channels.\n");
    fflush(stdout);

    while (running) {
        drain_kernel_events(&state);
        int client = accept(server_fd, NULL, NULL);
        if (client == -1) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        struct buffer request = receive_request(client);
        if (!request.data || request.len == 0) {
            close(client);
            free(request.data);
            continue;
        }

        char method[16] = {0};
        char path[256] = {0};
        if (!parse_request_line(request.data, method, sizeof(method), path, sizeof(path))) {
            send_json_text(client, 400, "Bad Request", "{\"ok\":false,\"error\":\"bad request\"}");
        } else if (strncmp(path, "/api/", 5) == 0) {
            handle_api(client, method, path, request_body(request.data), &state);
        } else {
            serve_static(client, path);
        }

        close(client);
        free(request.data);
    }

    close(server_fd);
    close_zmq_frontend(&state);
    return 0;
}
