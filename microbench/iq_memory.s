# Issue Queue Memory Test
# 测试目的: 观察issue queue中的内存操作调度和replay逻辑
# 关键观察点:
# - retryMem() 和 replay() 逻辑
# - markMemDepDone() 如何处理内存依赖
# - Load/Store 指令的调度策略
# - 内存依赖的唤醒机制

.section .data
test_array:
    .word 1, 2, 3, 4, 5, 6, 7, 8, 9, 10

.section .text
.global _start

_start:
    # 获取数组地址
    la x1, test_array

    # 测试1: 简单的Load操作序列 (测试Load port调度)
    lw x2, 0(x1)      # Load 1 - 加载test_array[0]
    lw x3, 4(x1)      # Load 2 - 加载test_array[1] (可能与Load 1竞争port)
    lw x4, 8(x1)      # Load 3 - 加载test_array[2]
    lw x5, 12(x1)     # Load 4 - 加载test_array[3]

    # 测试2: Load后立即使用 (测试Load-Use依赖和唤醒)
    lw x6, 16(x1)     # Load test_array[4]
    add x7, x6, x2    # 立即使用x6 (可能需要stall等待Load完成)
    lw x8, 20(x1)     # Load test_array[5]
    mul x9, x8, x3    # 立即使用x8

    # 测试3: Store操作序列 (测试Store port调度)
    sw x7, 24(x1)     # Store 1 - 存储到test_array[6]
    sw x9, 28(x1)     # Store 2 - 存储到test_array[7]
    sw x2, 32(x1)     # Store 3 - 存储到test_array[8]
    sw x3, 36(x1)     # Store 4 - 存储到test_array[9]

    # 测试4: Load-Store依赖 (可能的内存aliasing)
    lw x10, 0(x1)     # Load from address
    sw x10, 4(x1)     # Store to nearby address (可能有内存依赖)
    lw x11, 4(x1)     # Load from just stored location (应该forward)

    # 测试5: 复杂的Load-ALU-Store链
    lw x12, 8(x1)     # Load
    addi x13, x12, 100 # ALU操作依赖Load结果
    sw x13, 12(x1)    # Store ALU结果
    lw x14, 12(x1)    # Load刚存储的值
    sub x15, x14, x12 # 应该得到100

    # 测试6: 多个内存地址的并发访问
    addi x16, x1, 16  # 计算另一个地址
    lw x17, 0(x1)     # Load from base address
    lw x18, 0(x16)    # Load from offset address (不同cache line)
    add x19, x17, x18 # 使用两个Load结果

    # 测试7: 地址计算依赖 (测试地址生成单元调度)
    add x20, x1, x2   # 动态计算地址 (地址依赖x2)
    lw x21, 0(x20)    # Load使用计算的地址

    sll x22, x3, 2    # 计算偏移 (x3 * 4)
    add x23, x1, x22  # 计算地址
    lw x24, 0(x23)    # Load使用计算的地址

    # 测试8: 条件Load/Store (分支预测影响)
    li x25, 5
    beq x2, x25, load_branch
    lw x26, 16(x1)    # 条件Load 1
    j after_branch
load_branch:
    lw x26, 20(x1)    # 条件Load 2
after_branch:

    # 测试9: 不同大小的内存操作
    lb x27, 0(x1)     # Load byte
    lh x28, 2(x1)     # Load halfword
    lw x29, 4(x1)     # Load word
    sb x27, 8(x1)     # Store byte
    sh x28, 10(x1)    # Store halfword
    sw x29, 12(x1)    # Store word

    # 测试10: 内存屏障效果
    lw x30, 0(x1)     # Load 1
    sw x30, 4(x1)     # Store 1
    fence             # 内存屏障
    lw x31, 8(x1)     # Load 2 (在fence之后)
    sw x31, 12(x1)    # Store 2

    # 结束
    li a0, 0
    li a7, 93
    ecall
