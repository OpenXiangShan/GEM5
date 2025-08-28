# Makefile for Issue Queue RISC-V Tests (KISS principle)

# 工具和选项
CC = riscv64-unknown-linux-gnu-gcc
OBJDUMP = riscv64-unknown-linux-gnu-objdump
CFLAGS = -O2 -static -g -fno-omit-frame-pointer
BUILD_DIR = build

# 自动发现源文件
C_SOURCES = $(wildcard *.c)
S_SOURCES = $(wildcard *.s)
C_TARGETS = $(C_SOURCES:%.c=$(BUILD_DIR)/%)
S_TARGETS = $(S_SOURCES:%.s=$(BUILD_DIR)/%)

# 默认目标
all: $(BUILD_DIR) $(C_TARGETS) $(S_TARGETS)

# 创建build目录
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# C程序编译规则
$(BUILD_DIR)/%: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $<

# 汇编程序编译规则
$(BUILD_DIR)/%: %.s | $(BUILD_DIR)
	$(CC) $(CFLAGS) -nostdlib -nostartfiles -Ttext=0x10000 -o $@ $<

# 反汇编规则
$(BUILD_DIR)/%.dump: $(BUILD_DIR)/%
	$(OBJDUMP) -S $< > $@

# 生成所有反汇编文件
dumps: $(C_TARGETS:=.dump) $(S_TARGETS:=.dump)

# gem5测试 (可选)
GEM5_BIN = ../build/RISCV/gem5.opt
GEM5_CONFIG = ../configs/example/se.py
DEBUG_FLAGS ?=
DEBUG_DIR = ./output


# 清理
clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(DEBUG_DIR)

# 构建debug参数
DEBUG_ARGS = $(if $(DEBUG_FLAGS),--debug-flags=$(DEBUG_FLAGS) --debug-file=$*.all)

# 测试目标 - 自动发现build目录中的程序
run-%: $(BUILD_DIR)/%
	@echo "运行测试: $<"
	mkdir -p $(DEBUG_DIR)/$*
	$(GEM5_BIN) $(DEBUG_ARGS) -d $(DEBUG_DIR)/$* $(GEM5_CONFIG) --ideal-kmhv3 -c $<

# 帮助
help:
	@echo "使用方法:"
	@echo "  make         - 编译所有程序到build/目录"
	@echo "  make clean   - 清理build/目录"
	@echo "  make dumps   - 生成所有程序的反汇编文件(.dump)"
	@echo "  make run-<program> - 运行指定程序"
	@echo "  make run-<program> DEBUG_FLAGS=Schedule - 运行程序并输出debug信息"
	@echo ""
	@echo "示例:"
	@echo "  make run-iq_dependency              # 普通运行"
	@echo "  make run-iq_dependency DEBUG_FLAGS=Schedule  # 输出调度debug信息"
	@echo "  make run-simple_tests DEBUG_FLAGS=IEW,O3CPU      # 输出多个debug标志"
	@echo "  make build/alu.dump                  # 生成单个程序的反汇编"
	@echo ""
	@echo "添加新测试: 只需要放.c或.s文件到当前目录即可"

.PHONY: all clean dumps help run-%
