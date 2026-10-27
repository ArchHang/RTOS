/**
 * @file    kern_port_rtos.c
 * @brief   内核适配层的自研 RTOS 实现。
 *
 * @details 把 rtos_* API 映射到 kern_port.h 定义的统一接口。自研 RTOS 的
 *          优先级语义(数值小=高)与适配层一致, 故无需翻转。
 *
 *          对象存储: 自研 RTOS 的 sem/mutex/event 是调用方提供存储的静态
 *          结构体, 适配层直接把 storage 当作对象本体使用(零额外开销)。
 *
 *          计时: 直接读 DWT->CYCCNT, 与两侧内核无关, 保证口径一致。
 *
 * @note    本文件只被自研 RTOS 工程编译。
 */
#if defined(KERN_IS_CUSTOM_RTOS) /* KERN_IMPL_GUARD */
#include "kern_port.h"
#include "main.h"
#include "rtos.h"
#include <string.h>

/* ============================== 编译期契约检查 ============================== */

_Static_assert(sizeof(rtos_sem_t) <= 64U, "kern_port storage slot too small for rtos_sem_t");
_Static_assert(sizeof(rtos_mutex_t) <= 64U, "kern_port storage slot too small for rtos_mutex_t");
_Static_assert(sizeof(rtos_event_t) <= 64U, "kern_port storage slot too small for rtos_event_t");

/* ============================== 时间基准 ============================== */

/** @brief DWT 周期计数使能位。 */
#define KERN_DWT_CYCCNTENA (1UL << 0)

void kern_cycles_init(void)
{
    /* Cortex-M4 的 DWT 无 LAR 锁寄存器(M7/M33 才有), 直接使能即可 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= KERN_DWT_CYCCNTENA;
}

uint32_t kern_cycles(void)
{
    return DWT->CYCCNT;
}

uint32_t kern_cycles_to_us(uint32_t cycles)
{
    /* 168MHz: 168 周期 = 1µs。用整数除法, 由上位机决定小数处理。 */
    return cycles / (SystemCoreClock / 1000000U);
}

uint32_t kern_ticks(void)
{
    return rtos_sched_get_tick_count();
}

uint32_t kern_tick_rate(void)
{
    return RTOS_CONFIG_TICK_RATE_HZ;
}

/* ============================== 内核生命周期 ============================== */

void kern_init(void)
{
    (void)rtos_init();
}

void kern_start(void)
{
    (void)rtos_start();
    for (;;) {
        /* rtos_start 声明为 noreturn, 此处仅为形式完整 */
    }
}

void kern_yield(void)
{
    rtos_task_yield();
}

void kern_delay(uint32_t ticks)
{
    rtos_task_delay(ticks);
}

void kern_delay_until(uint32_t *last_wake_tick, uint32_t period)
{
    rtos_task_delay_until(last_wake_tick, period);
}

void kern_enter_critical(void)
{
    RTOS_PORT_ENTER_CRITICAL();
}

void kern_exit_critical(void)
{
    RTOS_PORT_EXIT_CRITICAL();
}

/* ============================== 任务 ============================== */

/** @brief 编译器要求的任务入口签名与适配层一致, 直接转型即可。 */
typedef void (*rtos_entry_t)(void *);

int32_t kern_task_create(kern_task_func_t func, void *arg, uint32_t stack_words, uint32_t prio,
                         const char *name)
{
    if ((func == NULL) || (prio >= RTOS_CONFIG_MAX_PRIORITIES)) {
        return -1;
    }

    rtos_tcb_t *tcb = NULL;
    rtos_status_t status =
        rtos_task_create(&tcb, stack_words, (rtos_entry_t)func, arg, prio, name);

    return (status == RTOS_OK) ? 0 : (int32_t)status;
}

void kern_task_delete_self(void)
{
    (void)rtos_task_delete(NULL);
}

uint32_t kern_task_priority_current(void)
{
    return rtos_task_get_priority(NULL);
}

/* ============================== 同步对象 ============================== */

