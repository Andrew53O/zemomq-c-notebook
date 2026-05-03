#include "jupyter_proto.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zmq.h>

#define RUN_TIMEOUT_SECONDS 8
#define MAX_CELLS 128

static volatile sig_atomic_t running = 1;
static int execution_count = 0;
static const char *kernel_session = "zmqbook-c-kernel";

struct cell_list {
    char **items;
    int count;
};

struct string_builder {
    char *data;
    size_t len;
    size_t cap;
};

struct kernel_sockets {
    void *shell;
    void *iopub;
    void *stdin_sock;
    void *control;
    void *heartbeat;
};

struct exec_result {
    int ok;
    int interrupted;
    int timed_out;
    int exit_code;
    char *error;
    char **outputs;
    int output_count;
};

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

static void sb_init(struct string_builder *sb) {
    sb->cap = 512;
    sb->len = 0;
    sb->data = calloc(sb->cap, 1);
}

static void sb_reserve(struct string_builder *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    while (sb->len + extra + 1 > sb->cap) sb->cap *= 2;
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

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return strdup("");
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

static char *copy_range(const char *start, const char *end) {
    size_t len = (size_t)(end - start);
    char *out = calloc(len + 1, 1);
    if (!out) return strdup("");
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static int ensure_dirs(void) {
    if (mkdir("data", 0755) == -1 && errno != EEXIST) return -1;
    if (mkdir("runtime", 0755) == -1 && errno != EEXIST) return -1;
    return 0;
}

static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static char *parse_json_string_at(const char **cursor) {
    const char *p = skip_ws(*cursor);
    if (*p != '"') return NULL;
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
                default:
                    if (*p) sb_append_n(&sb, p, 1);
                    break;
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

static void cells_free(struct cell_list *cells) {
    for (int i = 0; i < cells->count; i++) free(cells->items[i]);
    free(cells->items);
    cells->items = NULL;
    cells->count = 0;
}

static void cells_push(struct cell_list *cells, char *value) {
    if (cells->count >= MAX_CELLS) {
        free(value);
        return;
    }
    cells->items = realloc(cells->items, sizeof(char *) * (size_t)(cells->count + 1));
    cells->items[cells->count++] = value;
}

static void parse_cells_array(const char *json, struct cell_list *cells) {
    const char *cp = strstr(json, "\"cells\"");
    if (!cp) return;
    const char *p = strchr(cp, '[');
    if (!p) return;
    p++;
    while (*p) {
        p = skip_ws(p);
        if (*p == ']') break;
        if (*p != '"') break;
        char *cell = parse_json_string_at(&p);
        if (!cell) break;
        cells_push(cells, cell);
        p = skip_ws(p);
        if (*p == ',') p++;
    }
}

static int parse_execute_content(const char *content, int *run_index, struct cell_list *cells, char **code_out) {
    *run_index = jp_json_get_int(content, "run_index", -1);
    parse_cells_array(content, cells);
    char *code = jp_json_get_string(content, "code");
    if (!code) code = strdup("");
    *code_out = code;

    if (cells->count == 0) {
        cells_push(cells, strdup(code));
    }
    if (*run_index < 0 || *run_index >= cells->count) {
        *run_index = cells->count - 1;
    }
    return cells->count > 0 ? 0 : -1;
}

static int write_generated_source(const char *path, const struct cell_list *cells, int run_index) {
    FILE *file = fopen(path, "wb");
    if (!file) return -1;

    fprintf(file,
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "#include <stdarg.h>\n"
        "#include <math.h>\n\n"
        "static void nb_input(const char *prompt, char *buffer, size_t size) {\n"
        "    if (!buffer || size == 0) return;\n"
        "    printf(\"__NB_INPUT__:%%s\\n\", prompt ? prompt : \"\");\n"
        "    fflush(stdout);\n"
        "    if (!fgets(buffer, (int)size, stdin)) { buffer[0] = '\\0'; return; }\n"
        "    buffer[strcspn(buffer, \"\\r\\n\")] = '\\0';\n"
        "}\n\n"
        "static int nb_scanf(const char *format, ...) {\n"
        "    char line[256];\n"
        "    printf(\"__NB_INPUT__:\\n\");\n"
        "    fflush(stdout);\n"
        "    if (!fgets(line, sizeof line, stdin)) return EOF;\n"
        "    va_list args;\n"
        "    va_start(args, format);\n"
        "    int rc = vsscanf(line, format, args);\n"
        "    va_end(args);\n"
        "    return rc;\n"
        "}\n"
        "#define scanf(...) nb_scanf(__VA_ARGS__)\n\n"
        "int main(void) {\n"
        "    setvbuf(stdout, NULL, _IONBF, 0);\n"
        "    setvbuf(stderr, NULL, _IONBF, 0);\n");

    for (int i = 0; i <= run_index; i++) {
        fprintf(file, "    printf(\"__CELL_START_%d__\\n\");\n", i);
        fprintf(file, "#line 1 \"cell_%d.c\"\n", i + 1);
        fprintf(file, "%s\n", cells->items[i]);
        fprintf(file, "#line 1 \"generated_notebook.c\"\n");
        fprintf(file, "    printf(\"__CELL_END_%d__\\n\");\n", i);
    }

    fprintf(file, "    return 0;\n}\n");
    fclose(file);
    return 0;
}

static int run_command(const char *command) {
    int status = system(command);
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128;
}

static int bind_socket(void *socket, int port, const char *name) {
    char endpoint[64];
    snprintf(endpoint, sizeof(endpoint), "tcp://127.0.0.1:%d", port);
    if (zmq_bind(socket, endpoint) == -1) {
        fprintf(stderr, "%s bind failed on %s: %s\n", name, endpoint, zmq_strerror(zmq_errno()));
        return -1;
    }
    return 0;
}

static void publish_status(void *iopub, const struct jp_message *parent, const char *state) {
    char content[128];
    snprintf(content, sizeof(content), "{\"execution_state\":\"%s\"}", state);
    jp_send_iopub(iopub, "status", parent, kernel_session, "status", "{}", content);
}

static void publish_execute_input(void *iopub, const struct jp_message *parent, const char *code, int count) {
    char *escaped = jp_json_string(code);
    char content[4096];
    snprintf(content, sizeof(content), "{\"code\":%s,\"execution_count\":%d}", escaped, count);
    jp_send_iopub(iopub, "execute_input", parent, kernel_session, "execute_input", "{}", content);
    free(escaped);
}

static void publish_stream(void *iopub, const struct jp_message *parent, int cell_index, const char *text) {
    char *escaped = jp_json_string(text);
    char content[4096];
    snprintf(content, sizeof(content), "{\"name\":\"stdout\",\"text\":%s,\"cell_index\":%d}", escaped, cell_index);
    jp_send_iopub(iopub, "stream.stdout", parent, kernel_session, "stream", "{}", content);
    free(escaped);
}

static void publish_error(void *iopub, const struct jp_message *parent, const char *ename, const char *evalue) {
    char *escaped_name = jp_json_string(ename);
    char *escaped_value = jp_json_string(evalue);
    char content[4096];
    snprintf(content, sizeof(content),
        "{\"ename\":%s,\"evalue\":%s,\"traceback\":[%s]}",
        escaped_name, escaped_value, escaped_value);
    jp_send_iopub(iopub, "error", parent, kernel_session, "error", "{}", content);
    free(escaped_name);
    free(escaped_value);
}

static void send_heartbeat_reply(void *heartbeat) {
    zmq_msg_t msg;
    if (zmq_msg_init(&msg) == -1) return;
    int n = zmq_msg_recv(&msg, heartbeat, ZMQ_DONTWAIT);
    if (n != -1) {
        zmq_msg_send(&msg, heartbeat, 0);
    }
    zmq_msg_close(&msg);
}

static int handle_control_once(struct kernel_sockets *socks, pid_t child, int *interrupted) {
    struct jp_message msg;
    if (jp_recv_message(socks->control, &msg, ZMQ_DONTWAIT) == -1) {
        return 0;
    }

    if (strcmp(msg.msg_type, "interrupt_request") == 0) {
        if (child > 0) kill(child, SIGTERM);
        *interrupted = 1;
        jp_send_message(socks->control, &msg, kernel_session, "interrupt_reply", "{}", "{\"status\":\"ok\"}");
    } else if (strcmp(msg.msg_type, "shutdown_request") == 0) {
        if (child > 0) kill(child, SIGTERM);
        running = 0;
        jp_send_message(socks->control, &msg, kernel_session, "shutdown_reply", "{}", "{\"status\":\"ok\",\"restart\":false}");
    } else {
        jp_send_message(socks->control, &msg, kernel_session, "error", "{}", "{\"status\":\"error\",\"ename\":\"UnsupportedMessage\",\"evalue\":\"unsupported control message\"}");
    }

    jp_msg_free(&msg);
    return 1;
}

static char *request_stdin(struct kernel_sockets *socks, const struct jp_message *parent,
                           const char *prompt, pid_t child, int *interrupted) {
    char *escaped = jp_json_string(prompt);
    char content[2048];
    snprintf(content, sizeof(content), "{\"prompt\":%s,\"password\":false}", escaped);
    free(escaped);
    jp_send_message(socks->stdin_sock, parent, kernel_session, "input_request", "{}", content);

    while (running) {
        zmq_pollitem_t items[] = {
            {socks->stdin_sock, 0, ZMQ_POLLIN, 0},
            {socks->control, 0, ZMQ_POLLIN, 0},
            {socks->heartbeat, 0, ZMQ_POLLIN, 0}
        };
        int rc = zmq_poll(items, 3, 200);
        if (rc == -1) {
            if (zmq_errno() == EINTR) continue;
            break;
        }
        if (items[2].revents & ZMQ_POLLIN) send_heartbeat_reply(socks->heartbeat);
        if (items[1].revents & ZMQ_POLLIN) handle_control_once(socks, child, interrupted);
        if (*interrupted) return strdup("");
        if (items[0].revents & ZMQ_POLLIN) {
            struct jp_message reply;
            if (jp_recv_message(socks->stdin_sock, &reply, 0) == 0) {
                char *value = jp_json_get_string(reply.content, "value");
                jp_msg_free(&reply);
                return value ? value : strdup("");
            }
        }
    }
    return strdup("");
}

static void append_output(char **outputs, int count, int cell_index, const char *line) {
    if (cell_index < 0 || cell_index >= count) return;
    size_t old_len = outputs[cell_index] ? strlen(outputs[cell_index]) : 0;
    size_t line_len = strlen(line);
    outputs[cell_index] = realloc(outputs[cell_index], old_len + line_len + 2);
    memcpy(outputs[cell_index] + old_len, line, line_len);
    outputs[cell_index][old_len + line_len] = '\n';
    outputs[cell_index][old_len + line_len + 1] = '\0';
}

static void process_child_line(struct kernel_sockets *socks, const struct jp_message *parent,
                               char **outputs, int output_count, int *cell_index,
                               int child_stdin, pid_t child, int *interrupted, time_t *start,
                               const char *line) {
    if (strncmp(line, "__CELL_START_", 13) == 0) {
        *cell_index = atoi(line + 13);
        return;
    }
    if (strncmp(line, "__CELL_END_", 11) == 0) {
        *cell_index = -1;
        return;
    }
    char *input_marker = strstr(line, "__NB_INPUT__:");
    if (input_marker) {
        char *prefix = copy_range(line, input_marker);
        const char *marker_prompt = input_marker + 13;
        const char *prompt = *marker_prompt ? marker_prompt : prefix;
        if (!*prompt) prompt = "Input:";
        char *value = request_stdin(socks, parent, prompt, child, interrupted);
        if (child_stdin != -1 && value) {
            ssize_t ignored = write(child_stdin, value, strlen(value));
            (void)ignored;
            ignored = write(child_stdin, "\n", 1);
            (void)ignored;
        }
        if (start) *start = time(NULL);
        free(prefix);
        free(value);
        return;
    }

    int target = *cell_index >= 0 ? *cell_index : output_count - 1;
    append_output(outputs, output_count, target, line);
    publish_stream(socks->iopub, parent, target, line);
    publish_stream(socks->iopub, parent, target, "\n");
}

static struct exec_result execute_cells(struct kernel_sockets *socks, const struct jp_message *parent,
                                        struct cell_list *cells, int run_index) {
    struct exec_result result = {0};
    result.output_count = run_index + 1;
    result.outputs = calloc((size_t)result.output_count, sizeof(char *));
    for (int i = 0; i < result.output_count; i++) result.outputs[i] = strdup("");

    if (ensure_dirs() == -1) {
        result.error = strdup("failed to create runtime directory");
        return result;
    }

    char base[128];
    snprintf(base, sizeof(base), "runtime/job_%ld_%d", (long)getpid(), execution_count);
    char source_path[160], binary_path[160], compile_path[160], command[1024];
    snprintf(source_path, sizeof(source_path), "%s.c", base);
    snprintf(binary_path, sizeof(binary_path), "%s.out", base);
    snprintf(compile_path, sizeof(compile_path), "%s.compile.txt", base);

    if (write_generated_source(source_path, cells, run_index) == -1) {
        result.error = strdup("failed to write generated source");
        return result;
    }

    snprintf(command, sizeof(command),
        "gcc -Wall -Wextra -std=c11 '%s' -o '%s' -lm > '%s' 2>&1",
        source_path, binary_path, compile_path);
    int compile_code = run_command(command);
    if (compile_code != 0) {
        result.error = read_file(compile_path);
        publish_error(socks->iopub, parent, "CompileError", result.error);
        return result;
    }

    int out_pipe[2], in_pipe[2];
    if (pipe(out_pipe) == -1 || pipe(in_pipe) == -1) {
        result.error = strdup("failed to create runtime pipes");
        return result;
    }

    pid_t child = fork();
    if (child == -1) {
        result.error = strdup("failed to fork runtime process");
        return result;
    }

    if (child == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        execl(binary_path, binary_path, NULL);
        perror("exec");
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    fcntl(out_pipe[0], F_SETFL, fcntl(out_pipe[0], F_GETFL, 0) | O_NONBLOCK);

    struct string_builder pending;
    sb_init(&pending);
    int current_cell = -1;
    int child_done = 0;
    int status = 0;
    time_t start = time(NULL);

    while (running && !child_done) {
        zmq_pollitem_t items[] = {
            {NULL, out_pipe[0], ZMQ_POLLIN, 0},
            {socks->control, 0, ZMQ_POLLIN, 0},
            {socks->heartbeat, 0, ZMQ_POLLIN, 0}
        };
        int rc = zmq_poll(items, 3, 100);
        if (rc == -1) {
            if (zmq_errno() == EINTR) continue;
            break;
        }

        if (items[2].revents & ZMQ_POLLIN) send_heartbeat_reply(socks->heartbeat);
        if (items[1].revents & ZMQ_POLLIN) handle_control_once(socks, child, &result.interrupted);

        if (time(NULL) - start > RUN_TIMEOUT_SECONDS) {
            result.timed_out = 1;
            kill(child, SIGTERM);
        }

        if (items[0].revents & ZMQ_POLLIN) {
            char chunk[512];
            ssize_t n = read(out_pipe[0], chunk, sizeof(chunk));
            if (n > 0) {
                sb_append_n(&pending, chunk, (size_t)n);
                char *line_start = pending.data;
                char *newline;
                while ((newline = strchr(line_start, '\n')) != NULL) {
                    *newline = '\0';
                    if (newline > line_start && newline[-1] == '\r') newline[-1] = '\0';
                    process_child_line(socks, parent, result.outputs, result.output_count,
                        &current_cell, in_pipe[1], child, &result.interrupted, &start, line_start);
                    line_start = newline + 1;
                }
                size_t remaining = strlen(line_start);
                memmove(pending.data, line_start, remaining + 1);
                pending.len = remaining;
            }
        }

        pid_t wait_rc = waitpid(child, &status, WNOHANG);
        if (wait_rc == child) child_done = 1;
    }

    if (!child_done) {
        kill(child, SIGTERM);
        waitpid(child, &status, 0);
    }

    if (pending.len > 0) {
        process_child_line(socks, parent, result.outputs, result.output_count,
            &current_cell, in_pipe[1], child, &result.interrupted, &start, pending.data);
    }

    close(in_pipe[1]);
    close(out_pipe[0]);
    free(pending.data);

    if (result.interrupted) {
        result.error = strdup("execution interrupted");
    } else if (result.timed_out) {
        result.error = strdup("execution timed out");
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        result.ok = result.exit_code == 0;
        if (!result.ok) result.error = strdup("program exited with non-zero status");
    } else {
        result.error = strdup("program terminated unexpectedly");
    }

    if (result.error && !result.ok) {
        publish_error(socks->iopub, parent, "RuntimeError", result.error);
    }
    return result;
}

static void exec_result_free(struct exec_result *result) {
    for (int i = 0; i < result->output_count; i++) free(result->outputs[i]);
    free(result->outputs);
    free(result->error);
}

static void send_execute_reply(void *shell, const struct jp_message *request,
                               const struct exec_result *result, int run_index) {
    char *escaped_error = jp_json_string(result->error ? result->error : "");
    struct string_builder content;
    sb_init(&content);
    if (result->ok) {
        char prefix[256];
        snprintf(prefix, sizeof(prefix),
            "{\"status\":\"ok\",\"execution_count\":%d,\"run_index\":%d,\"outputs\":[",
            execution_count, run_index);
        sb_append(&content, prefix);
    } else {
        char prefix[512];
        snprintf(prefix, sizeof(prefix),
            "{\"status\":\"error\",\"execution_count\":%d,\"run_index\":%d,\"ename\":\"Error\",\"evalue\":%s,\"traceback\":[%s],\"outputs\":[",
            execution_count, run_index, escaped_error, escaped_error);
        sb_append(&content, prefix);
    }
    for (int i = 0; i < result->output_count; i++) {
        if (i > 0) sb_append(&content, ",");
        jp_json_escape_append(&content.data, &content.len, &content.cap,
            result->outputs[i] ? result->outputs[i] : "");
    }
    sb_append(&content, "]}");
    jp_send_message(shell, request, kernel_session, "execute_reply", "{}", content.data);
    free(content.data);
    free(escaped_error);
}

static void handle_execute(struct kernel_sockets *socks, const struct jp_message *request) {
    struct cell_list cells = {0};
    char *code = NULL;
    int run_index = -1;
    if (parse_execute_content(request->content, &run_index, &cells, &code) == -1) {
        jp_send_message(socks->shell, request, kernel_session, "execute_reply", "{}",
            "{\"status\":\"error\",\"execution_count\":0,\"ename\":\"BadRequest\",\"evalue\":\"invalid execute_request\",\"traceback\":[]}");
        free(code);
        cells_free(&cells);
        return;
    }

    int store_history = jp_json_get_bool(request->content, "store_history", 1);
    if (store_history) execution_count++;

    publish_status(socks->iopub, request, "busy");
    publish_execute_input(socks->iopub, request, code, execution_count);
    struct exec_result result = execute_cells(socks, request, &cells, run_index);
    send_execute_reply(socks->shell, request, &result, run_index);
    publish_status(socks->iopub, request, "idle");

    exec_result_free(&result);
    free(code);
    cells_free(&cells);
}

static void handle_shell(struct kernel_sockets *socks) {
    struct jp_message request;
    if (jp_recv_message(socks->shell, &request, 0) == -1) return;

    if (strcmp(request.msg_type, "kernel_info_request") == 0) {
        jp_send_message(socks->shell, &request, kernel_session, "kernel_info_reply", "{}",
            "{\"status\":\"ok\",\"protocol_version\":\"5.5\",\"implementation\":\"ZMQBook C\",\"implementation_version\":\"0.1\",\"language_info\":{\"name\":\"c\",\"version\":\"c11\",\"mimetype\":\"text/x-csrc\",\"file_extension\":\".c\"},\"banner\":\"ZMQBook C educational kernel\"}");
    } else if (strcmp(request.msg_type, "execute_request") == 0) {
        handle_execute(socks, &request);
    } else if (strcmp(request.msg_type, "shutdown_request") == 0) {
        running = 0;
        jp_send_message(socks->shell, &request, kernel_session, "shutdown_reply", "{}",
            "{\"status\":\"ok\",\"restart\":false}");
    } else {
        jp_send_message(socks->shell, &request, kernel_session, "error", "{}",
            "{\"status\":\"error\",\"ename\":\"UnsupportedMessage\",\"evalue\":\"unsupported shell message\"}");
    }

    jp_msg_free(&request);
}

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    ensure_dirs();
    jp_write_connection_file("data/kernel-connection.json");

    void *context = zmq_ctx_new();
    if (!context) {
        fprintf(stderr, "zmq_ctx_new failed: %s\n", zmq_strerror(zmq_errno()));
        return 1;
    }

    struct kernel_sockets socks = {0};
    socks.shell = zmq_socket(context, ZMQ_ROUTER);
    socks.iopub = zmq_socket(context, ZMQ_PUB);
    socks.stdin_sock = zmq_socket(context, ZMQ_ROUTER);
    socks.control = zmq_socket(context, ZMQ_ROUTER);
    socks.heartbeat = zmq_socket(context, ZMQ_REP);
    if (!socks.shell || !socks.iopub || !socks.stdin_sock || !socks.control || !socks.heartbeat) {
        fprintf(stderr, "zmq_socket failed: %s\n", zmq_strerror(zmq_errno()));
        return 1;
    }

    int linger = 0;
    zmq_setsockopt(socks.shell, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_setsockopt(socks.iopub, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_setsockopt(socks.stdin_sock, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_setsockopt(socks.control, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_setsockopt(socks.heartbeat, ZMQ_LINGER, &linger, sizeof(linger));

    if (bind_socket(socks.shell, JP_SHELL_PORT, "shell") == -1 ||
        bind_socket(socks.iopub, JP_IOPUB_PORT, "iopub") == -1 ||
        bind_socket(socks.stdin_sock, JP_STDIN_PORT, "stdin") == -1 ||
        bind_socket(socks.control, JP_CONTROL_PORT, "control") == -1 ||
        bind_socket(socks.heartbeat, JP_HB_PORT, "heartbeat") == -1) {
        return 1;
    }

    printf("ZMQBook C kernel running with Jupyter-style sockets\n");
    printf("  Shell ROUTER:    tcp://127.0.0.1:%d\n", JP_SHELL_PORT);
    printf("  IOPub PUB:       tcp://127.0.0.1:%d\n", JP_IOPUB_PORT);
    printf("  Stdin ROUTER:    tcp://127.0.0.1:%d\n", JP_STDIN_PORT);
    printf("  Control ROUTER:  tcp://127.0.0.1:%d\n", JP_CONTROL_PORT);
    printf("  Heartbeat REP:   tcp://127.0.0.1:%d\n", JP_HB_PORT);
    printf("  Connection file: data/kernel-connection.json\n");
    fflush(stdout);

    while (running) {
        zmq_pollitem_t items[] = {
            {socks.shell, 0, ZMQ_POLLIN, 0},
            {socks.control, 0, ZMQ_POLLIN, 0},
            {socks.heartbeat, 0, ZMQ_POLLIN, 0}
        };
        int rc = zmq_poll(items, 3, 250);
        if (rc == -1) {
            if (zmq_errno() == EINTR) continue;
            fprintf(stderr, "kernel poll failed: %s\n", zmq_strerror(zmq_errno()));
            break;
        }
        if (items[2].revents & ZMQ_POLLIN) send_heartbeat_reply(socks.heartbeat);
        if (items[1].revents & ZMQ_POLLIN) {
            int interrupted = 0;
            handle_control_once(&socks, -1, &interrupted);
        }
        if (items[0].revents & ZMQ_POLLIN) handle_shell(&socks);
    }

    zmq_close(socks.shell);
    zmq_close(socks.iopub);
    zmq_close(socks.stdin_sock);
    zmq_close(socks.control);
    zmq_close(socks.heartbeat);
    zmq_ctx_destroy(context);
    return 0;
}
