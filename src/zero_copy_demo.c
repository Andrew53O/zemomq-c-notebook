#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zmq.h>

static void release_buffer(void *data, void *hint) {
    (void)hint;
    printf("zero-copy free callback released message buffer\n");
    free(data);
}

int main(void) {
    void *context = zmq_ctx_new();
    void *sender = zmq_socket(context, ZMQ_PAIR);
    void *receiver = zmq_socket(context, ZMQ_PAIR);

    zmq_bind(sender, "inproc://zero-copy");
    zmq_connect(receiver, "inproc://zero-copy");

    char *payload = strdup("message created with zmq_msg_init_data");
    zmq_msg_t message;
    zmq_msg_init_data(&message, payload, strlen(payload), release_buffer, NULL);
    zmq_msg_send(&message, sender, 0);
    zmq_msg_close(&message);

    zmq_msg_t received;
    zmq_msg_init(&received);
    zmq_msg_recv(&received, receiver, 0);
    printf("received: %.*s\n", (int)zmq_msg_size(&received), (char *)zmq_msg_data(&received));
    zmq_msg_close(&received);

    zmq_close(sender);
    zmq_close(receiver);
    zmq_ctx_destroy(context);
    return 0;
}
