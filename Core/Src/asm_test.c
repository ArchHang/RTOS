#include "asm_test.h"

/*
 * naked 纯汇编: C 无局部变量、无 prologue/epilogue, 栈帧由调用者维护;
 * 按 AAPCS 约定 r0 为首个入参兼返回值寄存器, lr 为返回地址。
 */

/**
 * @brief 验证编译器将首个入参装入 r0, 并经 r0 返回结果。
 *
 * @note 输入加 1 后返回; 返回值等于入参 +1, 证明 "首入参在 r0、
 *       返回值在 r0" 的 AAPCS 约定。
 *
 * @param value [in] 输入值
 * @return value + 1
 */
NAKED uint32_t asm_increment(uint32_t value)
{
    __asm volatile(
        "add r0, r0, #1\n"   /* r0 = r0 + 1: 入参与返回值同寄存器 */
        "bx lr\n"            /* 返回调用者 */
    );
}

/**
 * @brief 验证首个入参在 r0, 且调用 C 函数时 r0 兼作首个实参。
 *
 * @note 将 r0 加 4 后调用 printf; 输出从原字符串第 4 字符开始,
 *       证明 printf 按 r0 中的指针取格式串。
 *
 * @param fmt [in] 格式化字符串(调用点传入至少 4 字节)
 */
NAKED void asm_printf(const char *fmt)
{
    __asm volatile(
        "push {lr}\n"         /* 保护返回地址: bl 会覆盖 lr */
        "add r0, r0, #4\n"   /* r0 = fmt + 4: 首入参在 r0 */
        "bl printf\n"        /* printf(fmt + 4): r0 兼作其首实参 */
        "pop {lr}\n"         /* 恢复返回地址 */
        "bx lr\n"
    );
}

/**
 * @brief 验证 AAPCS 入参传递: 前 4 个参数经 r0-r3, 第 5 个压栈。
 *
 * @note 返回 r0+r1+r2+r3+[sp](第 5 参), 结果与五行和相等即证明约定成立。
 *
 * @param arg0 [in] 第 1 参(r0)
 * @param arg1 [in] 第 2 参(r1)
 * @param arg2 [in] 第 3 参(r2)
 * @param arg3 [in] 第 4 参(r3)
 * @param arg4 [in] 第 5 参(栈上, 调用者压入)
 * @return 五个参数之和
 */
NAKED uint32_t asm_pass_args(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4)
{
    __asm volatile(
        "add r0, r0, r1\n"    /* r0 = arg0 + arg1 */
        "add r0, r0, r2\n"    /* r0 += arg2 */
        "add r0, r0, r3\n"    /* r0 += arg3 */
        "push {r4}\n"         /* r4 为 callee-saved, 借用前先保存 */
        "ldr r4, [sp, #4]\n"  /* r4 = 栈上第 5 参(sp+4: 跳过刚压入的 r4) */
        "add r0, r0, r4\n"    /* r0 += arg4 */
        "pop {r4}\n"          /* 归还 r4 */
        "bx lr\n"
    );
}
