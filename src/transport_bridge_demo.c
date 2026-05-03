#include <signal.h>
#include <stdio.h>
#include <zmq.h>

static volatile sig_atomic_t running = 1;

static void stop(int sig) {
    (void)sig;
    running = 0;
}

int main(void) {
    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    void *context = zmq_ctx_new();
    void *tcp_side = zmq_socket(context, ZMQ_ROUTER);
    void *ipc_side = zmq_socket(context, ZMQ_DEALER);

    zmq_bind(tcp_side, "tcp://127.0.0.1:7010");
    zmq_bind(ipc_side, "ipc:///tmp/zeromq-notebook-bridge.ipc");

    printf("transport bridge demo\n");
    printf("  tcp side: tcp://127.0.0.1:7010\n");
    printf("  ipc side: ipc:///tmp/zeromq-notebook-bridge.ipc\n");
    printf("This demonstrates a TCP-to-IPC bridge with zmq_proxy(). Press Ctrl-C to stop.\n");
    fflush(stdout);

    while (running) {
        int rc = zmq_proxy(tcp_side, ipc_side, NULL);
        if (rc == -1) {
            break;
        }
    }

    zmq_close(tcp_side);
    zmq_close(ipc_side);
    zmq_ctx_destroy(context);
    return 0;
}
