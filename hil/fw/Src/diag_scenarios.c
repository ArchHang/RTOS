/**
 * @file    diag_scenarios.c
 * @brief   HIL 诊断场景 —— 用于核准既有场景夹具(S1..S5)的测量口径。
 *
 * @details 这些场景**不是**产品测试, 而是审计工具: 每一个回答一个具体的
 *          "报告里的结论是否真的被这份夹具测到了"的问题。只经 kern_port.h
 *          访问内核, 因此同一份代码也可编进 FreeRTOS 侧做对照。
 *
 *          触发方式: hil\build_fw.ps1 -Variant diag -Scenario <n>
 *          11 -> D1: S1 争用取证(回放 S1 的创建顺序, 记录是否真发生争用)
 *          16 -> D4: event_wait 是否遗留陈旧 queue_bitmap
 *          17 -> D5: timer_change_period 的周期上界
 *          18 -> D6: 初始栈帧 PC 的 bit0(Thumb 位)
 *          19 -> D7: delay() 成本随延时链长度的变化(O(n) 插入探测)   ⚠ 尚未采集完整数据
 *          20 -> D8: sem_take 阻塞成本随等待者数的变化(O(n) 尾插探测) ⚠ 尚未采集完整数据
 *          21 -> D9: 重负载下最坏关中断窗口(BASEPRI 持有时长上界)     ⚠ 口径未收敛, 不可引用
 *          22 -> D10: sched_suspend 期间阻塞 API 的拒绝契约            ✅ 已上板验证
 *
 *          各场景结论状态见 hil\README.md §4.1。
 */
#include "kern_port.h"
#include "test_framework.h"
#if defined(KERN_IS_CUSTOM_RTOS)
#include "rtos.h"
#endif
#include <stdio.h>
/* TIM7 寄存器定义 ── D7/D8/D9 的 µs 级计时源(84MHz, 11.9ns/计数)。
 * kern_port.h 只暴露统一接口, 不含 ST 头文件, 故此处直接引入 HAL。 */
#include "stm32f4xx_hal.h"

/* 与 test_scenarios.c 的 S1 规模保持一致, 便于结论对照 */
#define D_S1_ROUNDS    20U
#define D_S1_HOLD_WORK 4000U
#define D_S1_IF_WORK   40000U

/* ============================== D1: S1 争用取证 ============================== */

static volatile uint32_t d1_high_takes;
static volatile uint32_t d1_high_free_acquisitions;  /**< 试探取锁成功 = 此刻锁为空。 */
static volatile uint32_t d1_holder_entries;
static volatile uint32_t d1_holder_first_seen;
static volatile uint32_t d1_high_first_acquisition;
static volatile uint32_t d1_interferer_iters;
static test_stats_t      d1_latency;
static uint8_t           d1_mutex[64];

/** @brief D1 低优先级持锁者。 */
static void d1_holder_task(void *arg)
{
    (void)arg;
    for (uint32_t round = 0U; round < D_S1_ROUNDS; round++) {
        if (kern_mutex_take(d1_mutex, KERN_WAIT_FOREVER) == 0) {
            d1_holder_entries = d1_holder_entries + 1U;
            if (d1_holder_first_seen == 0U) {
                d1_holder_first_seen = round + 1U;
            }
            for (volatile uint32_t i = 0U; i < D_S1_HOLD_WORK; i++) {
                /* 持锁工作 */
            }
            (void)kern_mutex_give(d1_mutex);
        }
        kern_delay(1U);
    }
}

/**
 * @brief D1 高优先级抢锁者: 先非阻塞试探记录"此刻锁是否为空", 再阻塞获取并计时。
 * @details 试探结果只用于取证, 与计时口径无关(试探后立即归还)。
 */
static void d1_high_task(void *arg)
{
    (void)arg;
    for (uint32_t round = 0U; round < D_S1_ROUNDS; round++) {
        if (kern_mutex_take(d1_mutex, KERN_NO_WAIT) == 0) {
            d1_high_free_acquisitions = d1_high_free_acquisitions + 1U;
            (void)kern_mutex_give(d1_mutex);
        }
        const uint32_t t0 = kern_cycles();
        if (kern_mutex_take(d1_mutex, KERN_WAIT_FOREVER) == 0) {
            test_stats_add(&d1_latency, kern_cycles() - t0);
            d1_high_takes = d1_high_takes + 1U;
            if (d1_high_first_acquisition == 0U) {
                d1_high_first_acquisition = round + 1U;
            }
            (void)kern_mutex_give(d1_mutex);
        }
        kern_delay(1U);
    }
}

/** @brief D1 中优先级干扰者(与 S1 同构)。 */
static void d1_interferer_task(void *arg)
{
    (void)arg;
    for (;;) {
        for (volatile uint32_t i = 0U; i < D_S1_IF_WORK; i++) {
            /* 纯 CPU 消耗 */
        }
        d1_interferer_iters = d1_interferer_iters + 1U;
        kern_delay(1U);
    }
}

