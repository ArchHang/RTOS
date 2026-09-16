/**
 * @file    port.c
 * @brief   Cortex-M4F / M7F 移植层实现(GCC)
 *
 * @details 本文件实现 RTOS 与 Cortex-M4F/M7F 内核的对接，包含:
 *            1. 上下文切换(PendSV 中断，保存/恢复 R4-R11、S16-S31、PSP)
 *            2. 临界区进出(BASEPRI 屏蔽系统调用级中断，支持嵌套)
 *            3. 首个任务启动(通过 SVC 进入 Handler 模式后异常返回进入线程模式)
 *            4. SysTick 周期配置与 FPU 惰性堆栈使能
 *            5. 任务栈初始化(构造伪硬件帧)
 *
 *          中断优先级配置职责:
 *            本移植层不设置 PendSV / SysTick 的中断优先级。这属于用户工程
 *            的配置职责, 用户必须在调用 rtos_start() 前将 PendSV 与
 *            SysTick 设为最低优先级(0xFF), 并将所有调用 RTOS API 的外设
 *            中断设为 >= MAX_SYSCALL_INTERRUPT_PRIORITY。详见移植文档
 *            design_and_analysis.md 第五部分。
 *
 *          适用内核(由编译器预定义宏自动识别，无需用户配置):
 *            - Cortex-M4F (STM32F4/F7/L4 等部分型号)
 *            - Cortex-M7F (STM32F7/H7 系列)
 *          两者均含 FPU(FPv4-SP 或 FPv5)，本移植仅使用 S16-S31 单精度
 *          寄存器保存，M4F/M7F 完全兼容。
 *
 *          上下文切换原理(Cortex-M 优势):
 *            - 硬件自动保存 R0-R3/R12/LR/PC/xPSR(基本帧)和 S0-S15/FPSCR
 *              (扩展帧，若启用 FPU 且任务使用了 FPU)。
 *            - 软件只需保存 R4-R11 与 S16-S31(callee-saved 寄存器)。
 *            - 利用 PendSV(最低优先级)执行切换，保证不在中断嵌套中间切换。
 *            - EXC_RETURN 值决定返回线程/处理模式及使用 MSP/PSP。
 *
 *          FPU 策略:
 *            - 启用惰性堆栈(Lazy Stacking, FPCCR.LSPEN=1)，硬件按需保存
 *              S0-S15/FPSCR，减少普通中断延迟。
 *            - 软件按 EXC_RETURN.bit4 判断当前任务是否含扩展帧:
 *              bit4=0(扩展帧)则保存/恢复 S16-S31；bit4=1(基本帧)则跳过。
 *            - EXC_RETURN 按任务保存(各任务可能不同)，保证切换正确。
 *
 *          栈帧布局(任务栈，低地址 -> 高地址):
 *            [R3(填充对齐)][EXC_RETURN][R4..R11][(S16..S31)][硬件帧]
 *             ^top_of_stack                                ^栈顶
 *          软件保存 10(无FPU) 或 26(有FPU) 字，均为偶数，保持 8 字节对齐。
 *
 *          时钟配置职责:
 *            本移植不负责时钟树初始化。SystemCoreClock 全局变量由用户工程
 *            提供(如 STM32CubeMX 的 system_stm32xx.c)，移植层在启动调度器
 *            时读取以配置 SysTick 重载值。用户需在调用 rtos_start_scheduler()
 *            之前完成时钟配置。
 *
 * @note    本移植面向 GCC(arm-none-eabi-)工具链。Keil/IAR 需调整汇编语法。
 */
#include "rtos_port.h"
#include "rtos_sched.h"
#include "rtos_task.h"
#include "rtos_perf.h"

/* ============================== 内核兼容性检查 ============================== */
/* 移植层要求 Cortex-M4F 或 M7F 内核(由 arm-none-eabi-gcc -mcpu 自动定义)。
 * 若编译报错，说明当前内核不在支持范围，需另行提供 port.c。 */
