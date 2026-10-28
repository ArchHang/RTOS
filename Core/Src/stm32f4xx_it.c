/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f4xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f4xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>        /* fault_output 默认实现: printf + fflush */
#include "rtos_port.h"
#include "rtos_sched.h"   /* rtos_kernel.current_tcb (故障任务归因) */
#include "rtos_task.h"    /* rtos_tcb_t 完整定义(name/stack_base/...) */
#include "rtos_config.h"  /* RTOS_CONFIG_CHECK_FOR_STACK_OVERFLOW */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
/* 向量表指向的 4 个故障 Handler 声明为 naked(属性随前置声明作用于后文定义):
 *
 * 为什么必须 naked —— 序言对故障帧定位的破坏:
 *   硬件在进入 Handler 前把异常帧压到"故障时活跃的栈"(EXC_RETURN.bit2
 *   决定 PSP/MSP), 而 Handler 永远运行在 MSP 上, C 序言的 push 只作用于
 *   MSP。任务故障(帧在 PSP)不受影响; 但 ISR 内故障(帧在 MSP)时, 序言
 *   会把 MSP 再压低若干字节(-O3 实测 36 字节), 此时 MRS 读到的 MSP 比
 *   硬件帧基低, 帧解析整体错位。naked 保证零序言/零尾声, 第一条指令即
 *   采集代码, MSP/PSP 读数均为精确帧基。
 *
 * naked 的约束: 函数体只能含纯 basic asm(无 C 局部变量/无扩展 asm 操作数),
 * 因此采集代码以纯汇编编写, r0/r1 按 AAPCS 传参, 尾跳转进入 fault_report。 */
void HardFault_Handler(void)  __attribute__((naked));
void MemManage_Handler(void)  __attribute__((naked));
void BusFault_Handler(void)   __attribute__((naked));
void UsageFault_Handler(void) __attribute__((naked));
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* ==================================================================== */
/*          HardFault 故障诊断模块 (设计文档: docs/hardfault_debug.md)    */
/*                                                                      */
/* 采集: EXC_RETURN 判 PSP/MSP → 硬件压栈帧(R0-R3/R12/LR/PC/xPSR)       */
/* 解码: CFSR(MMFSR/BFSR/UFSR)/HFSR/MMAR/BFAR 逐位翻译                  */
/* 归因: 线程模式故障 → rtos_kernel.current_tcb 任务名/优先级/栈边界    */
/* 输出: fault_output 弱函数(默认 USART2 寄存器直写, 用户可覆盖)        */
/* 防护: 嵌套故障自报告(故障中再故障不级联 LOCKUP)                      */
/* ==================================================================== */

/* ---------- 异常入口硬件压栈帧(基本帧 8 字, ARMv7-M 固定布局) ---------- */
typedef struct
{
    uint32_t r0;      /* sp+0x00 */
    uint32_t r1;      /* sp+0x04 */
    uint32_t r2;      /* sp+0x08 */
    uint32_t r3;      /* sp+0x0C */
    uint32_t r12;     /* sp+0x10 */
    uint32_t lr;      /* sp+0x14 ★ 调用者返回地址 */
    uint32_t pc;      /* sp+0x18 ★ 出错指令(precise)/附近(imprecise) */
    uint32_t xpsr;    /* sp+0x1C   bit24=T位, bit[7:0]=IPSR */
} fault_frame_t;

/* ---------- 故障现场快照(全局 volatile: 调试器/Live Expressions 直接看) ---------- */
volatile struct
{
    uint32_t      cfsr, hfsr, shcsr, ccr;
    uint32_t      mmfar, bfar;
    uint32_t      sp, exc_return;
    fault_frame_t frame;
    uint8_t       frame_valid;
} g_fault;

/* ---------- 原始输出通道: USART2 寄存器轮询直写(零库依赖) ----------
 * 直接操作 USART2->SR/DR, 不经 newlib/HAL/中断 —— 这是故障上下文中
 * 唯一安全的输出方式。TXE 等待有上限(时钟异常时不至于死等)。
 * 末尾冲刷(TC 等待)由 fault_report 收尾处完成, 保证停机前最后一位
 * 字节移出移位寄存器。 */
