#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <zmq.h>

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    void *context = zmq_ctx_new();
    if (!context) {
        fprintf(stderr, "zmq_ctx_new failed: %s\n", zmq_strerror(zmq_errno()));
        return 1;
    }

    void *frontend = zmq_socket(context, ZMQ_ROUTER);
    void *backend = zmq_socket(context, ZMQ_DEALER);
    if (!frontend || !backend) {
        fprintf(stderr, "zmq_socket failed: %s\n", zmq_strerror(zmq_errno()));
        return 1;
    }

    int linger = 0;
    int hwm = 100;
    zmq_setsockopt(frontend, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_setsockopt(backend, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_setsockopt(frontend, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    zmq_setsockopt(frontend, ZMQ_RCVHWM, &hwm, sizeof(hwm));
    zmq_setsockopt(backend, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    zmq_setsockopt(backend, ZMQ_RCVHWM, &hwm, sizeof(hwm));

    int actual_hwm = 0;
    size_t actual_hwm_size = sizeof(actual_hwm);
    zmq_getsockopt(frontend, ZMQ_SNDHWM, &actual_hwm, &actual_hwm_size);

    if (zmq_bind(frontend, "tcp://127.0.0.1:7000") == -1) {
        fprintf(stderr, "broker frontend bind failed: %s\n", zmq_strerror(zmq_errno()));
        return 1;
    }
    if (zmq_bind(backend, "tcp://127.0.0.1:7001") == -1) {
        fprintf(stderr, "broker backend bind failed: %s\n", zmq_strerror(zmq_errno()));
        return 1;
    }

    printf("ZeroMQ broker running\n");
    printf("  frontend ROUTER: tcp://127.0.0.1:7000\n");
    printf("  backend  DEALER: tcp://127.0.0.1:7001\n");
    printf("  frontend SNDHWM from zmq_getsockopt(): %d\n", actual_hwm);
    printf("Press Ctrl-C to stop.\n");
    fflush(stdout);

    while (running) {
        int rc = zmq_proxy(frontend, backend, NULL);
        if (rc == -1 && zmq_errno() == ETERM) {
            break;
        }
        if (rc == -1) {
            fprintf(stderr, "zmq_proxy stopped: %s\n", zmq_strerror(zmq_errno()));
            break;
        }
    }

    zmq_close(frontend);
    zmq_close(backend);
    zmq_ctx_destroy(context);
    return 0;
}