/** @brief 运行 D1: 复刻 S1 的创建顺序与工作量, 记录是否真的发生争用。 */
int32_t diag_s1_forensics(void)
{
    test_log("D1 start: does the S1 fixture actually create lock contention?");

    if (kern_mutex_init(d1_mutex, sizeof(d1_mutex)) != 0) { return -1; }

    d1_high_takes = 0U;
    d1_high_free_acquisitions = 0U;
    d1_holder_entries = 0U;
    d1_holder_first_seen = 0U;
    d1_high_first_acquisition = 0U;
    d1_interferer_iters = 0U;
    test_stats_reset(&d1_latency, 0U);

    /* 与 S1 完全相同的创建顺序: holder(LOW) → high(HIGH) → interferer */
    if (kern_task_create(d1_holder_task, NULL, 256U, KERN_PRIO_LOW, "d1_hold") != 0) { return -1; }
    if (kern_task_create(d1_high_task, NULL, 256U, KERN_PRIO_HIGH, "d1_high") != 0) { return -1; }
    if (kern_task_create(d1_interferer_task, NULL, 256U, KERN_PRIO_ABOVE, "d1_intf") != 0) {
        return -1;
    }

    for (uint32_t poll = 0U; (poll < 2000U) && (d1_high_takes < D_S1_ROUNDS); poll++) {
        kern_delay(10U);
    }

    printf("SC|D1|high_takes|value=%lu|unit=count\r\n", (unsigned long)d1_high_takes);
    printf("SC|D1|free_acquisitions|value=%lu|unit=count\r\n",
           (unsigned long)d1_high_free_acquisitions);
    printf("SC|D1|holder_entries|value=%lu|unit=count\r\n", (unsigned long)d1_holder_entries);
    printf("SC|D1|holder_first_round|value=%lu|unit=round\r\n",
           (unsigned long)d1_holder_first_seen);
    printf("SC|D1|high_first_acquisition|value=%lu|unit=round\r\n",
           (unsigned long)d1_high_first_acquisition);
    printf("SC|D1|interferer_iters|value=%lu|unit=rounds\r\n",
           (unsigned long)d1_interferer_iters);
    test_report_stats("D1", "block_latency_cyc", &d1_latency);
    test_log("D1 done");
    return 0;
}

/* ============================== D4: event_wait 陈旧 queue_bitmap ============================== */

static rtos_tcb_t       *d4_victim_tcb;
static volatile uint32_t d4_stage;
static rtos_sem_t       *d4_sem;
static uint8_t           d4_sem_storage[64];
static rtos_event_t      d4_event;

static void d4_victim_task(void *arg)
{
    (void)arg;
    (void)rtos_sem_take(d4_sem, 10U); /* 超时: 节点上留下该信号量的位图指针 */
    d4_stage = 1U;
    (void)rtos_event_wait(&d4_event, 0x01U, RTOS_EVENT_WAIT_ANY, RTOS_WAIT_FOREVER);
    d4_stage = 2U;
    for (;;) { kern_delay(1000U); }
}

/** @brief 运行 D4: 判定事件等待期间 queue_bitmap 是否为 NULL。 */
int32_t diag_event_stale_bitmap(void)
{
    test_log("D4 start: event_wait must not keep a stale queue_bitmap");

    d4_sem = (rtos_sem_t *)d4_sem_storage;
    (void)rtos_sem_init(d4_sem, 0U, 4U);
    (void)rtos_event_init(&d4_event);
    d4_stage = 0U;
    d4_victim_tcb = NULL;

    if (rtos_task_create(&d4_victim_tcb, 256U, d4_victim_task, NULL, 13U, "d4_vic") != 0) {
        printf("SC|D4|verdict|value=CREATE_FAILED|unit=text\r\n");
        return -1;
    }
    for (uint32_t i = 0U; (i < 500U) && (d4_stage < 1U); i++) { kern_delay(10U); }
    kern_delay(30U);

    const uint32_t blocked = (d4_stage == 1U) ? 1U : 0U;
    const uint32_t bm = (uint32_t)(uintptr_t)d4_victim_tcb->wait_node.queue_bitmap;
    const uint32_t sem_addr = (uint32_t)(uintptr_t)&d4_sem->wait_bitmap;

    (void)rtos_task_set_priority(d4_victim_tcb, 11U); /* 触发 requeue 路径 */
    const uint32_t sem_bm_now = (uint32_t)d4_sem->wait_bitmap;

    printf("SC|D4|blocked_in_event|value=%lu|unit=bool\r\n", (unsigned long)blocked);
    printf("SC|D4|bitmap_addr|value=0x%08lX|unit=addr\r\n", (unsigned long)bm);
    printf("SC|D4|sem_bitmap_addr|value=0x%08lX|unit=addr\r\n", (unsigned long)sem_addr);
    printf("SC|D4|sem_bitmap_value|value=%lu|unit=bits\r\n", (unsigned long)sem_bm_now);
    printf("SC|D4|verdict|value=%s|unit=text\r\n",
           (bm == 0U) ? "OK_bitmap_null" : "DEFECT_bitmap_not_null");

    (void)rtos_event_set(&d4_event, 0x01U);
    for (uint32_t i = 0U; (i < 200U) && (d4_stage < 2U); i++) { kern_delay(10U); }
    (void)rtos_task_delete(d4_victim_tcb);
    test_log("D4 done");
    return 0;
}

/* ============================== D5: timer_change_period 周期上界 ============================== */

static volatile uint32_t d5_fires;
static rtos_timer_t      d5_timer;

static void d5_cb(void *arg)
{
    (void)arg;
    d5_fires = d5_fires + 1U;
}

/** @brief 运行 D5: 改周期为 0xFFFFFFFF 后不得产生回调风暴。 */
int32_t diag_timer_period_bound(void)
{
    test_log("D5 start: change_period must reject periods outside the signed tick domain");

    d5_fires = 0U;
    const rtos_status_t cst =
        rtos_timer_create(&d5_timer, "d5", d5_cb, NULL, 100U, RTOS_TIMER_PERIODIC);
    const rtos_status_t sst = rtos_timer_start(&d5_timer);
    kern_delay(250U);
    const uint32_t fires_before = d5_fires;

    const rtos_status_t cps = rtos_timer_change_period(&d5_timer, 0xFFFFFFFFU);
    kern_delay(100U);
    const uint32_t fires_after = d5_fires - fires_before;
    (void)rtos_timer_stop(&d5_timer);

    printf("SC|D5|create_ret|value=%ld|unit=code\r\n", (long)cst);
    printf("SC|D5|start_ret|value=%ld|unit=code\r\n", (long)sst);
    printf("SC|D5|fires_before_change|value=%lu|unit=count\r\n", (unsigned long)fires_before);
    printf("SC|D5|change_period_ret|value=%ld|unit=code\r\n", (long)cps);
    printf("SC|D5|fires_in_100ms_after|value=%lu|unit=count\r\n", (unsigned long)fires_after);
    printf("SC|D5|verdict|value=%s|unit=text\r\n",
           ((cps != 0) || (fires_after <= 2U)) ? "OK_bounded" : "DEFECT_storm");
    test_log("D5 done");
    return 0;
}