#define FAULT_UART      USART2
#define FAULT_TXE_GUARD 100000U

static void fault_raw_putc(char c)
{
    uint32_t guard = FAULT_TXE_GUARD;
    while (((FAULT_UART->SR & USART_SR_TXE) == 0U) && (--guard != 0U)) {
        /* 等发送数据寄存器空 */
    }
    FAULT_UART->DR = (uint8_t)c;
}

static void fault_raw_puts(const char *s)
{
    while (*s != '\0') {
        fault_raw_putc(*s++);
    }
}

static void fault_raw_puthex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    fault_raw_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        fault_raw_putc(hex[(v >> i) & 0xFU]);
    }
}

/* ---------- 故障文本输出口: 弱函数, 用户可重定义 ----------
 * fault_output 是诊断模块唯一的文本出口, 所有报告内容都经它送出。
 *
 * 默认实现: printf + fflush(stdout)。
 *   - fflush 逐段强制刷新, 规避 stdout 行缓冲在停机时丢失尾部无换行
 *     输出的问题(2026-08 实测教训, 见 docs/hardfault_debug.md 5.1 节);
 *   - 前提: CCR.UNALIGN_TRP 已关闭(main.c)——库代码的非对齐字访问
 *     优化不再构成故障源。若重新开启该陷阱, 请覆盖本函数(见下)。
 *
 * 残余风险(可接受): 故障若恰好发生在 UART/HAL/stdio 内部, 本通道可能
 * 失效。此时嵌套保护(fault_report 入口)自动切换到零依赖的
 * fault_raw_* 寄存器直写通道报告嵌套故障, 系统不会进入 LOCKUP。
 *
 * 用户覆盖方式: 在任意 .c 定义同名强符号(需先声明原型):
 *     void fault_output(const char *str);
 * 可选通道:
 *   - fault_raw_puts: 本文件提供的 USART2 寄存器直写(最鲁棒);
 *   - ITM/SWO:        while (*str) ITM_SendChar(*str++);
 *   - 其他 UART:      仿照 fault_raw_puts 修改寄存器基地址;
 *   - RAM 黑匣子:     拷入 .noinit 缓冲, 复位后回放。 */
__attribute__((weak)) void fault_output(const char *str)
{
    (void)printf("%s", str);
    (void)fflush(stdout);
}

/* 内部统一出口(便于将来增加过滤/缓冲层) */
static void fault_puts(const char *s)
{
    fault_output(s);
}

/* 32 位十六进制打印: 逐字符输出, 无局部缓冲拼接。
 *
 * ★ 为什么禁止缓冲拼接(2026-08 真机事故第二轮根因):
 *   -O3 的 store-merging 会把连续字节写合并为整字存储, 本工程实测
 *   生成 "str.w r2,[sp,#10]" —— 地址恒为 2 mod 4 的非对齐写。这在
 *   Cortex-M3/M4/M7 上是架构支持的合法操作(GCC 依赖之), 但若用户
 *   开启 CCR.UNALIGN_TRP, 该指令在故障处理内再触发 UNALIGNED
 *   UsageFault, 同级无法抢占 → 升级 HardFault(级联)。
 *   本实现每字符"单字节写 + 外部函数调用": 字节写天然无对齐要求,
 *   调用屏障阻止任何合并 —— 无论 CCR 如何配置均安全。 */
static void fault_puthex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    char              c[2];

    c[1] = '\0';
    fault_output("0x");
    for (int i = 28; i >= 0; i -= 4) {
        c[0] = hex[(v >> i) & 0xFU];
        fault_output(c);
    }
}

/* 无符号十进制: 从最高位幂逐位输出(同上, 无缓冲拼接;
 * 位间以调用分隔, 不存在可合并的相邻字节写)。 */
static void fault_putdec(uint32_t v)
{
    char     c[2];
    uint32_t p       = 1000000000U; /* 10^9(uint32 上限 4294967295) */
    uint8_t  started = 0U;

    if (v == 0U) {
        fault_output("0");
        return;
    }
    c[1] = '\0';
    while (p > 0U) {
        uint32_t d = v / p;
        if ((d != 0U) || (started != 0U)) {
            c[0] = (char)('0' + d);
            fault_output(c);
            started = 1U;
        }
        v -= d * p;
        p /= 10U;
    }
}

