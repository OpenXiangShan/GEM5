# Issue Queue Resource Conflict Test
# 测试目的: 观察issue queue中的资源冲突和端口选择逻辑
# 关键观察点:
# - 不同指令类型的port selection
# - selectInst() 如何处理资源冲突
# - functional unit占用情况
# - issueToFu() 的调度策略

.section .text
.global _start

_start:
    # 设置测试数据
    li x1, 1000
    li x2, 2000
    li x3, 3000
    li x4, 4000

    # 测试1: 大量ALU操作同时竞争 (测试ALU port selection)
    add x5, x1, x2    # ALU操作1
    add x6, x2, x3    # ALU操作2 (可能与上面竞争同一个port)
    add x7, x3, x4    # ALU操作3
    add x8, x4, x1    # ALU操作4
    sub x9, x5, x6    # ALU操作5 (还依赖之前的结果)
    sub x10, x7, x8   # ALU操作6

    # 测试2: 乘法操作竞争 (测试MUL functional unit)
    mul x11, x1, x2   # 乘法1 (通常占用专门的MUL unit)
    mul x12, x3, x4   # 乘法2 (可能需要等待MUL unit释放)
    mul x13, x5, x6   # 乘法3 (依赖前面的ALU结果)

    # 测试3: 除法操作序列 (测试DIV functional unit)
    div x14, x11, x1  # 除法1 (DIV unit通常latency很高)
    div x15, x12, x2  # 除法2 (需要等待DIV unit)
    rem x16, x13, x3  # 取余操作 (也使用DIV unit)

    # 测试4: 混合操作类型 (测试不同FU的调度)
    add x17, x1, x2   # ALU
    mul x18, x2, x3   # MUL
    div x19, x4, x1   # DIV
    sub x20, x3, x4   # ALU (与第一个add可能竞争port)

    # 测试5: 立即数vs寄存器操作 (可能使用不同的port)
    addi x21, x1, 100  # 立即数加法
    add x22, x1, x2    # 寄存器加法
    slli x23, x3, 2    # 立即数左移
    sll x24, x3, x2    # 寄存器左移

    # 测试6: 逻辑操作批量 (测试逻辑运算port)
    and x25, x1, x2    # 逻辑与1
    or x26, x2, x3     # 逻辑或1
    xor x27, x3, x4    # 逻辑异或1
    and x28, x4, x1    # 逻辑与2
    or x29, x25, x26   # 逻辑或2 (依赖前面结果)
    xor x30, x27, x28  # 逻辑异或2

    # 测试7: 条件操作 (分支预测和资源分配)
    beq x1, x2, skip1   # 分支1 (不会跳转)
    add x31, x1, x2     # 应该执行
skip1:
    bne x2, x3, skip2   # 分支2 (会跳转)
    sub x31, x2, x3     # 不应该执行
skip2:

    # 测试8: 复杂依赖+资源冲突组合
    mul x1, x17, x18   # 使用前面计算的结果，测试依赖+资源冲突
    add x2, x19, x20   #
    div x3, x21, x22   #
    sub x4, x23, x24   #

    # 结束
    li a0, 0
    li a7, 93
    ecall