#if !defined(__ARM_ARCH_7EM__)
#error "本 port.c 仅支持 Cortex-M4F/M7F 内核(__ARM_ARCH_7EM__)。\
            Cortex-M3/M0+ 需要另外的移植实现。"
#endif

/* ============================== CMSIS 寄存器定义(精简) ============================== */
/* 仅包含 RTOS 移植所需，避免依赖完整 CMSIS 包。地址参考 ARMv7-M 架构手册。 */

/** @brief 系统控制块(SCB)寄存器结构。 */
typedef struct {
    volatile uint32_t CPUID; /**< 0xE000ED00: CPU ID。 */
    volatile uint32_t ICSR; /**< 0xE000ED04: 中断控制与状态。 */
    volatile uint32_t VTOR; /**< 0xE000ED08: 向量表偏移。 */
    volatile uint32_t AIRCR; /**< 0xE000ED0C: 应用中断与复位控制。 */
    volatile uint32_t SCR; /**< 0xE000ED10: 系统控制。 */
    volatile uint32_t CCR; /**< 0xE000ED14: 配置与控制。 */
    volatile uint8_t SHP[12]; /**< 0xE000ED18: 系统处理程序优先级。 */
    volatile uint32_t SHCSR; /**< 0xE000ED24: 系统处理程序控制与状态。 */
} SCB_Type;

/** @brief 系统滴答定时器(SysTick)寄存器结构。 */
typedef struct {
    volatile uint32_t CTRL; /**< 0xE000E010: 控制寄存器。 */
    volatile uint32_t LOAD; /**< 0xE000E014: 重载值。 */
    volatile uint32_t VAL; /**< 0xE000E018: 当前值。 */
    volatile uint32_t CALIB; /**< 0xE000E01C: 校准值。 */
} SysTick_Type;

/** @brief 中断控制器(NVIC)结构(精简，仅 ISER/ICER/IP)。 */
typedef struct {
    volatile uint32_t ISER[8]; /**< 0xE000E100: 中断使能。 */
    uint32_t RESERVED0[24];
    volatile uint32_t ICER[8]; /**< 0xE000E180: 中断除能。 */
    uint32_t RESERVED1[24];
    volatile uint32_t ISPR[8]; /**< 0xE000E200: 挂起设置。 */
    uint32_t RESERVED2[24];
    volatile uint32_t ICPR[8]; /**< 0xE000E280: 挂起清除。 */
    uint32_t RESERVED3[24];
    volatile uint32_t IABR[8]; /**< 0xE000E300: 活跃状态。 */
    uint32_t RESERVED4[56];
    volatile uint8_t IP[240]; /**< 0xE000E400: 中断优先级。 */
} NVIC_Type;

/** @brief 浮点单元上下文控制寄存器(FPU)。 */
typedef struct {
    volatile uint32_t FPCCR; /**< 0xE000EF34: 浮点上下文控制。 */
    volatile uint32_t FPCAR; /**< 0xE000EF38: 浮点上下文地址。 */
    volatile uint32_t FPDSCR; /**< 0xE000EF3C: 浮点默认状态控制。 */
    volatile uint32_t MVFR0; /**< 0xE000EF40: 媒体与 FP 特征 0。 */
} FPU_Type;

#define SCB_BASE (0xE000ED00UL)
#define SCS_BASE (0xE000E000UL)
#define SysTick_BASE (SCS_BASE + 0x0010UL)
#define NVIC_BASE (SCS_BASE + 0x0100UL)
#define FPU_BASE (0xE000EF34UL)

#define SCB ((SCB_Type *)SCB_BASE)
#define SysTick ((SysTick_Type *)SysTick_BASE)
#define NVIC ((NVIC_Type *)NVIC_BASE)
#define FPU ((FPU_Type *)FPU_BASE)

/* ICSR 位定义 */
#define SCB_ICSR_PENDSVSET_Msk (1UL << 28)
#define SCB_ICSR_PENDSVCLR_Msk (1UL << 27)

