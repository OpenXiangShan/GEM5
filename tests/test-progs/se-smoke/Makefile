CROSS_COMPILE ?= riscv64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
CFLAGS ?= -O2
LDFLAGS ?= -static

BIN_DIR := bin/riscv/linux
TARGET := $(BIN_DIR)/se-smoke

.PHONY: all clean

all: $(TARGET)

$(TARGET): src/se-smoke.c
	mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)
