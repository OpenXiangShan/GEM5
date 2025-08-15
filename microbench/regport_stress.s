# Register Port Stress Test
# 测试目的: 制造读端口竞争和regCache压力
# 策略:
# 1. 使用大量不同的寄存器，超过regCache容量(28)
# 2. 同时发射多条需要多个读端口的指令
# 3. 观察端口仲裁和regCache替换行为

.section .text
.global _start

_start:
    # 阶段1: 初始化30个寄存器，超过regCache容量(28)
    li x1, 10
    li x2, 20
    li x3, 30
    li x4, 40
    li x5, 50
    li x6, 60
    li x7, 70
    li x8, 80
    li x9, 90
    li x10, 100
    li x11, 110
    li x12, 120
    li x13, 130
    li x14, 140
    li x15, 150
    li x16, 160
    li x17, 170
    li x18, 180
    li x19, 190
    li x20, 200
    li x21, 210
    li x22, 220
    li x23, 230
    li x24, 240
    li x25, 250
    li x26, 260
    li x27, 270
    li x28, 280
    li x29, 290
    li x30, 300

    # 阶段2: 高密度读端口需求 - 每条指令2个读端口，总共24个读端口需求
    # 远超过12个整数读端口的硬件限制，应该触发端口仲裁
    add x1, x2, x3     # 读端口需求: x2, x3
    add x4, x5, x6     # 读端口需求: x5, x6
    add x7, x8, x9     # 读端口需求: x8, x9
    add x10, x11, x12  # 读端口需求: x11, x12
    add x13, x14, x15  # 读端口需求: x14, x15
    add x16, x17, x18  # 读端口需求: x17, x18
    add x19, x20, x21  # 读端口需求: x20, x21
    add x22, x23, x24  # 读端口需求: x23, x24
    add x25, x26, x27  # 读端口需求: x26, x27
    add x28, x29, x30  # 读端口需求: x29, x30
    sub x2, x1, x4     # 读端口需求: x1, x4 (x1应该在regCache中)
    sub x3, x7, x10    # 读端口需求: x7, x10

    # 阶段3: 测试regCache LRU替换
    # 重复访问早期寄存器，应该看到cache命中
    add x31, x1, x1    # 重复读x1，应该cache命中
    add x31, x2, x2    # 重复读x2，应该cache命中
    add x31, x3, x3    # 重复读x3，应该cache命中

    # 阶段4: 强制cache miss
    # 访问新的寄存器组合，应该触发LRU替换
    add x31, x25, x26  # 这些寄存器可能被从cache中替换出去
    add x31, x27, x28
    add x31, x29, x30

    # 再次访问早期寄存器，看是否需要重新从寄存器文件读取
    add x31, x1, x2    # x1,x2可能不在cache中了
    add x31, x3, x4    # x3,x4可能不在cache中了

    # 结束程序
    li a0, 0
    li a7, 93
    ecall