/* ============================== D6: 首个任务的 PC 位 0 到底要不要清? ============================== */

/*
 * 背景: FreeRTOS 的 pxPortInitialiseStack 写初始硬件帧的 PC 槽时做了
 *   *pxTopOfStack = ((StackType_t)pxCode) & portSTART_ADDRESS_MASK;   [0xFFFFFFFE]
 * 即**主动清掉入口地址的 bit0(Thumb 位)**, 注释称 "strict compliance with the
 * Cortex-M spec: the task start address should have bit-0 clear, as it is loaded
 * into the PC on exit from an ISR"。自研内核则直接写原始函数指针(bit0 = 1)。
 *
 * 本场景用真机判定: 两种形式是否都可用?
 *
 * 手法: 任务创建后、首次被调度前, 直接改写其初始栈帧的 PC 槽。
 *   初始栈帧布局(见 rtos_port_init_stack, top_of_stack 索引 0):
 *     [0]=R3 pad [1]=EXC_RETURN [2..9]=R4-R11 [10..13]=R12,R3,R2,R1
 *     [14]=R0    [15]=LR        [16]=PC       [17]=xPSR
 *   故 PC 槽 = top_of_stack[16]。
 */

#define D6_TASK_STACK_WORDS 192U

static volatile uint32_t d6_runs[4];
static rtos_tcb_t       *d6_tcb[4];
static volatile uint32_t d6_stop;

static void d6_body(void *arg)
{
    uint32_t slot = (uint32_t)(uintptr_t)arg;
    if (slot < 4U) {
        d6_runs[slot] = d6_runs[slot] + 1U;
    }
    while (d6_stop == 0U) {
        kern_delay(5U);
    }
}

/** @brief 运行 D6: 比较入口地址 bit0 = 0 / 1 / 置位 / 取反 四种形式。 */
int32_t diag_pc_thumb_bit(void)
{
    static const rtos_prio_t prios[4] = { 20U, 21U, 22U, 23U };
    const uintptr_t entry = (uintptr_t)&d6_body;

    test_log("D6 start: must the initial-frame PC have bit0 cleared?");

    for (uint32_t i = 0U; i < 4U; i++) {
        d6_runs[i] = 0U;
        d6_tcb[i] = NULL;
    }
    d6_stop = 0U;

    for (uint32_t i = 0U; i < 4U; i++) {
        if (rtos_task_create(&d6_tcb[i], D6_TASK_STACK_WORDS, d6_body, (void *)(uintptr_t)i,
                             prios[i], "d6") != RTOS_OK) {
            printf("SC|D6|create_%lu|value=FAILED|unit=text\r\n", (unsigned long)i);
            return -1;
        }
    }

    /* 任务优先级 20~23 低于本场景任务(9), 尚未被调度; 首次切入前栈顶不会移动,
     * 因此 top_of_stack[16] 就是硬件帧的 PC 槽。 */
    for (uint32_t i = 0U; i < 4U; i++) {
        rtos_stack_t *pc_slot = &d6_tcb[i]->top_of_stack[16];
        uintptr_t v = entry;
        switch (i) {
            case 0U: v = entry & ~(uintptr_t)1U; break; /* FreeRTOS 形式: 清 bit0 */
            case 1U: v = entry;                  break; /* 自研形式: 原始函数指针  */
            case 2U: v = entry | (uintptr_t)1U;  break; /* 显式置 bit0             */
            default: v = entry ^ (uintptr_t)1U;  break; /* 取反 = 清 bit0          */
        }
        *pc_slot = (rtos_stack_t)v;
        printf("SC|D6|variant_%lu|entry=0x%08lX|pc_slot=0x%08lX|bit0=%lu\r\n",
               (unsigned long)i, (unsigned long)entry, (unsigned long)v,
               (unsigned long)(v & 1U));
    }

    kern_delay(300U); /* 让四个任务各自跑起来 */

    for (uint32_t i = 0U; i < 4U; i++) {
        printf("SC|D6|runs_variant_%lu|value=%lu|unit=count\r\n", (unsigned long)i,
               (unsigned long)d6_runs[i]);
    }
    printf("SC|D6|verdict|value=%s|unit=text\r\n",
           (d6_runs[0] && d6_runs[1]) ? "BOTH_ACCEPTED" : "DIFFERENT");

    d6_stop = 1U;
    kern_delay(50U);
    for (uint32_t i = 0U; i < 4U; i++) {
        (void)rtos_task_delete(d6_tcb[i]);
    }
    test_log("D6 done");
    return 0;
}


/* ==================================================================== */
/*  D7/D8/D9: 规模退化与最坏关中断窗口的定量测量                          */
/*                                                                      */
/*  背景: 既有审计长期把三项列为"已承认但从未测量":                       */
/*    - delay_list_insert 的 O(n) 慢路径                                 */
/*    - rtos_internal_wait_append 的 O(n) 尾插                           */
/*    - 最坏关中断窗口(WCET): BM0 的 23 周期只是空临界区对, 不是上界      */
/*  本组用真机把它们量化。计时方案: TIM7 自由运行于 84MHz(11.9ns/计数),   */
/*  比 DWT 的 5.952ns/周期粗, 但 DWT 已在别处用尽; 对 >1µs 的量级足够。   */
/* ==================================================================== */