/* ---------- SP 合法性校验(防止二次异常进 LOCKUP) ----------
 * 读帧之前必须确认: SP 在 RAM 内、留得下 8 字帧、4 字节对齐。
 * 本工程 RAM: 0x20000000 ~ 0x2001FFFF(见 STM32F446RETX_FLASH.ld)。
 * 若压栈失败(MSTKERR/STKERR), SP 可能已指向非法地址。 */
static uint8_t fault_sp_valid(uint32_t sp)
{
    if ((sp < 0x20000000UL) ||
        (sp > (0x20020000UL - sizeof(fault_frame_t)))) {
        return 0U;
    }
    return ((sp & 0x3U) == 0U) ? 1U : 0U;
}

/* ---------- CFSR/HFSR 位级解码(仅打印置位位, 每位一行术语+释义) ---------- */

static void fault_decode_mmfsr(uint32_t cfsr)
{
    uint8_t v = (uint8_t)(cfsr & 0xFFU);            /* CFSR bit0~7 */
    if (v == 0U) { return; }
    fault_puts("  MemManage faults:\r\n");
    if (v & (1U << 0)) { fault_puts("    IACCVIOL    instruction fetch from XN/MPU region\r\n"); }
    if (v & (1U << 1)) { fault_puts("    DACCVIOL    data access violates MPU policy\r\n"); }
    if (v & (1U << 4)) { fault_puts("    MSTKERR     error stacking exception frame (stack overflow suspected)\r\n"); }
    if (v & (1U << 5)) { fault_puts("    MUNSTKERR   error unstacking exception frame\r\n"); }
    if (v & (1U << 6)) { fault_puts("    MLSPERR     error during lazy FPU state save\r\n"); }
    if (v & (1U << 7)) { fault_puts("    MMARVALID   MMFAR holds the faulting address\r\n"); }
}

static void fault_decode_bfsr(uint32_t cfsr)
{
    uint8_t v = (uint8_t)((cfsr >> 8) & 0xFFU);   /* CFSR bit8~15 */
    if (v == 0U) { return; }
    fault_puts("  BusFaults:\r\n");
    if (v & (1U << 0)) { fault_puts("    IBUSERR     instruction prefetch bus error\r\n"); }
    if (v & (1U << 1)) { fault_puts("    PRECISERR   precise data bus error, PC and BFAR reliable\r\n"); }
    if (v & (1U << 2)) { fault_puts("    IMPRECISERR imprecise data bus error, PC and BFAR NOT reliable\r\n"); }
    if (v & (1U << 3)) { fault_puts("    UNSTKERR    error unstacking exception frame\r\n"); }
    if (v & (1U << 4)) { fault_puts("    STKERR      error stacking exception frame (stack overflow suspected)\r\n"); }
    if (v & (1U << 5)) { fault_puts("    LSPERR      error during lazy FPU state save\r\n"); }
    if (v & (1U << 7)) { fault_puts("    BFARVALID   BFAR holds the faulting address\r\n"); }
}

static void fault_decode_ufsr(uint32_t cfsr)
{
    uint16_t v = (uint16_t)((cfsr >> 16) & 0xFFFFU); /* CFSR bit16~31 */
    if (v == 0U) { return; }
    fault_puts("  UsageFaults:\r\n");
    if (v & (1U << 0)) { fault_puts("    UNDEFINSTR  undefined instruction executed\r\n"); }
    if (v & (1U << 1)) { fault_puts("    INVSTATE    invalid EPSR state (branch to non-Thumb address)\r\n"); }
    if (v & (1U << 2)) { fault_puts("    INVPC       invalid use of EXC_RETURN\r\n"); }
    if (v & (1U << 3)) { fault_puts("    NOCP        coprocessor (FPU) access with CP10/CP11 disabled\r\n"); }
    if (v & (1U << 8)) { fault_puts("    UNALIGNED   unaligned access with always-faulting instruction (LDRD/LDM/STM)\r\n"); }
    if (v & (1U << 9)) { fault_puts("    DIVBYZERO   integer division by zero\r\n"); }
}

