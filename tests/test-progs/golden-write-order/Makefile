CROSS_COMPILE ?= riscv64-linux-gnu-
BUILD_DIR ?= build
ROUNDS ?= 32
DELAY ?= 0
COUNTER_ROUNDS ?= 1000
AM_LIB = $(AM_HOME)/am/build/am-riscv64-xs-dual.a
KLIB = $(AM_HOME)/libs/klib/build/klib-riscv64-xs-dual.a
CFLAGS = -O2 -Wall -Werror -std=gnu11 -ffreestanding -fno-builtin \
         -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables \
         -ffunction-sections -fdata-sections -march=rv64gc -mcmodel=medany
CASES = double-add add-swap add-store concurrent-add zero-vl-store

.PHONY: all check-env clean
all: $(addprefix $(BUILD_DIR)/,$(addsuffix .bin,$(CASES)))

check-env:
	@test -n "$(AM_HOME)" || { echo "Set AM_HOME to a nexus-am checkout"; exit 1; }
	@test -r "$(AM_LIB)" -a -r "$(KLIB)" || \
	    { echo "Build the riscv64-xs-dual AM and klib archives first"; exit 1; }

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/double-add.o: DEFINES = -DMODE=0
$(BUILD_DIR)/add-swap.o: DEFINES = -DMODE=1
$(BUILD_DIR)/add-store.o: DEFINES = -DMODE=2

$(BUILD_DIR)/concurrent-add.o: concurrent.c | check-env $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $(CFLAGS) -DROUNDS=$(COUNTER_ROUNDS) -c $< -o $@

$(BUILD_DIR)/zero-vl-store.o: zero-vl-store.c | check-env $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: main.c | check-env $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $(CFLAGS) $(DEFINES) -DROUNDS=$(ROUNDS) -DDELAY=$(DELAY) -c $< -o $@

$(BUILD_DIR)/%.elf: $(BUILD_DIR)/%.o
	$(CROSS_COMPILE)ld -melf64lriscv --gc-sections \
	    -L $(AM_HOME)/am/src/xs/ldscript \
	    -T $(AM_HOME)/am/src/nemu/isa/riscv/boot/loader64.ld \
	    -o $@ --start-group $< $(AM_LIB) $(KLIB) --end-group

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf
	$(CROSS_COMPILE)objcopy -S --set-section-flags .bss=alloc,contents -O binary $< $@

clean:
	rm -rf $(BUILD_DIR)