/** @brief 启动 TIM7 作为自由运行的 84MHz 计数器(1 计数 ≈ 11.9ns)。 */
static void d_tim7_start(void)
{
    __HAL_RCC_TIM7_CLK_ENABLE();
    TIM7->PSC = 0U;          /* 84MHz / 1 = 84MHz */
    TIM7->ARR = 0xFFFFU;     /* 最大量程, 允许回绕; 用 16 位差值读 */
    TIM7->SR = 0U;
    TIM7->EGR = TIM_EGR_UG;  /* 立即装载 PSC */
    TIM7->SR = 0U;
    TIM7->CR1 = TIM_CR1_CEN;
}

/** @brief 读 TIM7 计数(16 位, 回绕安全)。 */
static inline uint16_t d_tim7(void)
{
    return (uint16_t)TIM7->CNT;
}

/** @brief 自上次读数以来的 84MHz 计数差(16 位回绕安全)。 */
static inline uint16_t d_tim7_delta(uint16_t t0)
{
    return (uint16_t)(d_tim7() - t0);
}

/** @brief 把 84MHz 计数换成有效 ns(避免 32 位溢出, 先除后乘)。 */
static inline uint32_t d_tim7_ns(uint16_t cyc)
{
    /* 84 counts = 1µs => ns = cyc * 1000 / 84 */
    return ((uint32_t)cyc * 1000U) / 84U;
}

/* ---------------- D7: 延时链插入的规模敏感性 ---------------- */

#define D7_MAX_TASKS 30U

static volatile uint32_t d7_run;
static volatile uint32_t d7_phase;
static uint16_t          d7_t0;
static uint32_t          d7_samples[D7_MAX_TASKS + 1U];
static uint32_t          d7_counts[D7_MAX_TASKS + 1U];

/** @brief D7 保持延时链非空的填充任务: 用 delay_until 带偏移, 保证互不同步。 */
static void d7_filler_task(void *arg)
{
    const uint32_t slot = (uint32_t)(uintptr_t)arg;
    rtos_tick_t last = 0U;
    const rtos_tick_t period = 200U + (slot * 37U); /* 错开相位, 避免同 tick 到期 */

    for (;;) {
        rtos_task_delay_until(&last, period);
    }
}

/** @brief D7 测量任务: 在延时链长度为 N 时重复调用 delay(1) 并计时。 */
static void d7_probe_task(void *arg)
{
    (void)arg;
    uint32_t i;

    /* 自旋等主任务放行 */
    while (d7_phase == 0U) { }

    /* 预热 1 次 */
    d7_t0 = d_tim7();
    rtos_task_delay(1U);
    (void)d_tim7_delta(d7_t0);

    /* 采样 */
    for (i = 0U; i < 32U; i++) {
        d7_t0 = d_tim7();
        rtos_task_delay(1U);
        d7_samples[d7_run] += d_tim7_delta(d7_t0);
        d7_counts[d7_run]++;
    }
    d7_phase = 2U; /* 通知主任务本次采样完成 */
}

/**
 * @brief 运行 D7: 测 delay(1) 在延时链长度 N = 0..30 下的往返耗时。
 * @note  测量含"阻塞→tick 唤醒"的固定量化(0~1ms), 故只比较**相对变化**;
 *        若插入为 O(n) 且每节点开销可观, 会看到随 N 单调上升的趋势。
 */
int32_t diag_delay_list_scaling(void)
{
    rtos_tcb_t *filler[D7_MAX_TASKS];
    rtos_tcb_t *probe = NULL;
    uint32_t    i;

    test_log("D7 start: delay() cost vs delay-list length (O(n) insert probe)");
    d_tim7_start();

    if (rtos_task_create(&probe, 256U, d7_probe_task, NULL, 6U, "d7_probe") != RTOS_OK) {
        printf("SC|D7|verdict|value=CREATE_FAILED|unit=text\r\n");
        return -1;
    }

    for (i = 0U; i <= D7_MAX_TASKS; i++) {
        /* 目标链长 = i: 先建/删填充任务使其恰好为 i 个在延时链上 */
        if (i > 0U) {
            if (rtos_task_create(&filler[i - 1U], 192U, d7_filler_task, (void *)(uintptr_t)i, 12U,
                                 "d7_fill") != RTOS_OK) {
                printf("SC|D7|fill_create_fail_at|value=%lu|unit=count\r\n", (unsigned long)i);
                break;
            }
        }

        d7_run = i;
        d7_samples[i] = 0U;
        d7_counts[i] = 0U;
        d7_phase = 0U;
        rtos_task_delay(40U); /* 让填充任务进入各自的 delay_until */
        d7_phase = 1U;        /* 放行 probe */

        for (uint32_t w = 0U; (w < 2000U) && (d7_phase != 2U); w++) {
            rtos_task_delay(5U);
        }

        printf("SC|D7|chain=%lu|samples=%lu|sum_84MHz_ticks=%lu|avg_ns=%lu|typical_ns=%lu\r\n",
               (unsigned long)i, (unsigned long)d7_counts[i], (unsigned long)d7_samples[i],
               (unsigned long)((d7_counts[i] != 0U) ? (d7_samples[i] / d7_counts[i]) : 0U),
               (unsigned long)((d7_counts[i] != 0U)
                                   ? (((d7_samples[i] / d7_counts[i]) * 1000U) / 84U)
                                   : 0U));
        if (d7_phase != 2U) {
            printf("SC|D7|timeout_at_chain|value=%lu|unit=count\r\n", (unsigned long)i);
            break;
        }
    }

    printf("SC|D7|verdict|value=see_avg_ns_trend|unit=text\r\n");
    (void)rtos_task_delete(probe);
    test_log("D7 done");
    return 0;
}