/* SysTick CTRL 位定义 */
#define SysTick_CTRL_ENABLE_Msk (1UL << 0)
#define SysTick_CTRL_TICKINT_Msk (1UL << 1)
#define SysTick_CTRL_CLKSOURCE_Msk (1UL << 2)

/* AIRCR: 优先级分组(STM32 系列用 4 位优先级，无分组)。 */
#define SCB_AIRCR_VECTKEY_Pos (16U)
#define SCB_AIRCR_VECTKEY_Msk (0xFFFFUL << SCB_AIRCR_VECTKEY_Pos)

/* SHPR: 系统处理程序优先级寄存器(字节访问)。
 * SHP 数组从 SHPR1 起算: index 0..11 对应异常 4..15 的优先级字节。
 * CMSIS 索引公式:  SHP[ ((uint32_t)IRQn & 0xF) - 4 ]
 *   PendSV  (IRQn = -2, 异常 14) -> SHP[10]   (SHPR3.byte[2])
 *   SysTick (IRQn = -1, 异常 15) -> SHP[11]   (SHPR3.byte[3])
 *   DebugMonitor (IRQn = -4, 异常 12) -> SHP[8] (SHPR3.byte[0])
 * 注意: SHP[8] 是 DebugMonitor, 不是 PendSV 也不是 SysTick!
 * 用户设置优先级时用 CMSIS API 即可, 无需手算索引:
 *   HAL_NVIC_SetPriority( PendSV_IRQn,  15, 0 );  // GROUP_4 下 = 0xFF
 *   HAL_NVIC_SetPriority( SysTick_IRQn, 15, 0 ); */

/* FPCCR 位定义 */
#define FPU_FPCCR_ASPEN_Msk (1UL << 31) /**< 自动状态保留使能。 */
#define FPU_FPCCR_LSPEN_Msk (1UL << 30) /**< 惰性状态保留使能。 */

/* ============================== 内部变量 ============================== */

/** @brief [perf P-1] 临界区嵌套计数(全局唯一, rtos_port.h 内联函数访问)。
 *         初始 0 表示未在临界区。enter 时先屏蔽中断再自增，exit 时自减，
 *         归零时解除屏蔽。BASEPRI 寄存器实际控制屏蔽，计数只跟踪嵌套深度。 */
volatile uint32_t rtos_port_crit_nest = 0U;

/* ============================== 临界区实现 ============================== */
/* [perf P-1] enter/exit/in_isr 已移至 rtos_port.h 的 static inline,
 * 消除每对临界区 ~10 周期的函数调用开销; exit 侧去掉 isb。 */

void rtos_port_disable_interrupts(void)
{
    __asm volatile(" cpsid i \n" ::: "memory");
}

void rtos_port_enable_interrupts(void)
{
    __asm volatile(" cpsie i \n" ::: "memory");
}

uint32_t rtos_port_get_interrupt_mask(void)
{
    uint32_t basepri;
    __asm volatile(" mrs %0, basepri \n" : "=r"(basepri)::"memory");
    return basepri;
}

void rtos_port_set_interrupt_mask(uint32_t mask)
{
    __asm volatile(" msr basepri, %0 \n" ::"r"(mask) : "memory");
}

uint32_t rtos_port_get_critical_nesting(void)
{
    return rtos_port_crit_nest;
}

uint32_t rtos_port_isr_priority(void)
{
    /* [guard G-3] 读取当前激活中断的 NVIC 优先级(高 4 位有效, 与
     * BASEPRI 数值直接可比)。线程模式返回 0。 */
    uint32_t ipsr;
    __asm volatile(" mrs %0, ipsr \n" : "=r"(ipsr) :: "memory");
    if (ipsr == 0U) {
        return 0U; /* 线程模式 */
    }
    if (ipsr >= 16U) {
        /* 外设 IRQ: NVIC->IP[] 已是移位后的值(优先级 << (8-__NVIC_PRIO_BITS)) */
        return NVIC->IP[ipsr - 16U];
    }
    /* 系统异常(PendSV/SysTick/SVC 等): SCB->SHP[(编号&0xF)-4] */
    return SCB->SHP[(ipsr & 0xFU) - 4U];
}

