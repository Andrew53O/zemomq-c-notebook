#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zmq.h>

#define MAX_REQUEST (1024 * 1024)
#define RUN_TIMEOUT_SECONDS 3

static volatile sig_atomic_t running = 1;

struct cell_list {
    char **items;
    int count;
    int cap;
};

struct string_builder {
    char *data;
    size_t len;
    size_t cap;
};

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

static void sb_init(struct string_builder *sb) {
    sb->cap = 4096;
    sb->len = 0;
    sb->data = calloc(sb->cap, 1);
}

static void sb_reserve(struct string_builder *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) {
        return;
    }
    while (sb->len + extra + 1 > sb->cap) {
        sb->cap *= 2;
    }
    sb->data = realloc(sb->data, sb->cap);
}

static void sb_append_n(struct string_builder *sb, const char *text, size_t len) {
    sb_reserve(sb, len);
    memcpy(sb->data + sb->len, text, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

static void sb_append(struct string_builder *sb, const char *text) {
    sb_append_n(sb, text, strlen(text));
}

static void sb_append_json_string(struct string_builder *sb, const char *text) {
    sb_append(sb, "\"");
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        char escape[8];
        switch (*p) {
            case '\\': sb_append(sb, "\\\\"); break;
            case '"': sb_append(sb, "\\\""); break;
            case '\n': sb_append(sb, "\\n"); break;
            case '\r': sb_append(sb, "\\r"); break;
            case '\t': sb_append(sb, "\\t"); break;
            default:
                if (*p < 32) {
                    snprintf(escape, sizeof(escape), "\\u%04x", *p);
                    sb_append(sb, escape);
                } else {
                    sb_append_n(sb, (const char *)p, 1);
                }
        }
    }
    sb_append(sb, "\"");
}

static char *copy_range(const char *start, const char *end) {
    size_t len = (size_t)(end - start);
    char *out = calloc(len + 1, 1);
    memcpy(out, start, len);
    return out;
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return strdup("");
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0) {
        fclose(file);
        return strdup("");
    }
    char *data = calloc((size_t)size + 1, 1);
    size_t count = fread(data, 1, (size_t)size, file);
    data[count] = '\0';
    fclose(file);
    return data;
}

static void cells_push(struct cell_list *cells, char *value) {
    if (cells->count == cells->cap) {
        cells->cap = cells->cap ? cells->cap * 2 : 8;
        cells->items = realloc(cells->items, sizeof(char *) * (size_t)cells->cap);
    }
    cells->items[cells->count++] = value;
}

static void cells_free(struct cell_list *cells) {
    for (int i = 0; i < cells->count; i++) {
        free(cells->items[i]);
    }
    free(cells->items);
    cells->items = NULL;
    cells->count = 0;
    cells->cap = 0;
}

static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static char *parse_json_string(const char **cursor) {
    const char *p = skip_ws(*cursor);
    if (*p != '"') {
        return NULL;
    }
    p++;

    struct string_builder sb;
    sb_init(&sb);
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': sb_append(&sb, "\n"); break;
                case 'r': sb_append(&sb, "\r"); break;
                case 't': sb_append(&sb, "\t"); break;
                case '"': sb_append(&sb, "\""); break;
                case '\\': sb_append(&sb, "\\"); break;
                default: sb_append_n(&sb, p, 1); break;
            }
            if (*p) p++;
        } else {
            sb_append_n(&sb, p, 1);
            p++;
        }
    }
    if (*p != '"') {
        free(sb.data);
        return NULL;
    }
    *cursor = p + 1;
    return sb.data;
}

static int parse_request(const char *json, int *run_index, struct cell_list *cells) {
    *run_index = -1;

    const char *ri = strstr(json, "\"run_index\"");
    if (ri) {
        const char *colon = strchr(ri, ':');
        if (colon) {
            *run_index = (int)strtol(colon + 1, NULL, 10);
        }
    }

    const char *cp = strstr(json, "\"cells\"");
    if (!cp) {
        return -1;
    }
    const char *array = strchr(cp, '[');
    if (!array) {
        return -1;
    }
    const char *p = array + 1;
    while (*p) {
        p = skip_ws(p);
        if (*p == ']') {
            break;
        }
        if (*p != '"') {
            return -1;
        }
        char *cell = parse_json_string(&p);
        if (!cell) {
            return -1;
        }
        cells_push(cells, cell);
        p = skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == ']') {
            break;
        }
        return -1;
    }

    if (cells->count == 0) {
        cells_push(cells, strdup(""));
    }
    if (*run_index < 0 || *run_index >= cells->count) {
        *run_index = cells->count - 1;
    }
    return 0;
}