/* ---------------- D8: 信号量等待链尾插的规模敏感性 ---------------- */

#define D8_MAX_WAITERS 16U

static rtos_sem_t        d8_sem;
static volatile uint32_t d8_run8;   /* 本轮等待者数(探测任务回写用) */
static uint16_t          d8_t0;
static volatile uint32_t d8_ready_cnt;
static volatile uint8_t  d8_go;
static uint32_t          d8_take_ticks[D8_MAX_WAITERS + 1U];
static uint32_t          d8_take_n[D8_MAX_WAITERS + 1U];

/** @brief D8 等待者: 永久阻塞在信号量上, 使等待链长度可控。 */
static void d8_waiter_task(void *arg)
{
    (void)arg;
    d8_ready_cnt++;
    (void)rtos_sem_take(&d8_sem, RTOS_WAIT_FOREVER);
    for (;;) { rtos_task_delay(1000U); }
}

/** @brief D8 探测任务: 等 N 个等待者就位后, 计时一次"必然阻塞"的 take。 */
static void d8_probe_task(void *arg)
{
    (void)arg;
    while (d8_go == 0U) { }

    /* timeout=4 tick: 无法获得(链上有等待者且计数为 0) → 必然走"入链+阻塞+超时" */
    d8_t0 = d_tim7();
    (void)rtos_sem_take(&d8_sem, 4U);
    d8_take_ticks[d8_run8] = d_tim7_delta(d8_t0);
    d8_take_n[d8_run8] = 1U;
    d8_go = 2U;
}

/** @brief D8 的"当前链长"索引(探测任务回写用)。 */

/**
 * @brief 运行 D8: 测 sem_take 阻塞成本随等待者数 N 的变化。
 * @note  sem 用"尾插 + 位图", 入链需遍历到链尾 → 预期每等待者 ~常数增量。
 *        对比: FreeRTOS 的等价实现是双向链尾插(其 xTaskRemoveFromEventList 亦为 O(1))。
 */
int32_t diag_wait_chain_scaling(void)
{
    rtos_tcb_t *waiter[D8_MAX_WAITERS];
    rtos_tcb_t *probe = NULL;
    uint32_t    i;

    test_log("D8 start: sem_take blocking cost vs number of blocked waiters");
    d_tim7_start();
    (void)rtos_sem_init(&d8_sem, 0U, 1U);

    for (i = 0U; i < D8_MAX_WAITERS; i++) { waiter[i] = NULL; }

    if (rtos_task_create(&probe, 256U, d8_probe_task, NULL, 6U, "d8_probe") != RTOS_OK) {
        printf("SC|D8|verdict|value=CREATE_FAILED|unit=text\r\n");
        return -1;
    }

    for (i = 0U; i <= D8_MAX_WAITERS; i++) {
        d8_ready_cnt = 0U;
        d8_go = 0U;

        /* 先建 i 个等待者并等它们全部阻塞 */
        for (uint32_t k = 0U; k < i; k++) {
            if (rtos_task_create(&waiter[k], 192U, d8_waiter_task, NULL, (rtos_prio_t)(20U + k),
                                 "d8_wait") != RTOS_OK) {
                printf("SC|D8|waiter_create_fail_at|value=%lu|unit=count\r\n", (unsigned long)k);
                i = D8_MAX_WAITERS + 1U;
                break;
            }
        }
        for (uint32_t w = 0U; (w < 500U) && (d8_ready_cnt < i); w++) {
            rtos_task_delay(5U);
        }

        d8_run8 = i;
        d8_take_ticks[i] = 0U;
        d8_take_n[i] = 0U;
        d8_go = 1U;
        for (uint32_t w = 0U; (w < 1000U) && (d8_go != 2U); w++) {
            rtos_task_delay(5U);
        }

        printf("SC|D8|waiters=%lu|ready=%lu|take_ns=%lu|ok=%lu\r\n", (unsigned long)i,
               (unsigned long)d8_ready_cnt,
               (unsigned long)((d8_take_n[i] != 0U)
                                   ? (((uint32_t)d8_take_ticks[i] * 1000U) / 84U)
                                   : 0U),
               (unsigned long)((d8_go == 2U) ? 1U : 0U));

        /* 拆掉本轮等待者(用 give 逐个唤醒, 再删除) */
        for (uint32_t k = 0U; k < i; k++) {
            (void)rtos_sem_give(&d8_sem);
            rtos_task_delay(2U);
            if (waiter[k] != NULL) { (void)rtos_task_delete(waiter[k]); }
        }
        rtos_task_delay(20U);
    }

    printf("SC|D8|verdict|value=see_take_ns_trend|unit=text\r\n");
    (void)rtos_task_delete(probe);
    test_log("D8 done");
    return 0;
}

/* ---------------- D9: 最坏关中断窗口(WCET) ---------------- */

/*
 * 方法: TIM6 以固定 10µs 周期产生**优先级 4** 的中断。BASEPRI 阈值是 0x50
 * (=优先级 5), 因此优先级 4 **不被临界区屏蔽**, 但必须等临界区退出才能进入 ——
 * 于是一个优先级 4 的 ISR 的**入口抖动**就直接等于"内核拉高 BASEPRI 的时长",
 * 也就是最坏关中断窗口的实测上界(含 ISR 硬件入栈抖动, 属真实延迟的一部分)。
 *
 * 用 TIM7(84MHz) 测量"本应发生的两次中断之间"的实际间隔与标称间隔之差, 取最大值。
 *
 * ⚠ 结论状态: **口径未收敛, 本场景打印的任何 late_ns 都不可引用**。
 *   已验证成立的前提: 临界区内 BASEPRI=0x50、TIM6 的 IP 字节=0x40(优先级 4, 不被屏蔽)、
 *   对照窗口实测 5.0ms 且窗口内 TIM6 进入次数为 0(= 中断确实被屏蔽并挂起)。
 *   失败点: 按上述前提, 退出临界区时那次被推迟的中断应让相邻两次 ISR 的时间差暴增,
 *   但实测 late 始终只有 190ns。最可能的根因是 TIM7 为 16 位计数器(84MHz 下 780µs 回绕),
 *   5ms 的推迟在 16 位差值下混叠成小数值; 小延迟档同样未捕捉到, 说明可能还有第二个问题。
 *   要拿到 WCET 上界必须换方案: 32 位时基, 或 GPIO 翻转 + 逻辑分析仪, 或统计"被推迟中断"的
 *   计数差。完整记录见 docs/rtos_testing_report.md §10.3。
 */