/* ============================== 触发上下文切换 ============================== */

void rtos_port_context_switch(void)
{
#if RTOS_CONFIG_PERF_HOTPATH_STATS
    /* 性能监视器: 记录 PendSV 挂起时刻, 供调度延迟统计。
     * 必须在置 PENDSVSET 之前调用, 以精确测量"请求切换→实际切换"的派发延迟。
     * 开销: 1 次 DWT_CYCCNT 读取 + 1 次内存写, 约 3 周期。 */
    rtos_perf_on_pend_switch();
#endif

    /* 设置 PendSV 挂起位; PendSV 优先级最低，会在退出所有中断后执行 */
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    __asm volatile(" dsb \n" ::: "memory");
    __asm volatile(" isb \n" ::: "memory");
}

/* ============================== 任务栈初始化 ============================== */

rtos_stack_t *rtos_port_init_stack(rtos_stack_t *stack_top, uint32_t stack_size,
                                   rtos_task_func_t entry, void *arg)
{
    rtos_stack_t *sp;
    uint32_t aligned;

    /* 任务退出处理函数声明(定义于 rtos_task.c) */
    extern void rtos_task_exit_handler(void);

    /* 8 字节对齐栈顶 */
    aligned = ((uint32_t)stack_top) & ~((uint32_t)RTOS_PORT_STACK_ALIGN_MASK);
    sp = (rtos_stack_t *)aligned;

    /* ---- 构造硬件帧(基本帧 8 字，xPSR 在高地址，R0 在低地址) ---- */
    sp--;
    *sp = 0x01000000UL; /* xPSR: Thumb 位(bit24)=1 */
    sp--;
    *sp = (rtos_stack_t)entry; /* PC: 任务入口 */
    sp--;
    *sp = (rtos_stack_t)rtos_task_exit_handler; /* LR: 任务返回时调用 */
    sp--;
    *sp = 0x12121212UL; /* R12 */
    sp--;
    *sp = 0x03030303UL; /* R3 */
    sp--;
    *sp = 0x02020202UL; /* R2 */
    sp--;
    *sp = 0x01010101UL; /* R1 */
    sp--;
    *sp = (rtos_stack_t)arg; /* R0: 任务参数 */

    /* ---- 软件保存区(顺序必须与 PendSV 保存顺序一致) ----
     * PendSV 保存顺序(STMDB，每次 r0 递减):
     *   1) vstmdbeq r0!, {s16-s31}  (若 FPU 扩展帧，最高地址)
     *   2) stmdb   r0!, {r4-r11}    (R11 在高地址，R4 在低地址)
     *   3) stmdb   r0!, {r3, lr}    (LR 在高地址，R3 在最低地址)
     *
     * 内存布局(低地址 -> 高地址):
     *   [R3 填充][EXC_RETURN][R4..R11][(S16..S31)][R0..R12|LR|PC|xPSR]
     *   ^sp 返回                                       ^栈顶
     *
     * 因此 *--sp 压栈顺序必须为: hw帧 -> R4-R11 -> EXC_RETURN -> R3 pad。
     * R4-R11 (8 字，R11 在高地址先压，R4 在低地址后压) */
    sp--;
    *sp = 0x11111111UL; /* R11 */
    sp--;
    *sp = 0x10101010UL; /* R10 */
    sp--;
    *sp = 0x09090909UL; /* R9 */
    sp--;
    *sp = 0x08080808UL; /* R8 */
    sp--;
    *sp = 0x07070707UL; /* R7 */
    sp--;
    *sp = 0x06060606UL; /* R6 */
    sp--;
    *sp = 0x05050505UL; /* R5 */
    sp--;
    *sp = 0x04040404UL; /* R4 */

    /* EXC_RETURN + R3(填充)用于对齐: STMDB {R3, LR} 布局
     * 低地址: R3(填充), 高地址: LR(EXC_RETURN)。
     * 初始任务未使用 FPU，使用基本帧返回值 0xFFFFFFFD。 */
    sp--;
    *sp = RTOS_PORT_EXC_RETURN_NO_FPU; /* LR(EXC_RETURN) */
    sp--;
    *sp = 0xDEDEDEDEUL; /* R3 填充(对齐用，最低地址) */

    /* 注意: S16-S31 不在初始化时压栈(惰性策略)。
     * 首次切换时 EXC_RETURN=0xFFFFFFFD(基本帧)，PendSV 不恢复 S16-S31。
     * 任务使用 FPU 后，硬件改为扩展帧并自动保存 S0-S15/FPSCR，
     * PendSV 检测到 bit4=0 后会保存/恢复 S16-S31。 */

    return sp; /* 指向 R3 填充(最低地址)，与 PendSV 恢复顺序一致 */
}

