/**
 * @file    benchmark.c
 * @brief   自研 RTOS 性能基准测试(与 FreeRTOS 版逐项镜像)
 *
 * @details 全部指标用 DWT CYCCNT(168MHz, 1 cycle ≈ 5.952ns)计时, printf
 *          全部在测量区间之外。优先级拓扑(数值小=高):
 *            主任务 9 | H(被测高优) 6 | A/B(同优) 14 | 消费者 5 | L(低优) 20
 *          FreeRTOS 版取相反语义(数值大=高), 相对拓扑一致。
 *
 * 输出: BM|<id>|<name>|N=<n>|avg=<>|min=<>|max=<>  (单位: CPU 周期)
 * ISR 测试: 上位机看到 "BM|7|ARM" 行后回发 100 个任意字节。
 */
#include "benchmark.h"
#include "rtos.h"
#include "main.h"
#include <stdio.h>

/* ============================== 计时与统计 ============================== */

#define BM_N_YIELD    20000   /* yield 切换次数 */
#define BM_N_ROUND    10000   /* sem 乒乓回合数 */
#define BM_N_PREEMPT  1000    /* 抢占唤醒样本数 */
#define BM_N_QUEUE    5000    /* 队列直传样本数 */
#define BM_N_CREATE   500     /* 任务创建/删除次数 */
#define BM_N_DELAY1   500     /* delay(1) 次数 */
#define BM_N_ISR      100     /* ISR 唤醒样本数 */

static inline uint32_t bm_dwt(void)
{
    return DWT->CYCCNT;
}

static void bm_report1(const char *id, const char *name, uint32_t v)
{
    printf("BM|%s|%s|N=1|avg=%lu|min=%lu|max=%lu\r\n",
           id, name, (unsigned long)v, (unsigned long)v, (unsigned long)v);
}

/* 流式统计(RAM 受限, 不存样本数组): 单写者任务在线更新, 主任务在 done 后读 */
typedef struct {
    volatile uint32_t min;
    volatile uint32_t max;
    volatile uint64_t sum;
    volatile uint32_t cnt;
    volatile uint32_t done;
} bm_stats_t;

static void bm_stats_reset(bm_stats_t *s)
{
    s->min = 0xFFFFFFFFU;
    s->max = 0;
    s->sum = 0;
    s->cnt = 0;
    s->done = 0;
}

static void bm_stats_add(bm_stats_t *s, uint32_t v)
{
    if (v < s->min) { s->min = v; }
    if (v > s->max) { s->max = v; }
    s->sum += v;
    s->cnt = s->cnt + 1U;
}

static void bm_stats_report(bm_stats_t *s, const char *id, const char *name)
{
    printf("BM|%s|%s|N=%lu|avg=%lu|min=%lu|max=%lu\r\n",
           id, name, (unsigned long)s->cnt,
           (unsigned long)(s->sum / s->cnt), (unsigned long)s->min, (unsigned long)s->max);
}

/* ============================== BM0 临界区开销 ============================== */

static void bm_critical_section(void)
{
    uint32_t t0, t1, i;
    const uint32_t n = 10000;
    uint32_t s;

    t0 = bm_dwt();
    for (i = 0; i < n; i++) {
        RTOS_PORT_ENTER_CRITICAL();
        RTOS_PORT_EXIT_CRITICAL();
    }
    t1 = bm_dwt();
    s = (uint32_t)((t1 - t0) / n);
    bm_report1("0", "critical_pair", s);
}

/* ============================== BM1 yield 上下文切换 ============================== */

static volatile uint32_t y_start, y_stop, y_done_a, y_done_b;

static void bm_yield_fn(void *arg)
{
    uint32_t local = 0;
    if (arg == NULL) {
        y_start = bm_dwt(); /* A 首次运行, 计时开始 */
    }
    while (local < BM_N_YIELD / 2U) {
        rtos_task_yield();
        local++;
    }
    if (arg != NULL) {
        y_stop = bm_dwt(); /* B 最后一次, 计时结束 */
        y_done_b = 1;
    } else {
        y_done_a = 1;
    }
    for (;;) { rtos_task_delay(1000); }
}

static void bm_yield_switch(void)
{
    rtos_tcb_t *ta, *tb;

    y_done_a = 0;
    y_done_b = 0;
    rtos_task_create(&ta, 256, bm_yield_fn, NULL, 14, "bm_ya");
    rtos_task_create(&tb, 256, bm_yield_fn, (void *)1, 14, "bm_yb");
    while ((y_done_a == 0U) || (y_done_b == 0U)) {
        rtos_task_delay(2);
    }
    /* 总共 BM_N_YIELD 次 yield, 各自计满 N/2 后停 */
    bm_report1("1", "yield_switch", (y_stop - y_start) / (BM_N_YIELD - 1U));
    rtos_task_delete(ta);
    rtos_task_delete(tb);
}

