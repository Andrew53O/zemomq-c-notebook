#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <zmq.h>

struct thread_args {
    void *context;
};

static void *worker_main(void *arg) {
    struct thread_args *args = (struct thread_args *)arg;
    void *worker = zmq_socket(args->context, ZMQ_PAIR);
    zmq_connect(worker, "inproc://node-control");

    char command[64] = {0};
    int n = zmq_recv(worker, command, sizeof(command) - 1, 0);
    if (n != -1) {
        command[n] = '\0';
        printf("worker thread received PAIR signal: %s\n", command);
        zmq_send(worker, "worker-ack", strlen("worker-ack"), 0);
    }

    zmq_close(worker);
    return NULL;
}

int main(void) {
    void *context = zmq_ctx_new();
    void *controller = zmq_socket(context, ZMQ_PAIR);
    zmq_bind(controller, "inproc://node-control");

    pthread_t worker;
    struct thread_args args = {context};
    pthread_create(&worker, NULL, worker_main, &args);

    zmq_send(controller, "start-node", strlen("start-node"), 0);

    char reply[64] = {0};
    int n = zmq_recv(controller, reply, sizeof(reply) - 1, 0);
    if (n != -1) {
        reply[n] = '\0';
        printf("main thread received coordination reply: %s\n", reply);
    }

    pthread_join(worker, NULL);
    zmq_close(controller);
    zmq_ctx_destroy(context);
    return 0;
}
