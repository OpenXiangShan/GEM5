
#include "common.h"

void __attribute__ ((noinline)) ifuwidth(int cnt) {
    // ... existing code ...

    #define ONE \
        "add t1, t2, t3\n\t"     /* 整数加法 1 */ \
        "add t4, t2, t3\n\t"     /* 整数加法 2 */ \
        "add t5, t2, t3\n\t"     /* 整数加法 3 */ \
        "add t6, t2, t3\n\t"     /* 整数加法 4 */ \
        "fadd.d f2, f0, f1\n\t"  /* 浮点加法 1 */ \
        "fadd.d f3, f0, f1\n\t"  /* 浮点加法 2 */ \
        "fadd.d f4, f0, f1\n\t"  /* 浮点加法 3 */ \
        "fadd.d f5, f0, f1\n\t"  /* 浮点加法 4 */

    asm volatile(
        // 初始化整数寄存器
        "li t2, 1\n\t"
        "li t3, 2\n\t"
        // 初始化浮点寄存器
        "fmv.d.x f0, t2\n\t"
        "fmv.d.x f1, t3\n\t"
        ".align 4\n\t"
        "1:\n\t"
        HUNDRED
        "addi %0, %0, -1\n\t"
        "bnez %0, 1b\n\t"
        :
        :"r"(cnt)
        :"t1", "t2", "t3", "t4", "t5", "t6",
         "f0", "f1", "f2", "f3", "f4", "f5", "memory");
}

// 避免退出后仍然执行复杂的函数，导致性能下降
static inline void fast_exit(int code) {
  asm volatile("li a7, 93\n\t"
               "mv a0, %0\n\t"
               "ecall"
               :
               : "r"(code)
               : "a0", "a7");
  __builtin_unreachable();
}

// main 之前也会执行复杂函数来初始化，通过se.py 中 warmup_insts_no_switch
// warmup 一段指令后再统计来规避
int main() {
  ifuwidth(1000);
  fast_exit(0);
}
