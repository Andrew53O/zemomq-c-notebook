CC ?= gcc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L
ZMQ_CFLAGS := $(shell $(PKG_CONFIG) --cflags libzmq)
ZMQ_LIBS := $(shell $(PKG_CONFIG) --libs libzmq)

BUILD_DIR := build
BINARIES := server broker kernel_worker pair_signal_demo zero_copy_demo transport_bridge_demo

.PHONY: all clean check-zmq

all: check-zmq $(addprefix $(BUILD_DIR)/,$(BINARIES))

check-zmq:
	@$(PKG_CONFIG) --exists libzmq || (echo "libzmq not found. Install with: sudo apt install libzmq3-dev pkg-config" && exit 1)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/server: src/server.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(ZMQ_CFLAGS) $< -o $@ $(ZMQ_LIBS) -pthread

$(BUILD_DIR)/broker: src/broker.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(ZMQ_CFLAGS) $< -o $@ $(ZMQ_LIBS) -pthread

$(BUILD_DIR)/kernel_worker: src/kernel_worker.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(ZMQ_CFLAGS) $< -o $@ $(ZMQ_LIBS) -pthread

$(BUILD_DIR)/pair_signal_demo: src/pair_signal_demo.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(ZMQ_CFLAGS) $< -o $@ $(ZMQ_LIBS) -pthread

$(BUILD_DIR)/zero_copy_demo: src/zero_copy_demo.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(ZMQ_CFLAGS) $< -o $@ $(ZMQ_LIBS) -pthread

$(BUILD_DIR)/transport_bridge_demo: src/transport_bridge_demo.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(ZMQ_CFLAGS) $< -o $@ $(ZMQ_LIBS) -pthread

clean:
	rm -rf $(BUILD_DIR) runtime