int32_t kern_sem_init(void *storage, uint32_t size, uint32_t initial_count, uint32_t max_count)
{
    if ((storage == NULL) || (size < sizeof(rtos_sem_t))) {
        return -1;
    }
    return (rtos_sem_init((rtos_sem_t *)storage, initial_count, max_count) == RTOS_OK) ? 0 : -1;
}

int32_t kern_sem_take(void *storage, uint32_t timeout_ticks)
{
    if (storage == NULL) {
        return -1;
    }
    return (rtos_sem_take((rtos_sem_t *)storage, timeout_ticks) == RTOS_OK) ? 0 : -1;
}

int32_t kern_sem_give(void *storage)
{
    if (storage == NULL) {
        return -1;
    }
    return (rtos_sem_give((rtos_sem_t *)storage) == RTOS_OK) ? 0 : -1;
}

int32_t kern_mutex_init(void *storage, uint32_t size)
{
    if ((storage == NULL) || (size < sizeof(rtos_mutex_t))) {
        return -1;
    }
    return (rtos_mutex_init((rtos_mutex_t *)storage) == RTOS_OK) ? 0 : -1;
}

int32_t kern_mutex_take(void *storage, uint32_t timeout_ticks)
{
    if (storage == NULL) {
        return -1;
    }
    return (rtos_mutex_take((rtos_mutex_t *)storage, timeout_ticks) == RTOS_OK) ? 0 : -1;
}

int32_t kern_mutex_give(void *storage)
{
    if (storage == NULL) {
        return -1;
    }
    return (rtos_mutex_give((rtos_mutex_t *)storage) == RTOS_OK) ? 0 : -1;
}

int32_t kern_event_init(void *storage, uint32_t size)
{
    if ((storage == NULL) || (size < sizeof(rtos_event_t))) {
        return -1;
    }
    return (rtos_event_init((rtos_event_t *)storage) == RTOS_OK) ? 0 : -1;
}

uint32_t kern_event_set(void *storage, uint32_t bits)
{
    if (storage == NULL) {
        return 0U;
    }
    return (uint32_t)rtos_event_set((rtos_event_t *)storage, bits);
}

/* ============================== 中断注入 ============================== */

static kern_isr_handler_t g_isr_handler;

void kern_isr_set_handler(kern_isr_handler_t handler)
{
    g_isr_handler = handler;
}

uint32_t kern_in_isr(void)
{
    return (RTOS_PORT_IN_ISR() != RTOS_FALSE) ? 1U : 0U;
}

/**
 * @brief TIM6 更新中断: 调用场景注册的回调。
 * @details 频率与优先级由 kern_timer_isr_start 配置。优先级须满足
 *          >= RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY, 否则回调中调用
 *          内核 API 会破坏内核数据结构。
 */
void TIM6_DAC_IRQHandler(void)
{
    if ((TIM6->SR & TIM_SR_UIF) != 0U) {
        TIM6->SR = (uint32_t)~TIM_SR_UIF;
        if (g_isr_handler != NULL) {
            g_isr_handler();
        }
    }
}

int32_t kern_timer_isr_start(uint32_t period_us, uint32_t prio)
{
    /* TIM6 时钟源为 APB1 定时器时钟 84MHz: PSC=83 -> 1MHz(1 计数 = 1µs) */
    const uint32_t psc = 83U;
    const uint32_t arr = (period_us > 0U) ? (period_us - 1U) : 0U;

    __HAL_RCC_TIM6_CLK_ENABLE();
    TIM6->PSC = psc;
    TIM6->ARR = arr;
    TIM6->SR = 0U;
    TIM6->DIER = TIM_DIER_UIE;
    TIM6->CNT = 0U;

    HAL_NVIC_SetPriority(TIM6_DAC_IRQn, prio, 0U);
    HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
    TIM6->CR1 = TIM_CR1_CEN;

    return 0;
}

void kern_timer_isr_stop(void)
{
    TIM6->CR1 = 0U;
    HAL_NVIC_DisableIRQ(TIM6_DAC_IRQn);
    HAL_NVIC_ClearPendingIRQ(TIM6_DAC_IRQn);
}

#endif /* KERN_IMPL_GUARD */