static int ensure_runtime_dir(void) {
    if (mkdir("runtime", 0755) == -1 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int write_generated_source(const char *path, struct cell_list *cells, int run_index) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        return -1;
    }

    fprintf(file,
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "#include <math.h>\n\n"
        "int main(void) {\n"
        "    setvbuf(stdout, NULL, _IONBF, 0);\n");

    for (int i = 0; i <= run_index; i++) {
        fprintf(file, "    printf(\"\\n__CELL_START_%d__\\n\");\n", i);
        fprintf(file, "    fflush(stdout);\n");
        fprintf(file, "#line 1 \"cell_%d.c\"\n", i + 1);
        fprintf(file, "%s\n", cells->items[i]);
        fprintf(file, "#line 1 \"generated_notebook.c\"\n");
        fprintf(file, "    printf(\"\\n__CELL_END_%d__\\n\");\n", i);
        fprintf(file, "    fflush(stdout);\n");
    }

    fprintf(file, "    return 0;\n}\n");
    fclose(file);
    return 0;
}

static int run_command(const char *command) {
    int status = system(command);
    if (status == -1) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 128;
}

static char **split_outputs(const char *run_output, int count) {
    char **outputs = calloc((size_t)count, sizeof(char *));
    for (int i = 0; i < count; i++) {
        outputs[i] = strdup("");
    }

    for (int i = 0; i < count; i++) {
        char start_marker[64];
        char end_marker[64];
        snprintf(start_marker, sizeof(start_marker), "__CELL_START_%d__", i);
        snprintf(end_marker, sizeof(end_marker), "__CELL_END_%d__", i);

        char *start = strstr(run_output, start_marker);
        char *end = strstr(run_output, end_marker);
        if (!start || !end || end < start) {
            continue;
        }
        start += strlen(start_marker);
        while (*start == '\n' || *start == '\r') start++;
        while (end > start && (end[-1] == '\n' || end[-1] == '\r')) end--;

        free(outputs[i]);
        outputs[i] = copy_range(start, end);
    }

    return outputs;
}

static char *json_error(const char *message) {
    struct string_builder sb;
    sb_init(&sb);
    sb_append(&sb, "{\"ok\":false,\"error\":");
    sb_append_json_string(&sb, message);
    sb_append(&sb, "}");
    return sb.data;
}

static int send_msg_frame(void *socket, const char *data, int flags) {
    zmq_msg_t message;
    size_t len = strlen(data);
    if (zmq_msg_init_size(&message, len) == -1) {
        return -1;
    }
    memcpy(zmq_msg_data(&message), data, len);
    int rc = zmq_msg_send(&message, socket, flags);
    zmq_msg_close(&message);
    return rc;
}

static void publish_status(void *publisher, const char *event) {
    if (!publisher) {
        return;
    }
    zmq_send(publisher, "kernel", 6, ZMQ_SNDMORE);
    zmq_send(publisher, event, strlen(event), 0);
}

static char *recv_request_body(void *socket) {
    char *body = NULL;
    int more = 0;
    size_t more_size = sizeof(more);

    do {
        zmq_msg_t message;
        if (zmq_msg_init(&message) == -1) {
            free(body);
            return NULL;
        }
        int n = zmq_msg_recv(&message, socket, 0);
        if (n == -1) {
            zmq_msg_close(&message);
            free(body);
            return NULL;
        }

        size_t len = zmq_msg_size(&message);
        char *frame = calloc(len + 1, 1);
        if (frame) {
            memcpy(frame, zmq_msg_data(&message), len);
            frame[len] = '\0';
        }

        free(body);
        body = frame;

        zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &more_size);
        zmq_msg_close(&message);
    } while (more);

    return body;
}