/* ============================== PendSV 中断(上下文切换) ============================== */

/* [perf P-2] rtos_kernel.current_tcb 的字节偏移(PendSV 汇编直接寻址用)。
 * 编译期断言防止结构体布局变化后汇编悄悄寻址错误字段。 */
#define PORT_CUR_TCB_OFF ((int)__builtin_offsetof(rtos_kernel_t, current_tcb))
#define PORT_TOP_STACK_OFF ((int)__builtin_offsetof(rtos_tcb_t, top_of_stack))
_Static_assert(PORT_TOP_STACK_OFF == 0, "top_of_stack must be first field of TCB");

/**
 * @brief PendSV 中断处理: 执行上下文切换。
 * @details naked 函数，无 prologue/epilogue，纯汇编。
 *
 * [perf P-2] 扁平化: 原实现经 ldr+blx 间接调用 rtos_port_save_and_switch
 * (C 函数: 保存 top_of_stack → 调 rtos_sched_context_switch → 返回新 PSP),
 * 共两层调用。现改为: 汇编内直接保存 top_of_stack(经偏移寻址)后直接
 * bl rtos_sched_context_switch, 从新 current_tcb 读回 PSP。
 * 消除一层调用序言/尾声 + 间接寻址, 每次切换节省 ~10-20 周期。
 *
 * 流程:
 *   1. 读 PSP(当前任务栈)。
 *   2. 测试 LR(EXC_RETURN).bit4: 0=扩展帧(含 FPU)，保存 S16-S31。
 *   3. 保存 R4-R11 + EXC_RETURN。
 *   4. 保存栈顶到 current_tcb->top_of_stack(汇编直存)。
 *   5. 调用 rtos_sched_context_switch 更新 current_tcb = next_tcb。
 *   6. 从新 current_tcb->top_of_stack 读回 PSP, 恢复上下文, 异常返回。
 *
 * @note  bl 覆盖 LR: EXC_RETURN 已保存到旧任务栈, 恢复时从新任务栈
 *        ldmia 取回新任务的 EXC_RETURN, 与原实现一致。
 * @note  PendSV 为最低优先级, 不会被 syscall 级中断打断, C 调用无需
 *        BASEPRI 保护(与原 save_and_switch 同判)。
 */
