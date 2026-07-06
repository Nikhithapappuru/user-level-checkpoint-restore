CC := gcc
CFLAGS := -D_GNU_SOURCE -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic -no-pie -Iinclude
BIN_DIR := bin
SRC_DIR := src
DEMO_DIR := demo

MANAGER_SRC := $(SRC_DIR)/common.c $(SRC_DIR)/config.c $(SRC_DIR)/procfs.c $(SRC_DIR)/snapshot.c $(SRC_DIR)/runtime.c $(SRC_DIR)/manager.c $(SRC_DIR)/main.c
DEMO_SRC := $(SRC_DIR)/common.c $(SRC_DIR)/config.c $(SRC_DIR)/procfs.c $(SRC_DIR)/snapshot.c $(SRC_DIR)/runtime.c $(DEMO_DIR)/counter_demo.c

.PHONY: all clean

all: $(BIN_DIR)/ulcr_manager $(BIN_DIR)/counter_demo

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/ulcr_manager: $(MANAGER_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(MANAGER_SRC)

$(BIN_DIR)/counter_demo: $(DEMO_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(DEMO_SRC)

clean:
	rm -rf $(BIN_DIR) runtime
