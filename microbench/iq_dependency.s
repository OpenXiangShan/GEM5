# Issue Queue Dependency Chain Test
# 测试目的: 观察issue queue中的依赖唤醒逻辑 (wakeUpDependents)
# 关键观察点:
# - 如何处理RAW依赖
# - 指令在issue queue中的调度顺序
# - wakeup信号的传播

.section .text
.global _start

_start:
    # 设置初始值
    li x1, 100      # x1 = 100
    li x2, 200      # x2 = 200
    li x3, 300      # x3 = 300

    # 测试1: 简单的依赖链 (重点观察wakeup逻辑)
    add x4, x1, x2   # x4 = x1 + x2 (300)
    add x5, x4, x3   # x5 = x4 + x3 (600) - 依赖x4
    add x6, x5, x4   # x6 = x5 + x4 (900) - 依赖x5和x4
    add x7, x6, x5   # x7 = x6 + x5 (1500) - 依赖x6和x5

    # 测试2: 更长的依赖链
    addi x8, x7, 1   # x8 = x7 + 1 (1501)
    sub x9, x8, x1   # x9 = x8 - x1 (1401)
    mul x10, x9, x2  # x10 = x9 * x2 (280200)
    div x11, x10, x3 # x11 = x10 / x3 (934)

    # 测试3: 分支依赖链 (两个独立的计算链)
    add x12, x1, x2  # 独立链1: x12 = x1 + x2 (300)
    add x13, x2, x3  # 独立链2: x13 = x2 + x3 (500)
    add x14, x12, x1 # 链1续: x14 = x12 + x1 (400)
    add x15, x13, x2 # 链2续: x15 = x13 + x2 (700)
    add x16, x14, x15# 汇聚: x16 = x14 + x15 (1100)

    # 测试4: 写后写依赖 (WAW hazard)
    add x17, x1, x2  # x17 = 300
    add x17, x3, x1  # x17 = 400 (覆盖前一个值)
    add x18, x17, x2 # x18 = 600 (依赖最新的x17)

    # 测试5: 立即数操作（减少依赖）
    addi x19, x1, 100  # x19 = 200
    addi x20, x2, 200  # x20 = 400
    addi x21, x3, 300  # x21 = 600
    add x22, x19, x20  # x22 = 600
    add x23, x21, x22  # x23 = 1200

    # 结束程序
    li a0, 0         # 返回值 0
    li a7, 93        # 系统调用号 (exit)
    ecall