/* ============================== BM2 sem 乒乓往返 ============================== */

static rtos_sem_t pp_s1, pp_s2;
static volatile uint32_t pp_start, pp_stop, pp_done_a, pp_done_b;

static void bm_pp_a(void *arg)
{
    uint32_t local = 0;
    (void)arg;
    pp_start = bm_dwt();
    while (local < BM_N_ROUND / 2U) {
        rtos_sem_take(&pp_s1, RTOS_WAIT_FOREVER);
        rtos_sem_give(&pp_s2);
        local++;
    }
    pp_done_a = 1;
    for (;;) { rtos_task_delay(1000); }
}

static void bm_pp_b(void *arg)
{
    uint32_t local = 0;
    (void)arg;
    while (local < BM_N_ROUND / 2U) {
        rtos_sem_take(&pp_s2, RTOS_WAIT_FOREVER);
        rtos_sem_give(&pp_s1);
        local++;
    }
    pp_stop = bm_dwt();
    pp_done_b = 1;
    for (;;) { rtos_task_delay(1000); }
}

static void bm_sem_pingpong(void)
{
    rtos_tcb_t *ta, *tb;
    rtos_sem_init(&pp_s1, 0, 1);
    rtos_sem_init(&pp_s2, 0, 1);
    pp_done_a = 0;
    pp_done_b = 0;

    rtos_task_create(&ta, 256, bm_pp_a, NULL, 14, "bm_pa");
    rtos_task_create(&tb, 256, bm_pp_b, NULL, 14, "bm_pb");
    rtos_sem_give(&pp_s1); /* 发令枪 */
    while ((pp_done_a == 0U) || (pp_done_b == 0U)) {
        rtos_task_delay(2);
    }
    /* 每 round = A(take+give) + B(take+give) + 2 次切换 */
    bm_report1("2", "sem_roundtrip", (pp_stop - pp_start) / (BM_N_ROUND / 2U));
    rtos_task_delete(ta);
    rtos_task_delete(tb);
}

/* ============================== BM3 sem 抢占唤醒延迟 ============================== */

static rtos_sem_t pr_sem;
static volatile uint32_t pr_t0;
static bm_stats_t pr_st;
static volatile uint32_t pr_h_done, pr_l_done;

static void bm_pr_high(void *arg)
{
    (void)arg;
    while (pr_st.cnt < BM_N_PREEMPT) {
        rtos_sem_take(&pr_sem, RTOS_WAIT_FOREVER);
        bm_stats_add(&pr_st, bm_dwt() - pr_t0);
    }
    pr_h_done = 1;
    for (;;) { rtos_task_delay(1000); }
}

static void bm_pr_low(void *arg)
{
    uint32_t n = 0;
    (void)arg;
    while (n < BM_N_PREEMPT) {
        pr_t0 = bm_dwt();
        rtos_sem_give(&pr_sem); /* H 抢占并醒来 */
        n++;
    }
    pr_l_done = 1;
    for (;;) { rtos_task_delay(1000); }
}

static void bm_preempt_wake(void)
{
    rtos_tcb_t *th, *tl;
    rtos_sem_init(&pr_sem, 0, 1);
    pr_h_done = 0;
    pr_l_done = 0;
    bm_stats_reset(&pr_st);
    rtos_task_create(&th, 256, bm_pr_high, NULL, 6, "bm_ph");
    rtos_task_create(&tl, 256, bm_pr_low, NULL, 20, "bm_pl");
    while ((pr_h_done == 0U) || (pr_l_done == 0U)) {
        rtos_task_delay(5);
    }
    bm_stats_report(&pr_st, "3", "sem_wake_latency");
    rtos_task_delete(th);
    rtos_task_delete(tl);
}

/* ============================== BM4 队列 4B 直传 ============================== */

static rtos_queue_t q_bench;
static uint8_t      q_storage[4 * 4];
static volatile uint32_t q_t0;
static bm_stats_t q_st;
static volatile uint32_t q_c_done;

static void bm_q_consumer(void *arg)
{
    uint32_t v;
    (void)arg;
    while (q_st.cnt < BM_N_QUEUE) {
        rtos_queue_recv(&q_bench, &v, RTOS_WAIT_FOREVER);
        bm_stats_add(&q_st, bm_dwt() - q_t0);
    }
    q_c_done = 1;
    for (;;) { rtos_task_delay(1000); }
}