void rtos_port_pend_sv_handler(void)
{
    __asm volatile(
        " mrs  r0, psp                        \n" /* r0 = 当前 PSP */
        " isb                                 \n"
        "                                     \n"
        " tst  lr, #0x10                      \n" /* 测试 EXC_RETURN.bit4 */
        " it   eq                             \n" /* 若 bit4=0(扩展帧) */
        " vstmdbeq r0!, {s16-s31}             \n" /*   保存 S16-S31 */
        "                                     \n"
        " stmdb r0!, {r4-r11}                 \n" /* 保存 R4-R11 */
        " stmdb r0!, {r3, lr}                 \n" /* 保存 R3(对齐) + LR(EXC_RETURN) */
        "                                     \n"
        " ldr  r1, =rtos_kernel               \n"
        " ldr  r1, [r1, %[cur]]               \n" /* r1 = current_tcb (GCC "i" 约束自带 # 前缀) */
        " str  r0, [r1, %[top]]               \n" /* 保存栈顶到 TCB */
        "                                     \n"
        " bl   rtos_sched_context_switch      \n" /* current_tcb = next(单层调用) */
        "                                     \n"
        " ldr  r1, =rtos_kernel               \n"
        " ldr  r1, [r1, %[cur]]               \n" /* r1 = 新 current_tcb */
        " ldr  r0, [r1, %[top]]               \n" /* r0 = 新任务栈顶 */
        "                                     \n"
        " ldmia r0!, {r3, lr}                 \n" /* 恢复 R3 + 新任务 EXC_RETURN */
        " ldmia r0!, {r4-r11}                 \n" /* 恢复 R4-R11 */
        " tst  lr, #0x10                      \n" /* 测试新任务帧类型 */
        " it   eq                             \n"
        " vldmiaeq r0!, {s16-s31}             \n" /* 恢复 S16-S31(若扩展帧) */
        "                                     \n"
        " msr  psp, r0                        \n" /* 设置新 PSP */
        " isb                                 \n"
        " bx   lr                             \n" /* 异常返回到新任务 */
        " .ltorg                              \n"
        : : [cur] "i" (PORT_CUR_TCB_OFF), [top] "i" (PORT_TOP_STACK_OFF));
}

/* ============================== 启动首个任务 ============================== */

/**
 * @brief 获取首个任务的栈顶指针(C 辅助，避免汇编访问结构体偏移)。
 * @return current_tcb->top_of_stack。
 */
__attribute__((used)) static rtos_stack_t *rtos_port_get_first_sp(void)
{
    RTOS_ASSERT(rtos_kernel.current_tcb != NULL);
    return rtos_kernel.current_tcb->top_of_stack;
}

/**
 * @brief SVC 中断处理: 加载首个任务栈并异常返回进入线程模式。
 * @details naked 函数，无 prologue/epilogue，纯汇编。
 *          由 rtos_port_start_first_task() 中的 `svc 0` 触发进入。
 *
 *          为什么必须经 SVC 进入 Handler 模式:
 *            ARMv7-M 架构规定，BX LR 加载 EXC_RETURN 魔数值(0xFFFFFFFx)
 *            触发"异常返回"仅在 Handler 模式下有效。Thread 模式下执行
 *            BX 0xFFFFFFFD 会被当作普通分支跳转到 0xFFFFFFFD，触发 HardFault。
 *            SVC 指令使处理器进入 Handler 模式，此时 BX LR 才是合法的
 *            异常返回，从 PSP 弹出硬件帧进入首个任务。
 *
 *          流程:
 *            1. (硬件)SVC 入口保存基本帧到当前栈，LR = EXC_RETURN。
 *            2. 调用 C 辅助获取首个任务栈顶(指向 R3 填充，最低地址)。
 *            3. 设置 PSP。
 *            4. 从 PSP 依次恢复: R3/EXC_RETURN -> R4-R11 -> (S16-S31)。
 *               恢复顺序与 init_stack 压栈顺序、PendSV 出栈顺序一致。
 *            5. 更新 PSP 指向硬件帧，BX LR 异常返回进入首个任务。
 *               EXC_RETURN=0xFFFFFFFD 会使硬件自动从 PSP 弹出基本帧，
 *               并将 CONTROL.SPSEL 置 1(线程模式用 PSP)。
 *
 * @note  SVC 入口硬件保存的帧位于 MSP(因启动时 CONTROL.SPSEL=0)，
 *        本函数不返回该帧(永不返回)，MSP 残留约 32 字节基本帧空间，
 *        对后续中断无影响(中断帧会继续向低地址压栈)。
 */