#define D9_IRQ_PERIOD_US 10U
#define D9_SAMPLES       4000U

static volatile uint32_t d9_max_late_ns;
#define D9_HIST_MAX 8192U
static uint32_t          d9_hist[D9_HIST_MAX];
static volatile uint32_t d9_hist_n;
static volatile uint32_t d9_bad_late_ns;    /* 对照样本: 故意制造的关中断窗口时长 */
static volatile uint32_t d9_ctrl_hold_cyc;  /* 对照临界区的实测时长(DWT 周期) */
static volatile uint32_t d9_dbg_basepri;   /* 临界区内读回的 BASEPRI */
static volatile uint32_t d9_dbg_tim6_prio; /* TIM6 的 NVIC->IP 字节 */
static volatile uint32_t d9_dbg_shcsr;
static volatile uint32_t d9_dbg_aircr;
static volatile uint32_t d9_ctrl_isr_inside;  /* 临界区窗口内进入的 ISR 次数 */
static volatile uint32_t d9_cnt;
static volatile uint32_t d9_load;           /* 1 = 同时制造重负载 */
static uint16_t          d9_last;
static volatile uint32_t d9_primed;

/**
 * @brief D9 的 ISR 体 —— 注册给 kern_port_rtos.c 的 TIM6 回调(不重复定义 IRQHandler)。
 * @note  复用既有 ISR 设施的原因: kern_port_rtos.c 已经定义了 TIM6_DAC_IRQHandler,
 *        重复定义会链接冲突。周期由 kern_timer_isr_start 决定, 标称值必须与之一致。
 */
static void d9_isr_body(void)
{
    /* 计数与"上次时刻"必须**无条件**更新: 基线与负载两段都靠 d9_cnt 收敛,
     * 只在负载态计数会让基线循环永远等不到样本(首版实现即卡死于此)。 */
    const uint16_t now = d_tim7();

    if (d9_primed != 0U) {
        /* 标称间隔 = 84 * D9_IRQ_PERIOD_US 个 84MHz 计数; 超出部分即被推迟的时间。
         * 只在负载态**记录**, 但 d9_last 在基线态也一直更新 —— 这样负载段的首个
         * 样本也用紧邻的上一拍作基准, 不会拿很久以前的时刻算出假的大延迟。 */
        const uint16_t nominal = (uint16_t)(84U * D9_IRQ_PERIOD_US);
        const uint16_t dlt = (uint16_t)(now - d9_last);
        if ((d9_load != 0U) && (dlt > nominal)) {
            const uint32_t late = d_tim7_ns((uint16_t)(dlt - nominal));
            if (late > d9_max_late_ns) { d9_max_late_ns = late; }
            if (late > d9_bad_late_ns) { d9_bad_late_ns = late; }
            if (d9_hist_n < D9_HIST_MAX) { d9_hist[d9_hist_n] = late; d9_hist_n++; }
        }
    }
    d9_last = now;
    d9_primed = 1U;
    if (d9_cnt < 0xFFFFFFFFU) { d9_cnt++; }
}

/** @brief 重负载任务: 持续跑那些"在临界区内做长扫描"的内核路径。 */
static void d9_load_task(void *arg)
{
    rtos_perf_task_t arr[RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE];
    uint32_t actual = 0U;

    (void)arg;
    while (d9_load == 0U) { rtos_task_delay(10U); }

    for (;;) {
        /* 1) perf 全任务栈水位扫描(临界区内逐字扫描每个任务的整段栈) */
        (void)rtos_perf_get_all_tasks(arr, RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE, &actual);
        /* 2) heap 反复分配/释放(first-fit 全程持锁遍历) */
        {
            void *p = rtos_heap_alloc(512U);
            if (p != NULL) { (void)rtos_heap_free(p); }
        }
        /* 3) 已知坏样本对照: 主动制造一次**可控时长**的关中断窗口, 并实测它有多长。
         *    若测量链有效, control_known_bad_ns 必须 ≈ ctrl_hold_ns;
         *    这同时也证伪"测量不到"这一可能(首版用空循环, 被优化掉后对照只有 166ns)。 */
        {
            const uint32_t c0 = DWT->CYCCNT;
            kern_enter_critical();
            {
                /* 诊断: 直接读回 BASEPRI 与 TIM6 的实际 NVIC 优先级字节,
                 * 确认"优先级 4 不被 BASEPRI 屏蔽"这一测量前提是否成立。 */
                uint32_t bp;
                __asm volatile(" mrs %0, basepri \n" : "=r"(bp));
                d9_dbg_basepri = bp;
                d9_dbg_tim6_prio = (uint32_t)NVIC->IP[(uint32_t)TIM6_DAC_IRQn];
                d9_dbg_shcsr = SCB->SHCSR;
                d9_dbg_aircr = SCB->AIRCR;
            }
            /* 忙等 840000 个 DWT 周期 ≈ 5ms, 远大于 TIM6 的 10µs 周期 */
            while ((uint32_t)(DWT->CYCCNT - c0) < 840000U) {
            }
            kern_exit_critical();
            d9_ctrl_hold_cyc = (uint32_t)(DWT->CYCCNT - c0);
        }
        rtos_task_delay(1U);   /* 必须放权: yield 只在本优先级内轮转, 会堵住采样主任务 */
    }
}