static void fault_decode_hfsr(uint32_t hfsr)
{
    if (hfsr & (1UL << 30)) { fault_puts("    FORCED      escalated from configurable fault, root cause in CFSR\r\n"); }
    if (hfsr & (1UL << 1))  { fault_puts("    VECTTBL     bus error on vector table read\r\n"); }
    if (hfsr & (1UL << 31)) { fault_puts("    DEBUGEVT    debug event escalated to HardFault\r\n"); }
}

/* ---------- 嵌套故障保护(防级联 LOCKUP) ----------
 * 正常流程 fault_report 只会进入一次。若报告路径自身发生故障
 * (如 fault_output 覆盖实现内部出错), 异常会在故障 ACTIVE 期间再次
 * 进入 fault_report: 此时改用零依赖的 fault_raw_* 通道最小化报告并
 * 停机 —— 不覆盖 g_fault(保留首次现场), 后续代码零库依赖, 不会触发
 * 第三次故障, 系统不进 LOCKUP, 调试器保持连接。 */
static volatile uint8_t g_fault_active = 0U;

/* ---------- 报告分段(每段职责单一, 输出结构见各函数头) ---------- */

/* 异常上下文: 模式 / 帧位置 / 帧类型 / 帧基有效性 */
static void report_context(uint32_t exc_return, uint32_t sp, uint8_t frame_valid)
{
    uint8_t from_psp  = ((exc_return & (1UL << 2)) != 0U) ? 1U : 0U;
    uint8_t from_thr  = ((exc_return & (1UL << 3)) != 0U) ? 1U : 0U;
    uint8_t ext_frame = ((exc_return & (1UL << 4)) == 0U) ? 1U : 0U;

    fault_puts("--- Exception context ---\r\n");
    fault_puts("EXC_RETURN = ");  fault_puthex32(exc_return);
    fault_puts("\r\nMode       : ");
    fault_puts(from_thr ? "Thread (task code)\r\n" : "Handler (interrupt code)\r\n");
    fault_puts("Stack frame: ");
    fault_puts(from_psp ? "PSP" : "MSP");
    fault_puts(ext_frame ? ", extended frame (26 words, FPU)\r\n"
                           : ", basic frame (8 words)\r\n");
    fault_puts("Frame SP   = ");  fault_puthex32(sp);
    if (frame_valid != 0U) {
        fault_puts(" (valid)\r\n");
    }
    else
    {
        fault_puts(" (INVALID: outside RAM or misaligned)\r\n");
        fault_puts("Exception frame not reliable - register analysis skipped.\r\n");
    }
    fault_puts("\r\n");
}

/* 硬件保存的寄存器组(异常帧)。参数为 volatile: g_fault 整体 volatile。 */
static void report_frame(volatile const fault_frame_t *frame)
{
    fault_puts("--- Saved registers (exception frame) ---\r\n");
    fault_puts("PC   = ");  fault_puthex32(frame->pc);
    fault_puts("    faulting instruction (reliable on precise faults)\r\n");
    fault_puts("LR   = ");  fault_puthex32(frame->lr);
    /* LR 为 caller-saved: 仅故障在被调函数内时才是有效调用点,
     * 任务顶层故障时为残留值 —— 详见 docs/hardfault_debug.md 2.2 节 */
    fault_puts("    return address (valid only if fault is inside a callee)\r\n");
    fault_puts("R0   = ");  fault_puthex32(frame->r0);
    fault_puts("    R1  = "); fault_puthex32(frame->r1); fault_puts("\r\n");
    fault_puts("R2   = ");  fault_puthex32(frame->r2);
    fault_puts("    R3  = "); fault_puthex32(frame->r3); fault_puts("\r\n");
    fault_puts("R12  = ");  fault_puthex32(frame->r12); fault_puts("\r\n");
    fault_puts("xPSR = ");  fault_puthex32(frame->xpsr);

    uint32_t ipsr = frame->xpsr & 0xFFU;
    fault_puts("    IPSR = ");
    fault_putdec(ipsr);
    if (ipsr == 0U) {
        fault_puts(" (Thread)");
    }
    else
    {
        fault_puts(" (Exception ");
        fault_putdec(ipsr);
        if (ipsr >= 16U) {
            fault_puts(", IRQ ");
            fault_putdec(ipsr - 16U);
        }
        fault_puts(")");
    }
    fault_puts("\r\n");

    if ((frame->xpsr & (1UL << 24)) == 0U) {
        fault_puts("NOTE: EPSR.T = 0, non-Thumb state (see UFSR.INVSTATE)\r\n");
    }
    fault_puts("\r\n");
}