static char *execute_request(const char *request) {
    struct cell_list cells = {0};
    int run_index = -1;
    if (parse_request(request, &run_index, &cells) == -1) {
        return json_error("invalid request JSON; expected run_index and cells array");
    }

    if (ensure_runtime_dir() == -1) {
        cells_free(&cells);
        return json_error("failed to create runtime directory");
    }

    char base[128];
    snprintf(base, sizeof(base), "runtime/job_%ld", (long)getpid());
    char source_path[160], binary_path[160], compile_path[160], output_path[160], command[1024];
    snprintf(source_path, sizeof(source_path), "%s.c", base);
    snprintf(binary_path, sizeof(binary_path), "%s.out", base);
    snprintf(compile_path, sizeof(compile_path), "%s.compile.txt", base);
    snprintf(output_path, sizeof(output_path), "%s.output.txt", base);

    if (write_generated_source(source_path, &cells, run_index) == -1) {
        cells_free(&cells);
        return json_error("failed to write generated source");
    }

    snprintf(command, sizeof(command),
        "gcc -Wall -Wextra -std=c11 '%s' -o '%s' -lm > '%s' 2>&1",
        source_path, binary_path, compile_path);
    int compile_code = run_command(command);
    char *compile_output = read_file(compile_path);
    if (compile_code != 0) {
        struct string_builder sb;
        sb_init(&sb);
        sb_append(&sb, "{\"ok\":false,\"stage\":\"compile\",\"run_index\":");
        char number[32];
        snprintf(number, sizeof(number), "%d", run_index);
        sb_append(&sb, number);
        sb_append(&sb, ",\"error\":");
        sb_append_json_string(&sb, compile_output);
        sb_append(&sb, "}");
        free(compile_output);
        cells_free(&cells);
        return sb.data;
    }
    free(compile_output);

    snprintf(command, sizeof(command),
        "timeout %ds '%s' > '%s' 2>&1",
        RUN_TIMEOUT_SECONDS, binary_path, output_path);
    int run_code = run_command(command);
    char *run_output = read_file(output_path);
    char **outputs = split_outputs(run_output, run_index + 1);

    struct string_builder sb;
    sb_init(&sb);
    sb_append(&sb, "{\"ok\":");
    sb_append(&sb, run_code == 0 ? "true" : "false");
    sb_append(&sb, ",\"stage\":\"run\",\"run_index\":");
    char number[32];
    snprintf(number, sizeof(number), "%d", run_index);
    sb_append(&sb, number);
    sb_append(&sb, ",\"outputs\":[");
    for (int i = 0; i <= run_index; i++) {
        if (i > 0) sb_append(&sb, ",");
        sb_append_json_string(&sb, outputs[i]);
    }
    sb_append(&sb, "]");
    if (run_code == 124) {
        sb_append(&sb, ",\"error\":\"execution timed out\"");
    } else if (run_code != 0) {
        sb_append(&sb, ",\"error\":\"program exited with non-zero status\"");
    }
    sb_append(&sb, "}");

    for (int i = 0; i <= run_index; i++) {
        free(outputs[i]);
    }
    free(outputs);
    free(run_output);
    cells_free(&cells);
    return sb.data;
}

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    void *context = zmq_ctx_new();
    void *responder = zmq_socket(context, ZMQ_REP);
    void *status_pub = zmq_socket(context, ZMQ_PUB);
    int linger = 0;
    zmq_setsockopt(responder, ZMQ_LINGER, &linger, sizeof(linger));
    if (status_pub) {
        zmq_setsockopt(status_pub, ZMQ_LINGER, &linger, sizeof(linger));
        zmq_connect(status_pub, "tcp://127.0.0.1:7002");
    }

    if (zmq_connect(responder, "tcp://127.0.0.1:7001") == -1) {
        fprintf(stderr, "worker connect failed: %s\n", zmq_strerror(zmq_errno()));
        return 1;
    }

    printf("C kernel worker connected to tcp://127.0.0.1:7001\n");
    fflush(stdout);
    publish_status(status_pub, "worker-ready");

    while (running) {
        zmq_pollitem_t item = {responder, 0, ZMQ_POLLIN, 0};
        int poll_rc = zmq_poll(&item, 1, 500);
        if (poll_rc == -1) {
            if (zmq_errno() == EINTR) continue;
            fprintf(stderr, "worker poll failed: %s\n", zmq_strerror(zmq_errno()));
            break;
        }
        if (poll_rc == 0 || !(item.revents & ZMQ_POLLIN)) {
            continue;
        }

        char *request = recv_request_body(responder);
        if (!request) {
            if (zmq_errno() == EINTR) continue;
            fprintf(stderr, "worker message receive failed: %s\n", zmq_strerror(zmq_errno()));
            break;
        }

        publish_status(status_pub, "run-start");
        char *reply = execute_request(request);
        publish_status(status_pub, "run-finished");
        send_msg_frame(responder, "RESULT", ZMQ_SNDMORE);
        send_msg_frame(responder, reply, 0);
        free(reply);
        free(request);
    }

    if (status_pub) {
        zmq_close(status_pub);
    }
    zmq_close(responder);
    zmq_ctx_destroy(context);
    return 0;
}