/**
 * @brief 运行 D9: 在重负载下测优先级 4 中断的最大入口抖动。
 * @note  只报"最大值"这一个数 —— 它就是本内核的关中断时长上界(含硬件入栈)。
 */
int32_t diag_interrupt_blackout(void)
{
    rtos_tcb_t *ld = NULL;
    uint32_t    w;

    test_log("D9 start: worst-case interrupt blackout window (BASEPRI hold time)");
    d_tim7_start();

    /* TIM6: 10µs 周期, 优先级 4(不被 BASEPRI 0x50 屏蔽) */
    /* TIM6: D9_IRQ_PERIOD_US 周期, NVIC 优先级 4 —— 优先级 4 < BASEPRI 阈值(5), 因此
     * **不被临界区屏蔽**, 但必须等临界区退出才能进入; 其入口抖动即关中断窗口的实测上界。
     * 用既有 ISR 设施(TIM6 handler 已在 kern_port_rtos.c 中定义, 不能重复定义)。 */
    kern_isr_set_handler(d9_isr_body);
    (void)kern_timer_isr_start(D9_IRQ_PERIOD_US, 4U);

    /* 先测空载基线 */
    d9_max_late_ns = 0U;
    d9_cnt = 0U;
    d9_primed = 0U;
    d9_load = 0U;
    for (w = 0U; (w < 8000U) && (d9_cnt < 2000U); w++) { rtos_task_delay(1U); }
    printf("SC|D9|baseline_done|cnt=%lu|w=%lu\r\n", (unsigned long)d9_cnt, (unsigned long)w);
    const uint32_t idle_max = d9_max_late_ns;
    const uint32_t idle_cnt = d9_cnt;
    kern_timer_isr_stop();
    printf("SC|D9|mark_stopped\r\n");

    /* 再测重负载 */
    if (rtos_task_create(&ld, 512U, d9_load_task, NULL, 12U, "d9_load") != RTOS_OK) {
        printf("SC|D9|verdict|value=LOAD_CREATE_FAILED|unit=text\r\n");
        return -1;
    }
    d9_max_late_ns = 0U;
    d9_cnt = 0U;
    d9_primed = 0U;
    d9_load = 1U;
    (void)kern_timer_isr_start(D9_IRQ_PERIOD_US, 4U);   /* 基线后停过, 负载段必须重启 */
    printf("SC|D9|mark_load_on\r\n");
    for (w = 0U; (w < 2500U) && (d9_cnt < D9_SAMPLES); w++) { rtos_task_delay(1U); }
    printf("SC|D9|mark_loop_end|cnt=%lu|w=%lu\r\n", (unsigned long)d9_cnt, (unsigned long)w);
    const uint32_t load_max = d9_max_late_ns;
    const uint32_t load_cnt = d9_cnt;
    kern_timer_isr_stop();
    printf("SC|D9|mark_stopped\r\n");
    d9_load = 0U;
    printf("SC|D9|baseline_max_late_ns|value=%lu|samples=%lu\r\n", (unsigned long)idle_max,
           (unsigned long)idle_cnt);
    printf("SC|D9|loaded_max_late_ns|value=%lu|samples=%lu\r\n", (unsigned long)load_max,
           (unsigned long)load_cnt);

    /* 分位数: 只看 max 无法判断分布形态。插入排序 4~5k 样本, 一次性诊断可接受。 */
    {
        const uint32_t hn = d9_hist_n;
        if (hn > 0U) {
            for (uint32_t a = 1U; a < hn; a++) {
                const uint32_t key = d9_hist[a];
                uint32_t b = a;
                while ((b > 0U) && (d9_hist[b - 1U] > key)) { d9_hist[b] = d9_hist[b - 1U]; b--; }
                d9_hist[b] = key;
            }
            printf("SC|D9|late_ns_percentiles|n=%lu|p50=%lu|p99=%lu|p99_9=%lu\r\n",
                   (unsigned long)hn, (unsigned long)d9_hist[(hn * 50U) / 100U],
                   (unsigned long)d9_hist[(hn * 99U) / 100U],
                   (unsigned long)d9_hist[(hn * 999U) / 1000U]);
        }
        printf("SC|D9|control_known_bad_ns|value=%lu|ctrl_hold_ns=%lu\r\n",
               (unsigned long)d9_bad_late_ns,
               (unsigned long)((d9_ctrl_hold_cyc * 1000U) / 168U));
    }
    printf("SC|D9|dbg|basepri=0x%02lX|tim6_ip=0x%02lX|aircr=0x%08lX|isr_inside=%lu\r\n",
           (unsigned long)d9_dbg_basepri, (unsigned long)d9_dbg_tim6_prio,
           (unsigned long)d9_dbg_aircr, (unsigned long)d9_ctrl_isr_inside);
    printf("SC|D9|dbg2|basepri=0x%02lX|tim6_ip=0x%02lX|aircr=0x%08lX\r\n",
           (unsigned long)d9_dbg_basepri, (unsigned long)d9_dbg_tim6_prio,
           (unsigned long)d9_dbg_aircr);
    printf("SC|D9|verdict|value=%s|unit=text\r\n",
           (load_max < 100000U) ? "measured" : "suspiciously_large");

    if (ld != NULL) { (void)rtos_task_delete(ld); }
    test_log("D9 done");
    return 0;
}

/* ============================== D10: sched_suspend 期间阻塞 API 的行为 ============================== */