void rtos_port_svc_handler(void)
{
    __asm volatile(" push {r1, lr}                \n" /* 保护 EXC_RETURN(8字节,保持MSP对齐) */
                   " bl rtos_port_get_first_sp    \n" /* r0 = 首任务栈顶 */
                   " pop {r1, lr}                 \n"
                   "                              \n"
                   " msr psp, r0                  \n" /* 设置 PSP */
                   " isb                          \n"
                   "                              \n"
                   " ldmia r0!, {r3, lr}          \n" /* 恢复 R3 + 任务 EXC_RETURN */
                   " ldmia r0!, {r4-r11}          \n" /* 恢复 R4-R11 */
                   " tst lr, #0x10                \n"
                   " it eq                        \n"
                   " vldmiaeq r0!, {s16-s31}      \n" /* 恢复 S16-S31(若扩展帧) */
                   " msr psp, r0                  \n"
                   " mov r1, #0                   \n" /* 清除 BASEPRI(start_first_task 中设置) */
                   " msr basepri, r1              \n" /* 允许所有中断, 首任务以全中断开启运行 */
                   "                              \n"
                   " bx lr                        \n" /* Handler 模式下: 异常返回进入首个任务 */
                   " .ltorg                       \n");
}

/**
 * @brief 触发 SVC 启动首个任务。
 * @details noreturn 函数。仅负责开中断并触发 SVC，所有上下文加载工作
 *          由 rtos_port_svc_handler() 在 Handler 模式下完成。
 *
 *          竞态防护: 先设 BASEPRI 屏蔽 SysTick/PendSV(优先级 >= 5)，再
 *          cpsie i 开中断。SVC 优先级为 0(最高)，不受 BASEPRI 影响，能
 *          立即响应。这样 cpsie→svc 之间即使 SysTick 到期也不会抢占 SVC，
 *          避免 PSP 未初始化时 PendSV 触发 HardFault。
 */
void rtos_port_start_first_task(void)
{
    __asm volatile(
        " msr basepri, %0              \n" /* 屏蔽 SysTick/PendSV, 不屏蔽 SVC */
        " cpsie i                      \n" /* 开中断, SVC(优先级0)立即响应 */
        " isb                          \n" /* [perf S-9] PRIMASK 写入仅需 isb 刷新
                                              * 取指流, dsb 徒增 ~10 周期(一次性
                                              * 路径, 纯卫生修正) */
        " svc 0                        \n" /* 触发 SVC,进入 Handler 模式 */
        " .ltorg                       \n" ::"r"(RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY)
        : "memory");
    __builtin_unreachable(); /* SVC 触发后通过异常返回进入任务, 永不返回调用者 */
}

/* ============================== 启动调度器(FPU/SysTick) ============================== */

