/**
 * @file lowlevel_fpu_probe.c
 * @brief Cortex-M4F/M7F 上下文寄存器专用测试任务。
 *
 * 不能用纯 C 表达“指定 S16-S31 持续驻留且不被编译器溢出到内存”的测试。
 * naked 入口仅含系统测试汇编；C 函数负责调度、整数逐位比较与结果发布。
 * expected 只加载一次，之后每轮真实切换再将仍驻留的寄存器保存到 observed。
 */
#include "lowlevel_checks.h"

#include <stddef.h>

#if !defined(__ARM_ARCH_7EM__)
#error "lowlevel_fpu_probe requires the Cortex-M4F/M7F ARMv7E-M port"
#endif

#if !defined(__ARM_FP) || ((__ARM_FP & 4) == 0)
#error "lowlevel_fpu_probe requires single-precision hardware FPU instructions"
#endif

#define LOWLEVEL_FPU_OBSERVED_OFFSET \
    ((uint32_t)offsetof(lowlevel_fpu_context_t, observed))

_Static_assert(offsetof(lowlevel_fpu_context_t, expected) == 0U,
               "expected must be the first context field");
_Static_assert(LOWLEVEL_FPU_OBSERVED_OFFSET == 64U,
               "observed must follow the sixteen expected register words");

/**
 * @brief 用真实高位浮点寄存器驱动检查，而非模拟内存中的浮点局部变量。
 * @param context [in,out] 通过 AAPCS 的 r0 传入，后由 callee-saved r4 保留。
 *
 * 入口 PSP 已由 RTOS 对齐到 8 字节。push 两字仍保持调用 C 函数时的栈对齐。
 * 这里保存的 LR 是任务入口的正常返回地址，不是 Handler 的 EXC_RETURN；
 * 每次 PendSV 自行保存、恢复其 EXC_RETURN。任务不从本入口返回。
 * 所有 C 调用必须遵守 AAPCS，保留 r4 与 S16-S31；BL 覆盖的 LR 不再被复用。
 */
/* 参数只能由汇编读取 r0；naked 中不能插入 C 的 (void)context 语句。 */
__attribute__((naked)) void lowlevel_fpu_probe_task(void *context __attribute__((unused)))
{
    __asm__ __volatile__(
        " push {r4, lr}                        \n" /* 两字保留入口 LR 与 PSP 8 字节对齐 */
        " mov r4, r0                           \n" /* r4 = 独占 context，跨 C 调用保留 */
        " vldmia r4, {s16-s31}                 \n" /* 仅此一次将 expected 装入寄存器 */
        "1:                                    \n"
        " mov r0, r4                           \n" /* r0 = schedule 的 context 参数 */
        " bl lowlevel_fpu_probe_schedule       \n" /* 真正 yield/delay，触发上下文切换 */
        " add.w r1, r4, %[observed]            \n" /* r1 = 该任务独占的 observed 数组 */
        " vstmia r1, {s16-s31}                 \n" /* 保存实际寄存器位模式，不重新加载 */
        " mov r0, r4                           \n" /* r0 = check_round 的 context 参数 */
        " bl lowlevel_fpu_probe_check_round    \n" /* r0 返回继续标志；C 比较原始整数 */
        " cmp r0, #0                           \n" /* 完成或停止请求时退出轮次 */
        " bne 1b                               \n" /* 非零：继续检查同一组驻留模式 */
        " mov r0, r4                           \n" /* r0 = finish 的 context 参数 */
        " bl lowlevel_fpu_probe_finish         \n" /* 发布 done 后停驻，由控制任务删除 */
        "2:                                    \n"
        " b 2b                                 \n" /* finish 不应返回的防御停驻 */
        :
        : [observed] "i"(LOWLEVEL_FPU_OBSERVED_OFFSET)
        : "r0", "r1", "r2", "r3", "r4", "r12", "lr", "cc", "memory",
          "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
          "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15",
          "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
          "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31");
}