/*
 * 契约(修复后, 见 rtos_sched.h 的 rtos_sched_suspend 说明):
 *   挂起调度器后, **任何会阻塞的 API 都不得阻塞**:
 *     - rtos_sem_take / rtos_queue_recv / rtos_task_notify_wait  -> RTOS_ERR_SCHED(-9)
 *     - rtos_event_wait                                          -> 0
 *     - rtos_task_delay(>0)                                      -> 立即返回(void)
 *   非阻塞路径不受影响(语义不能被误伤, 这是本场景的另一半判据)。
 *
 * 修复前的实测行为: 挂起后 rtos_task_delay(60) **永不返回**(永久挂死),
 * 见 docs/rtos_testing_report.md §10.1。本场景即该缺陷的回归用例。
 *
 * 关键: 全程不调用任何会阻塞的 API, 因此本场景自身在挂起期间也能跑完。
 */

static uint8_t  d10_q_storage[4U * 4U];
static rtos_sem_t   d10_sem;
static rtos_queue_t d10_q;
static rtos_event_t d10_ev;

/** @brief 运行 D10: 验证挂起期间的阻塞拒绝与非阻塞放行。 */
int32_t diag_sched_suspend(void)
{
    rtos_status_t r;
    uint32_t      v = 0U;

    test_log("D10 start: blocking APIs must be refused while the scheduler is suspended");

    (void)rtos_sem_init(&d10_sem, 0U, 1U);                        /* 空信号量 */
    (void)rtos_queue_init(&d10_q, d10_q_storage, 4U, 4U);         /* 空队列 */
    (void)rtos_event_init(&d10_ev);                               /* 无位 */

    const rtos_tick_t t_before = rtos_sched_get_tick_count();

    /* ---------- 进入挂起 ---------- */
    rtos_sched_suspend();
    printf("SC|D10|state_after_suspend|value=%lu|expect=2|unit=enum\r\n",
           (unsigned long)rtos_sched_get_state());

    /* ---------- A) 会阻塞的 API 必须被拒绝 ---------- */
    r = rtos_sem_take(&d10_sem, 100U);                     /* 空 sem + 超时 -> 应拒绝 */
    printf("SC|D10|sem_take_blocking|value=%ld|expect=-9|%s\r\n", (long)r,
           (r == RTOS_ERR_SCHED) ? "PASS" : "FAIL");

    r = rtos_queue_recv(&d10_q, &v, 100U);                 /* 空队列 + 超时 -> 应拒绝 */
    printf("SC|D10|queue_recv_blocking|value=%ld|expect=-9|%s\r\n", (long)r,
           (r == RTOS_ERR_SCHED) ? "PASS" : "FAIL");

    r = rtos_queue_send(&d10_q, &v, 100U, RTOS_FALSE);     /* 队列未满 -> 走非阻塞路径 */
    printf("SC|D10|queue_send_nonblocking|value=%ld|expect=0|%s\r\n", (long)r,
           (r == RTOS_OK) ? "PASS" : "FAIL");

    r = rtos_task_notify_wait(100U, &v);                   /* 无待处理通知 + 超时 -> 应拒绝 */
    printf("SC|D10|notify_wait_blocking|value=%ld|expect=-9|%s\r\n", (long)r,
           (r == RTOS_ERR_SCHED) ? "PASS" : "FAIL");

    {
        const rtos_event_bits_t e = rtos_event_wait(&d10_ev, 0x01U, RTOS_EVENT_WAIT_ANY, 100U);
        printf("SC|D10|event_wait_blocking|value=%lu|expect=0|%s\r\n", (unsigned long)e,
               (e == 0U) ? "PASS" : "FAIL");
    }

    /* ---------- B) 非阻塞路径不得被误伤 ---------- */
    r = rtos_sem_take(&d10_sem, RTOS_NO_WAIT);             /* 空 sem + NO_WAIT -> ERR_TIMEOUT */
    printf("SC|D10|sem_take_nowait|value=%ld|expect=-3|%s\r\n", (long)r,
           (r == RTOS_ERR_TIMEOUT) ? "PASS" : "FAIL");

    r = rtos_sem_give(&d10_sem);                           /* give 不受影响 */
    printf("SC|D10|sem_give_while_suspended|value=%ld|expect=0|%s\r\n", (long)r,
           (r == RTOS_OK) ? "PASS" : "FAIL");

    r = rtos_sem_take(&d10_sem, 100U);                     /* 计数充足 -> 走快路径, 应成功 */
    printf("SC|D10|sem_take_count_available|value=%ld|expect=0|%s\r\n", (long)r,
           (r == RTOS_OK) ? "PASS" : "FAIL");

    /* ---------- C) 确认没有卡死: tick 停走但本场景仍在执行 ---------- */
    {
        const rtos_tick_t t_after = rtos_sched_get_tick_count();
        printf("SC|D10|tick_frozen_while_suspended|delta=%lu|expect=0|%s\r\n",
               (unsigned long)(t_after - t_before),
               ((t_after - t_before) == 0U) ? "PASS" : "FAIL");
    }

    /* ---------- D) 恢复后阻塞必须重新可用 ---------- */
    rtos_sched_resume();
    printf("SC|D10|state_after_resume|value=%lu|expect=1|unit=enum\r\n",
           (unsigned long)rtos_sched_get_state());

    /* 恢复后 tick 应重新推进(用一次 yield 让 tick 有机会跑) */
    rtos_task_yield();
    rtos_task_yield();

    r = rtos_sem_take(&d10_sem, 5U);                       /* 空 sem + 短超时 -> 应正常超时(-3) */
    printf("SC|D10|sem_take_after_resume|value=%ld|expect=-3|%s\r\n", (long)r,
           (r == RTOS_ERR_TIMEOUT) ? "PASS" : "FAIL");

    /* 能走到这里本身就证明"挂起后阻塞"不再导致永久挂死 */
    printf("SC|D10|verdict|value=%s|unit=text\r\n", "REACHED_END");
    test_log("D10 done");
    return 0;
}