void rtos_port_start_scheduler(void)
{
    /* 【移植要求 — 用户必须自行配置中断优先级】
     *
     * 本移植层不负责设置 PendSV / SysTick 的中断优先级, 这属于用户工程的
     * 配置职责(如 STM32CubeMX 的 NVIC 设置或 HAL_MspInit)。用户必须在调用
     * rtos_start() 之前完成以下配置, 否则调度器将无法正常工作:
     *
     *   1. PendSV 与 SysTick 必须设为最低优先级(0xFF), 保证上下文切换
     *      不抢占其他中断。若 PendSV 保持复位值 0(最高), BASEPRI 临界区
     *      无法屏蔽它, PendSV 会在临界区内立即触发, 导致 EXIT_CRITICAL
     *      永不执行, BASEPRI 卡在阈值, SysTick 被屏蔽, 调度器停摆。
     *
     *   2. NVIC 优先级分组建议设为 GROUP_4(4 位抢占优先级, 0 位子优先级),
     *      使 HAL_NVIC_SetPriority 的 preempt 参数直接对应 4 位优先级数值,
     *      与 BASEPRI 阈值语义一致。
     *
     *   3. 所有调用 RTOS API 的外设中断, 优先级数值必须 >=
     *      RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY(默认 0x50 = 优先级 5),
     *      否则内核临界区(BASEPRI)无法屏蔽它们, 会破坏调度器数据结构。
     *      优先级 0~4 保留给不调用 RTOS API 的硬实时中断。
     *
     * CMSIS SHP 索引公式:  SCB->SHP[ ((uint32_t)IRQn & 0xF) - 4 ]
     *   PendSV  (IRQn = -2) -> SHP[10]  (异常 14)
     *   SysTick (IRQn = -1) -> SHP[11]  (异常 15)
     *   注意: SHP[8] 是 DebugMonitor(异常 12), 不是 PendSV/SysTick!
     *
     * 也可用 CMSIS 标准 API:
     *   HAL_NVIC_SetPriority( PendSV_IRQn,  15, 0 );   // GROUP_4 下
     *   HAL_NVIC_SetPriority( SysTick_IRQn, 15, 0 );
     * 详见移植文档 design_and_analysis.md 第五部分。 */

    /* 1. 启用 FPU 惰性堆栈(Cortex-M4F/M7F) */
    FPU->FPCCR |= FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk;

    /* 2. 初始化临界区嵌套计数(启动后第一次进临界区会自增) */
    rtos_port_crit_nest = 0U;

    /* 3. 配置 SysTick 产生 RTOS_CONFIG_TICK_RATE_HZ 的周期中断。
     *    SystemCoreClock 由用户工程提供(必须 > 0)，移植层据此计算重载值。
     *    若用户未设置(== 0)，断言失败提醒用户先完成时钟配置。 */
    RTOS_ASSERT(SystemCoreClock > 0U);
    SysTick->LOAD = (SystemCoreClock / RTOS_CONFIG_TICK_RATE_HZ) - 1U;
    SysTick->VAL = 0UL; /* 清当前值 */
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | /* 选内核时钟(AHB/HCLK) */
                    SysTick_CTRL_TICKINT_Msk | /* 使能中断 */
                    SysTick_CTRL_ENABLE_Msk; /* 使能 SysTick */
}

/* ============================== SysTick 中断处理 ============================== */

void rtos_port_sys_tick_handler(void)
{
    /* 必须在调度器运行后才处理 Tick */
    if (rtos_kernel.sched_state != RTOS_SCHED_RUNNING) {
        return;
    }

    /* 进入临界区后调用调度器 Tick 处理(推进计数、唤醒延时任务、时间片轮转) */
    RTOS_PORT_ENTER_CRITICAL();
    rtos_sched_tick();
    RTOS_PORT_EXIT_CRITICAL();
}

/* ============================== 向量表中断映射(供用户启动文件使用) ==============================
 *
 * 用户工程的启动文件需将以下符号映射到向量表:
 *   PendSV_Handler  -> rtos_port_pend_sv_handler
 *   SysTick_Handler -> rtos_port_sys_tick_handler
 *   SVC_Handler     -> rtos_port_svc_handler(启动首个任务用, 必须映射)
 *
 * 实现方式有两种(任选其一):
 *   1. 在启动文件向量表中直接写 rtos_port_pend_sv_handler / rtos_port_sys_tick_handler
 *      / rtos_port_svc_handler。
 *   2. 在用户 C 代码中定义 PendSV_Handler/SysTick_Handler/SVC_Handler 并在其中
 *      调用上述函数。
 *
 * 注意: SVC 默认优先级为 0(最高), 不被 BASEPRI 屏蔽, 无需额外配置 SHPR2。
 *       若用户工程改过 SVC 优先级, 需保证其数值 < RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY,
 *       否则 SVC 会被临界区屏蔽, 导致启动失败。
 *
 * 时钟与启动文件均由用户 SDK 提供，本 RTOS 不捆绑。
 * ==================================================================================== */
