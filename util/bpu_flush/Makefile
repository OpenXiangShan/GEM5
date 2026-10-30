RISCV_PREFIX ?= riscv64-linux-gnu-
CC := $(RISCV_PREFIX)gcc
OBJCOPY := $(RISCV_PREFIX)objcopy

CFLAGS := -march=rv64gc -mabi=lp64 -nostdlib -nostartfiles -static \
	-fno-pic -fno-pie
LDFLAGS := -Wl,-T,fence_i_raw.ld -Wl,--build-id=none

.PHONY: all clean wrong-path

all: fence_i_raw.bin

wrong-path: fence_i_wrong_path.bin

fence_i_raw.elf: fence_i_raw.S fence_i_raw.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

fence_i_raw.bin: fence_i_raw.elf
	$(OBJCOPY) -O binary $< $@

fence_i_wrong_path.elf: fence_i_wrong_path.S fence_i_raw.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

fence_i_wrong_path.bin: fence_i_wrong_path.elf
	$(OBJCOPY) -O binary $< $@

clean:
	$(RM) fence_i_raw.elf fence_i_raw.bin \
		fence_i_wrong_path.elf fence_i_wrong_path.bin
