#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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

static volatile sig_atomic_t running = 1;

struct buffer {
    char *data;
    size_t len;
};

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

static char *read_file(const char *path, size_t *len_out) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
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
    if (len_out) {
        *len_out = read_count;
    }
    return data;
}

static int write_file(const char *path, const char *data, size_t len) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        return -1;
    }
    size_t written = fwrite(data, 1, len, file);
    fclose(file);
    return written == len ? 0 : -1;
}

static const char *mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) {
        return "application/octet-stream";
    }
    if (strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(ext, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(ext, ".js") == 0) return "application/javascript; charset=utf-8";
    if (strcmp(ext, ".json") == 0) return "application/json; charset=utf-8";
    return "application/octet-stream";
}

static void send_response(int client, int status, const char *status_text, const char *type, const char *body, size_t len) {
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
    if (len > 0) {
        send(client, body, len, 0);
    }
}

static void send_json_text(int client, int status, const char *status_text, const char *body) {
    send_response(client, status, status_text, "application/json; charset=utf-8", body, strlen(body));
}

static void send_not_found(int client) {
    send_json_text(client, 404, "Not Found", "{\"ok\":false,\"error\":\"not found\"}");
}

static int ensure_data_dir(void) {
    if (mkdir("data", 0755) == -1 && errno != EEXIST) {
        return -1;
    }
    return 0;
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
            if (result.len >= header_len + content_len) {
                break;
            }
        }
        if (result.len >= REQ_LIMIT) {
            break;
        }
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

static void serve_static(int client, const char *path) {
    char file_path[512];
    if (strcmp(path, "/") == 0) {
        snprintf(file_path, sizeof(file_path), "web/index.html");
    } else if (strstr(path, "..")) {
        send_not_found(client);
        return;
    } else {
        snprintf(file_path, sizeof(file_path), "web%s", path);
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

static char *recv_msg_frames(void *socket) {
    struct buffer reply = {0};
    reply.data = calloc(REQ_LIMIT + 1, 1);
    if (!reply.data) {
        return strdup("{\"ok\":false,\"error\":\"out of memory\"}");
    }

    int more = 0;
    size_t more_size = sizeof(more);
    do {
        zmq_msg_t message;
        if (zmq_msg_init(&message) == -1) {
            free(reply.data);
            return strdup("{\"ok\":false,\"error\":\"failed to initialize ZeroMQ message\"}");
        }
        int n = zmq_msg_recv(&message, socket, 0);
        if (n == -1) {
            zmq_msg_close(&message);
            free(reply.data);
            return strdup("{\"ok\":false,\"error\":\"kernel unavailable or timed out\"}");
        }

        size_t len = zmq_msg_size(&message);
        if (len < REQ_LIMIT) {
            memcpy(reply.data, zmq_msg_data(&message), len);
            reply.data[len] = '\0';
            reply.len = len;
        }

        zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &more_size);
        zmq_msg_close(&message);
    } while (more);

    return reply.data;
}

static char *call_kernel(void *context, const char *body) {
    void *requester = zmq_socket(context, ZMQ_REQ);
    if (!requester) {
        return strdup("{\"ok\":false,\"error\":\"failed to create ZeroMQ request socket\"}");
    }
    int timeout = 7000;
    int linger = 0;
    zmq_setsockopt(requester, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_setsockopt(requester, ZMQ_SNDTIMEO, &timeout, sizeof(timeout));
    zmq_setsockopt(requester, ZMQ_LINGER, &linger, sizeof(linger));
    if (zmq_connect(requester, "tcp://127.0.0.1:7000") == -1) {
        zmq_close(requester);
        return strdup("{\"ok\":false,\"error\":\"failed to connect to ZeroMQ broker\"}");
    }

    if (send_msg_frame(requester, "RUN", ZMQ_SNDMORE) == -1 ||
        send_msg_frame(requester, body, 0) == -1) {
        zmq_close(requester);
        return strdup("{\"ok\":false,\"error\":\"failed to send multipart request to ZeroMQ broker\"}");
    }

    char *reply = recv_msg_frames(requester);
    zmq_close(requester);
    return reply;
}

static void drain_status(void *status_sub) {
    if (!status_sub) {
        return;
    }

    while (1) {
        zmq_pollitem_t item = {status_sub, 0, ZMQ_POLLIN, 0};
        int rc = zmq_poll(&item, 1, 0);
        if (rc <= 0 || !(item.revents & ZMQ_POLLIN)) {
            break;
        }

        char topic[64] = {0};
        char payload[256] = {0};
        int n = zmq_recv(status_sub, topic, sizeof(topic) - 1, 0);
        if (n == -1) break;
        topic[n] = '\0';
        n = zmq_recv(status_sub, payload, sizeof(payload) - 1, 0);
        if (n == -1) break;
        payload[n] = '\0';
        printf("status channel [%s] %s\n", topic, payload);
        fflush(stdout);
    }
}

static void handle_api(int client, const char *method, const char *path, char *body, void *context) {
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/notebook") == 0) {
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

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/notebook/save") == 0) {
        if (ensure_data_dir() == -1 || write_file(NOTEBOOK_PATH, body, strlen(body)) == -1) {
            send_json_text(client, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"failed to save notebook\"}");
            return;
        }
        send_json_text(client, 200, "OK", "{\"ok\":true}");
        return;
    }

    if (strcmp(method, "POST") == 0 &&
        (strcmp(path, "/api/cell/run") == 0 || strcmp(path, "/api/run-all") == 0)) {
        char *reply = call_kernel(context, body);
        send_response(client, 200, "OK", "application/json; charset=utf-8", reply, strlen(reply));
        free(reply);
        return;
    }

    send_not_found(client);
}

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    ensure_data_dir();

    void *context = zmq_ctx_new();
    void *status_sub = zmq_socket(context, ZMQ_SUB);
    if (status_sub) {
        const char *topic = "kernel";
        zmq_setsockopt(status_sub, ZMQ_SUBSCRIBE, topic, strlen(topic));
        if (zmq_bind(status_sub, "tcp://127.0.0.1:7002") == -1) {
            fprintf(stderr, "status SUB bind failed: %s\n", zmq_strerror(zmq_errno()));
            zmq_close(status_sub);
            status_sub = NULL;
        }
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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
    printf("ZeroMQ execution requests go to tcp://127.0.0.1:7000\n");
    printf("ZeroMQ status SUB listens on tcp://127.0.0.1:7002 topic 'kernel'\n");
    fflush(stdout);

    while (running) {
        drain_status(status_sub);
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
            handle_api(client, method, path, request_body(request.data), context);
        } else {
            serve_static(client, path);
        }

        close(client);
        free(request.data);
        drain_status(status_sub);
    }

    close(server_fd);
    if (status_sub) {
        zmq_close(status_sub);
    }
    zmq_ctx_destroy(context);
    return 0;
}