/* RTOS 任务归因与任务栈边界判定(仅线程模式故障有效) */
static void report_task(uint32_t sp)
{
    rtos_tcb_t *tcb = rtos_kernel.current_tcb;

    if (tcb == NULL) {
        return;
    }
    fault_puts("--- RTOS task ---\r\n");
    fault_puts("Task       : \"");
    fault_puts(tcb->name);
    fault_puts("\" (priority ");
    fault_putdec(tcb->priority);
    fault_puts(", id ");
    fault_putdec(tcb->task_id);
    fault_puts(")\r\n");

    /* 栈向下生长, 合法区间 [stack_base, stack_base + stack_size*4);
     * stack_size 单位为字(×4 才是字节)。 */
    fault_puts("Task stack : base ");
    fault_puthex32((uint32_t)tcb->stack_base);
    fault_puts(", size ");
    fault_puthex32(tcb->stack_size * 4U);
    fault_puts(" bytes\r\n");

    if (sp < (uint32_t)tcb->stack_base) {
        fault_puts("OVERFLOW   : SP is ");
        fault_puthex32((uint32_t)tcb->stack_base - sp);
        fault_puts(" bytes below stack base\r\n");
    }
    else
    {
        fault_puts("Stack usage: ");
        fault_puthex32(sp - (uint32_t)tcb->stack_base);
        fault_puts(" bytes in use at fault\r\n");
    }

#if RTOS_CONFIG_CHECK_FOR_STACK_OVERFLOW
    if (tcb->stack_magic != 0xDEADBEEFU) {
        fault_puts("CORRUPTION : task stack magic destroyed (out-of-bounds write reached TCB)\r\n");
    }
#endif
    fault_puts("\r\n");
}

/* Handler 模式故障: 说明 current_tcb 的归因局限 */
static void report_isr_context(void)
{
    fault_puts("--- RTOS context ---\r\n");
    fault_puts("Fault raised in interrupt context.\r\n");
    fault_puts("current_tcb denotes the interrupted task, not the faulting code.\r\n\r\n");
}

/* 故障状态寄存器: CFSR 分类解码 + 有效地址寄存器 + HFSR/SHCSR */
static void report_status(void)
{
    fault_puts("--- Fault status registers ---\r\n");
    fault_puts("CFSR = ");  fault_puthex32(g_fault.cfsr);
    fault_puts("\r\n");
    fault_decode_mmfsr(g_fault.cfsr);
    fault_decode_bfsr(g_fault.cfsr);
    fault_decode_ufsr(g_fault.cfsr);

    fault_puts("MMFAR = ");
    if ((g_fault.cfsr & (1UL << 7)) != 0U) {
        fault_puthex32(g_fault.mmfar);
        fault_puts(" (valid)\r\n");
    }
    else
    {
        fault_puts("not valid\r\n");
    }
    fault_puts("BFAR  = ");
    if ((g_fault.cfsr & (1UL << 15)) != 0U) {
        fault_puthex32(g_fault.bfar);
        fault_puts(" (valid)\r\n");
    }
    else
    {
        fault_puts("not valid\r\n");
    }

    if (g_fault.hfsr != 0U) {
        fault_puts("HFSR  = ");
        fault_puthex32(g_fault.hfsr);
        fault_puts("\r\n");
        fault_decode_hfsr(g_fault.hfsr);
    }
    fault_puts("SHCSR = ");  fault_puthex32(g_fault.shcsr);
    fault_puts("\r\n\r\n");
}