static void bm_queue_direct(void)
{
    rtos_tcb_t *tc;
    uint32_t v = 0x11223344, n;

    rtos_queue_init(&q_bench, q_storage, 4, 4);
    q_c_done = 0;
    bm_stats_reset(&q_st);
    rtos_task_create(&tc, 256, bm_q_consumer, NULL, 5, "bm_qc");
    for (n = 0; n < BM_N_QUEUE; n++) {
        q_t0 = bm_dwt();
        rtos_queue_send(&q_bench, &v, RTOS_NO_WAIT, RTOS_FALSE);
    }
    while (q_c_done == 0U) { rtos_task_delay(5); }
    bm_stats_report(&q_st, "4", "queue_send_wake");
    rtos_task_delete(tc);
}

/* ============================== BM5 任务创建+删除 ============================== */

static void bm_dummy_fn(void *arg)
{
    (void)arg;
    for (;;) { rtos_task_delay(1000); }
}

static void bm_task_create_delete(void)
{
    rtos_tcb_t *t;
    uint32_t n, t0, t1;

    t0 = bm_dwt();
    for (n = 0; n < BM_N_CREATE; n++) {
        rtos_task_create(&t, 128, bm_dummy_fn, NULL, 20, "bm_d");
        rtos_task_delete(t);
    }
    t1 = bm_dwt();
    bm_report1("5", "task_create_del", (t1 - t0) / BM_N_CREATE);
    rtos_task_delay(200); /* idle 回收 TCB */
}

/* ============================== BM6 delay(1) 周期 ============================== */

static void bm_delay1(void)
{
    uint32_t n, t0, t1;

    t0 = bm_dwt();
    for (n = 0; n < BM_N_DELAY1; n++) {
        rtos_task_delay(1);
    }
    t1 = bm_dwt();
    bm_report1("6", "delay1_period", (t1 - t0) / BM_N_DELAY1);
}

/* ============================== BM7 ISR→任务唤醒延迟 ============================== */

static rtos_sem_t isr_sem;
static volatile uint32_t isr_t0;
static bm_stats_t isr_st;
static volatile uint32_t isr_armed, isr_done;

/* TIM6 100µs 周期中断作为确定性触发源(消除上位机交互随机性, 两工程同机制) */
void TIM6_DAC_IRQHandler(void)
{
    if ((TIM6->SR & TIM_SR_UIF) != 0U) {
        TIM6->SR = (uint32_t)~TIM_SR_UIF;
        if (isr_armed != 0U) {
            isr_armed = 0U;
            isr_t0 = bm_dwt();
            rtos_sem_give(&isr_sem); /* ISR 内直接 give(内部自动请求切换) */
        }
    }
}

static void bm_isr_high(void *arg)
{
    (void)arg;
    while (isr_st.cnt < BM_N_ISR) {
        rtos_sem_take(&isr_sem, RTOS_WAIT_FOREVER);
        bm_stats_add(&isr_st, bm_dwt() - isr_t0);
        isr_armed = 1U; /* 重新布防等下一次中断 */
    }
    isr_done = 1;
    for (;;) { rtos_task_delay(1000); }
}

static void bm_isr_wake(void)
{
    rtos_tcb_t *th;

    rtos_sem_init(&isr_sem, 0, 1);
    bm_stats_reset(&isr_st);
    isr_done = 0;

    /* TIM6: APB1 定时器时钟 84MHz, PSC=83 → 1MHz, ARR=99 → 100µs 周期 */
    __HAL_RCC_TIM6_CLK_ENABLE();
    TIM6->PSC = 83U;
    TIM6->ARR = 99U;
    TIM6->SR = 0U;
    TIM6->DIER = TIM_DIER_UIE;
    TIM6->CNT = 0U;

    rtos_task_create(&th, 256, bm_isr_high, NULL, 6, "bm_ih");
    isr_armed = 1U;

    HAL_NVIC_SetPriority(TIM6_DAC_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
    TIM6->CR1 = TIM_CR1_CEN; /* 启动 100 个采样中断 */

    while (isr_done == 0U) {
        rtos_task_delay(20);
    }
    TIM6->CR1 = 0U;         /* 停止 */
    isr_armed = 0U;
    bm_stats_report(&isr_st, "7", "isr_wake_latency");
    rtos_task_delete(th);
}

/* ============================== 主流程 ============================== */

void bench_task(void *arg)
{
    (void)arg;

    /* 使能 DWT 周期计数器 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    DWT->CYCCNT = 0;

    rtos_task_delay(100); /* 系统稳定 */
    printf("BM|META|target|f446_rtos (custom RTOS) 168MHz -O2\r\n");

    bm_critical_section(); /* BM0 */
    bm_yield_switch();     /* BM1 */
    bm_sem_pingpong();     /* BM2 */
    bm_preempt_wake();     /* BM3 */
    bm_queue_direct();     /* BM4 */
    bm_task_create_delete(); /* BM5 */
    bm_delay1();           /* BM6 */
    bm_isr_wake();         /* BM7 */

    printf("BM|DONE|benchmark complete\r\n");
    for (;;) {
        rtos_task_delay(1000);
    }
}