/* ---------- 主分析函数(运行于 Handler 模式/MSP, 全部中断已被冻结) ----------
 * used 属性: 本函数仅被 naked Handler 内的汇编 "b fault_report" 引用,
 * GCC 不解析 basic asm 内容, 会误判"未使用"而不生成符号 → 必须强制保留。 */
static __attribute__((used)) void fault_report(uint32_t sp, uint32_t exc_return)
{
    if (g_fault_active != 0U) {
        /* 嵌套故障: 原始通道直报, 不依赖可能正是故障源的 fault_output */
        fault_raw_puts("\r\n*** NESTED FAULT: exception raised inside fault reporter ***\r\n");
        fault_raw_puts("CFSR       = ");  fault_raw_puthex32(SCB->CFSR);
        fault_raw_puts("\r\nHFSR       = ");  fault_raw_puthex32(SCB->HFSR);
        fault_raw_puts("\r\nSP         = ");  fault_raw_puthex32(sp);
        fault_raw_puts("\r\nEXC_RETURN = ");  fault_raw_puthex32(exc_return);
        fault_raw_puts("\r\nPrimary fault context preserved in g_fault.\r\n");
        fault_raw_puts("System halted.\r\n");
        for (;;) {
        }
    }
    g_fault_active = 1U;

    /* 1. 立即快照故障寄存器(只读不清: 状态位写1才清除, 保留现场)。
     *    后续解析代码万一二次异常, 快照里证据仍在, 调试器可查 g_fault。 */
    g_fault.cfsr   = SCB->CFSR;    /* 0xE000ED28 */
    g_fault.hfsr   = SCB->HFSR;    /* 0xE000ED2C */
    g_fault.shcsr  = SCB->SHCSR;   /* 0xE000ED24 */
    g_fault.ccr    = SCB->CCR;     /* 0xE000ED14 */
    g_fault.mmfar  = (g_fault.cfsr & (1UL << 7)) ? SCB->MMFAR : 0xFFFFFFFFU;
    g_fault.bfar   = (g_fault.cfsr & (1UL << 15)) ? SCB->BFAR : 0xFFFFFFFFU;
    g_fault.sp         = sp;
    g_fault.exc_return = exc_return;
    g_fault.frame_valid = fault_sp_valid(sp);

    /* 2. 报告主体 */
    uint8_t from_thr = ((exc_return & (1UL << 3)) != 0U) ? 1U : 0U;

    fault_puts("\r\n\r\n==================== FAULT REPORT ====================\r\n\r\n");

    report_context(exc_return, sp, g_fault.frame_valid);

    if (g_fault.frame_valid != 0U) {
        g_fault.frame = *(const fault_frame_t *)sp;
        report_frame(&g_fault.frame);

        /* 仅线程模式(bit3=1)时 current_tcb 才是肇事任务 */
        if (from_thr != 0U) {
            report_task(sp);
        }
        else
        {
            report_isr_context();
        }
    }

    report_status();

    /* 3. 处置指引 */
    fault_puts("--- Action ---\r\n");
    fault_puts("System halted. Fault context also mirrored in g_fault (debugger access).\r\n");
    fault_puts("Locate source: arm-none-eabi-addr2line -e f446_rtos.elf -f -C <PC>\r\n");
    fault_puts("======================================================\r\n");

    /* 4. 等最后一位字节移出移位寄存器(避免复位截尾; 对非 UART 通道无害) */
    {
        uint32_t guard = FAULT_TXE_GUARD;
        while (((FAULT_UART->SR & USART_SR_TC) == 0U) && (--guard != 0U)) { }
    }

    /* 5. 停机等调试器接管。
     *    生产期可改为: 把 g_fault 拷入 .noinit 段 + NVIC_SystemReset()。 */
    for (;;) {
    }
}
/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern UART_HandleTypeDef huart2;
/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1) {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */
  /* naked 纯汇编: 此刻 SP 未被任何软件触碰, MRS 读到的是硬件压栈帧
   * 精确基址; LR 仍是 EXC_RETURN(无 push 改写/无 BL 覆盖)。 */
  __asm volatile(
      " tst lr, #4            \n"   /* EXC_RETURN.bit2: 0=MSP, 1=PSP */
      " ite eq                \n"
      " mrseq r0, msp         \n"   /* r0 = 帧基(ISR 内故障场景) */
      " mrsne r0, psp         \n"   /* r0 = 帧基(任务故障场景) */
      " mov   r1, lr          \n"   /* r1 = EXC_RETURN */
      " b     fault_report    \n"   /* 尾跳转, 不返回(AAPCS: r0/r1 传参) */
);
  /* USER CODE END HardFault_IRQn 0 */
  while (1) {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */
  /* SHCSR.MEMFAULTENA 使能后 MPU/XN 类故障进入此处(不再升级 HardFault)。 */
  __asm volatile(
      " tst lr, #4            \n"   /* EXC_RETURN.bit2: 0=MSP, 1=PSP */
      " ite eq                \n"
      " mrseq r0, msp         \n"   /* r0 = 帧基(ISR 内故障场景) */
      " mrsne r0, psp         \n"   /* r0 = 帧基(任务故障场景) */
      " mov   r1, lr          \n"   /* r1 = EXC_RETURN */
      " b     fault_report    \n"   /* 尾跳转, 不返回 */
);
  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1) {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */
  /* SHCSR.BUSFAULTENA 使能后总线类错误(野指针/越界/非法地址)进入此处。 */
  __asm volatile(
      " tst lr, #4            \n"   /* EXC_RETURN.bit2: 0=MSP, 1=PSP */
      " ite eq                \n"
      " mrseq r0, msp         \n"   /* r0 = 帧基(ISR 内故障场景) */
      " mrsne r0, psp         \n"   /* r0 = 帧基(任务故障场景) */
      " mov   r1, lr          \n"   /* r1 = EXC_RETURN */
      " b     fault_report    \n"   /* 尾跳转, 不返回 */
);
  /* USER CODE END BusFault_IRQn 0 */
  while (1) {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */
  /* SHCSR.USAGEFAULTENA 使能后用法类错误(未定义指令/INVSTATE/NOCP/
   * DIVBYZERO/UNALIGNED)进入此处。 */
  __asm volatile(
      " tst lr, #4            \n"   /* EXC_RETURN.bit2: 0=MSP, 1=PSP */
      " ite eq                \n"
      " mrseq r0, msp         \n"   /* r0 = 帧基(ISR 内故障场景) */
      " mrsne r0, psp         \n"   /* r0 = 帧基(任务故障场景) */
      " mov   r1, lr          \n"   /* r1 = EXC_RETURN */
      " b     fault_report    \n"   /* 尾跳转, 不返回 */
);
  /* USER CODE END UsageFault_IRQn 0 */
  while (1) {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles System service call via SWI instruction.
  */
void SVC_Handler(void)
{
  /* USER CODE BEGIN SVCall_IRQn 0 */
  __asm volatile("b rtos_port_svc_handler");

  /* USER CODE END SVCall_IRQn 0 */
  /* USER CODE BEGIN SVCall_IRQn 1 */

  /* USER CODE END SVCall_IRQn 1 */
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles Pendable request for system service.
  */
void PendSV_Handler(void)
{
  /* USER CODE BEGIN PendSV_IRQn 0 */
  __asm volatile("b rtos_port_pend_sv_handler");

  /* USER CODE END PendSV_IRQn 0 */
  /* USER CODE BEGIN PendSV_IRQn 1 */

  /* USER CODE END PendSV_IRQn 1 */
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */
	rtos_port_sys_tick_handler();
  /* USER CODE END SysTick_IRQn 0 */
  HAL_IncTick();
  /* USER CODE BEGIN SysTick_IRQn 1 */

  /* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/* STM32F4xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f4xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles USART2 global interrupt.
  */
void USART2_IRQHandler(void)
{
  /* USER CODE BEGIN USART2_IRQn 0 */

  /* USER CODE END USART2_IRQn 0 */
  HAL_UART_IRQHandler(&huart2);
  /* USER CODE BEGIN USART2_IRQn 1 */

  /* USER CODE END USART2_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
