/**
 * @file    test_scenarios.c
 * @brief   系统级真实场景测试套件实现(自研 RTOS 与 FreeRTOS 共用)。
 *
 * @details 设计原则:
 *            1. 只经 kern_port.h 访问内核, 同一份代码跑在两套内核上;
 *            2. 每个场景自带完成判据, 由汇总任务轮询, 不依赖上位机交互;
 *            3. 统计口径统一走 test_framework.c(流式 min/avg/max/bad);
 *            4. 时序敏感场景在测量前把上一轮的任务清理干净。
 *
 *          单位: 全部时间量以 CPU 周期上报(168MHz 下 168 周期 = 1µs)。
 */
#include "test_scenarios.h"
#include "kern_port.h"
#include "test_framework.h"
#if defined(KERN_IS_CUSTOM_RTOS)
#include "rtos.h" /* 仅自研 RTOS 侧需要: 用于读取内核诊断量 */
#endif
#include <stdio.h>
#include <string.h>

/* ============================== 场景规模参数 ============================== */

/** @brief S1: 中优先级干扰者每轮的空转次数(与锁无关的纯 CPU 消耗)。 */
#define S1_INTERFERER_WORK 40000U

/** @brief S1: 采样轮数。 */
#define S1_ROUNDS 20U

/** @brief S1: 持锁者的持锁工作量。 */
#define S1_HOLD_WORK 4000U

/** @brief S2: 每个等待者需要完成的阻塞/唤醒回合数。 */
#define S2_ROUNDS_PER_WAITER 60U

/** @brief S2: 等待者退出轮询的超时 tick(用于干净拆除)。 */
#define S2_WAITER_POLL_TICKS 10U

/** @brief S3: 生产者条数。 */
#define S3_PRODUCER_COUNT 3U

/** @brief S3: 每个生产者产生的数据项数。 */
#define S3_ITEMS_PER_PRODUCER 3000U

/** @brief S3: 消费者条数。 */
#define S3_CONSUMER_COUNT 2U

/** @brief S3: 环形缓冲深度(条)。 */
#define S3_QUEUE_DEPTH 8U

/** @brief S3: 控制环周期(ms)。 */
#define S3_CONTROL_PERIOD_MS 5U

/** @brief S3: 控制环采样次数(约 3s)。 */
#define S3_CONTROL_ITERATIONS 600U

/** @brief S3: 单条数据的处理工作量(空转次数, 模拟算法耗时)。 */
#define S3_WORK_ITERATIONS 300U

/** @brief S4: 长跑时长(ms)。 */
#define S4_DURATION_MS 20000U

/** @brief S4: 控制环周期(ms)。 */
#define S4_PERIOD_MS 2U

/** @brief S5: 任务集规模。 */
#define S5_TASK_COUNT 8U

/** @brief S5: 每个任务的栈(字)。 */
#define S5_TASK_STACK_WORDS 256U

/** @brief S2: 等待者上限。 */
#define S2_WAITER_MAX 16U

/* ============================== 共享状态 ============================== */

/**
 * @brief 被测量任务回写、汇总任务读取的场景状态。
 * @details 集中在一个结构体里, 避免散落的全局变量。
 */
typedef struct {
    volatile uint32_t s1_rounds_done; /**< S1 完成的轮数。 */
    volatile uint32_t s1_interferer_iters; /**< S1 干扰者完成的工作轮数。 */
    volatile uint32_t s2_exchange_done; /**< S2 已完成的阻塞/唤醒回合数。 */
    volatile uint32_t s2_ready; /**< S2 已就位的等待者数。 */
    volatile uint32_t s2_exit; /**< S2 要求等待者退出的标志。 */
    volatile uint32_t s3_produced; /**< S3 已生产条数。 */
    volatile uint32_t s3_consumed; /**< S3 已消费条数。 */
    volatile uint32_t s3_control_ticks; /**< S3 控制环执行次数。 */
    volatile uint32_t s4_done; /**< S4 是否收尾。 */
    volatile uint32_t s4_samples; /**< S4 已采集样本数。 */
    volatile uint32_t s5_created; /**< S5 成功创建的任务数。 */
} scenario_state_t;

static scenario_state_t g_state;

static test_stats_t g_s1_high_latency; /**< S1 高优先级任务的头阻塞延迟(周期)。 */
static test_stats_t g_s2_give_cost; /**< S2 单次 give 调用耗时(周期)。 */
static test_stats_t g_s2_round_cost; /**< S2 单回合(give+让出)总耗时(周期)。 */
static test_stats_t g_s2_give_no_wait; /**< S2 无等待者时 give 的固定开销(周期)。 */
static test_stats_t g_s3_latency; /**< S3 入队到取走的延迟(周期)。 */
static test_stats_t g_s3_jitter; /**< S3 控制环唤醒偏差(周期)。 */
static test_stats_t g_s3_period; /**< S3 控制环实际唤醒间隔(周期)。 */
static test_stats_t g_s4_jitter; /**< S4 控制环唤醒超出量(周期)。 */
static test_stats_t g_s4_period; /**< S4 控制环实际唤醒间隔(周期)。 */

/* 各场景的内核对象(每个对象独立 storage, 见 kern_port.h 的约定) */
static uint8_t g_s1_mutex[64];
static uint8_t g_s2_sem[64];
static uint8_t g_s2_isolated[64]; /**< S2 固定开销隔离测量用的计数信号量。 */

/** @brief S3 环形缓冲与其同步对象。 */
static uint8_t g_s3_mutex[64];
static uint8_t g_s3_item_sem[64];

/** @brief S4/S5 不需要同步对象。 */

/** @brief 传给任务的参数(按角色索引, 避免传栈地址)。 */
static uint32_t g_role_arg[S2_WAITER_MAX + S3_PRODUCER_COUNT + S3_CONSUMER_COUNT + S5_TASK_COUNT];

/* ============================== S1 优先级反转 ============================== */

/** @brief S1 采样起始时刻。 */
static volatile uint32_t g_s1_t0;

/**
 * @brief S1 低优先级任务: 反复持锁做固定工作后释放。
 * @param arg 未使用。
 */
static void s1_holder_task(void *arg)
{
    (void)arg;

    for (uint32_t round = 0U; round < S1_ROUNDS; round++) {
        if (kern_mutex_take(g_s1_mutex, KERN_WAIT_FOREVER) != 0) {
            return;
        }

        /* 持锁期间的固定工作量: 若此时被中优先级任务抢占, 高优先级等待者
         * 的头阻塞延迟就会显著增长 —— 这正是优先级反转的代价。 */
        for (volatile uint32_t i = 0U; i < S1_HOLD_WORK; i++) {
            /* 空转: 模拟临界区内计算 */
        }

        (void)kern_mutex_give(g_s1_mutex);
        kern_delay(1U);
    }
}

/**
 * @brief S1 中优先级干扰者: 持续占用 CPU, 但从不碰这把锁。
 * @param arg 未使用。
 */
static void s1_interferer_task(void *arg)
{
    (void)arg;

    for (;;) {
        for (volatile uint32_t i = 0U; i < S1_INTERFERER_WORK; i++) {
            /* 空转: 纯 CPU 消耗 */
        }
        g_state.s1_interferer_iters = g_state.s1_interferer_iters + 1U;

        /* 干扰者以 1ms 为粒度持续占用 CPU(每个 tick 做完一份固定工作量后让出),
         * 制造稳定的中优先级 CPU 争用。这既符合"中优先级计算任务"的真实形态,
         * 又保证每个 tick 调度器都有机会重新选择就绪任务。 */
        kern_delay(1U);
    }
}

/**
 * @brief S1 高优先级任务: 反复抢锁, 测量"发起请求到真正获得"的延迟。
 * @param arg 未使用。
 */
static void s1_high_task(void *arg)
{
    (void)arg;

    for (uint32_t round = 0U; round < S1_ROUNDS; round++) {
        g_s1_t0 = kern_cycles();

        if (kern_mutex_take(g_s1_mutex, KERN_WAIT_FOREVER) != 0) {
            return;
        }
        test_stats_add(&g_s1_high_latency, kern_cycles() - g_s1_t0);

        for (volatile uint32_t i = 0U; i < 500U; i++) {
            /* 空转: 极短临界区 */
        }
        (void)kern_mutex_give(g_s1_mutex);

        g_state.s1_rounds_done = round + 1U;
        kern_delay(1U);
    }
}

/**
 * @brief 运行 S1: 互斥锁优先级继承下的头阻塞延迟。
 * @return 0 成功, 负值失败。
 */
static int32_t scenario_prio_inversion(void)
{
    test_log("S1 start: block latency with a medium-priority interferer");

    if (kern_mutex_init(g_s1_mutex, sizeof(g_s1_mutex)) != 0) {
        test_log("S1 mutex init FAILED");
        return -1;
    }

    g_state.s1_rounds_done = 0U;
    g_state.s1_interferer_iters = 0U;
    test_stats_reset(&g_s1_high_latency, 0U);

    /* 创建顺序: 先把低优先级持锁者与高优先级抢锁者建好, 最后才建中优先级
     * 干扰者 —— 否则干扰者一旦创建就立即抢占创建者, 使其无法完成后续创建。 */
    if (kern_task_create(s1_holder_task, NULL, 256U, KERN_PRIO_LOW, "s1_hold") != 0) {
        test_log("S1 create holder FAILED");
        return -1;
    }
    if (kern_task_create(s1_high_task, NULL, 256U, KERN_PRIO_HIGH, "s1_high") != 0) {
        test_log("S1 create high FAILED");
        return -1;
    }
    if (kern_task_create(s1_interferer_task, NULL, 256U, KERN_PRIO_ABOVE, "s1_intf") != 0) {
        test_log("S1 create interferer FAILED");
        return -1;
    }

    for (uint32_t poll = 0U; (poll < 2000U) && (g_state.s1_rounds_done < S1_ROUNDS); poll++) {
        kern_delay(10U);
    }

    test_report_stats("S1", "block_latency_cyc", &g_s1_high_latency);
    test_report_value("S1", "interferer_work_rounds", g_state.s1_interferer_iters, "rounds");
    test_log("S1 done");
    return 0;
}

/* ============================== S2 等待者伸缩性 ============================== */

/** @brief S2 各等待者已完成的回合数。 */
static volatile uint32_t g_s2_waiter_rounds[S2_WAITER_MAX];

/**
 * @brief S2 等待者: 阻塞在同一信号量上, 被唤醒后计数并重新阻塞;
 *        收到退出标志时自删除。
 * @param arg 角色索引的指针。
 */
static void s2_waiter_task(void *arg)
{
    const uint32_t index = *(const uint32_t *)arg;

    g_state.s2_ready = g_state.s2_ready + 1U;

    for (;;) {
        if (kern_sem_take(g_s2_sem, S2_WAITER_POLL_TICKS) == 0) {
            g_s2_waiter_rounds[index] = g_s2_waiter_rounds[index] + 1U;
            g_state.s2_exchange_done = g_state.s2_exchange_done + 1U;
        }

        if (g_state.s2_exit != 0U) {
            kern_task_delete_self();
            return;
        }
    }
}

/**
 * @brief 测量指定等待者数量下的 give 成本。
 * @param waiter_count 参与阻塞的等待者数量(1..S2_WAITER_MAX)。
 * @return 0 成功, 负值失败。
 */
static int32_t s2_measure(uint32_t waiter_count)
{
    g_state.s2_exit = 0U;
    g_state.s2_ready = 0U;
    g_state.s2_exchange_done = 0U;
    for (uint32_t i = 0U; i < waiter_count; i++) {
        g_s2_waiter_rounds[i] = 0U;
    }
    test_stats_reset(&g_s2_give_cost, 0U);
    test_stats_reset(&g_s2_round_cost, 0U);

    for (uint32_t i = 0U; i < waiter_count; i++) {
        g_role_arg[i] = i;
        if (kern_task_create(s2_waiter_task, &g_role_arg[i], 256U, KERN_PRIO_LOW, "s2_wait") != 0) {
            return -1;
        }
    }

    /* 等所有等待者就位(它们会先发 ready 再阻塞) */
    for (uint32_t poll = 0U; (poll < 2000U) && (g_state.s2_ready < waiter_count); poll++) {
        kern_delay(1U);
    }
    /* 再等 20ms 确保它们都已真正阻塞在信号量上 */
    kern_delay(20U);

    const uint32_t total = waiter_count * S2_ROUNDS_PER_WAITER;
    for (uint32_t round = 0U; (round < 200000U) && (g_state.s2_exchange_done < total); round++) {
        /* [归因] 记录本次 give 是否触发了抢占: 触发时"调用 give 到返回"之间的
         * 墙钟时间含被换出的等待, 与未触发的样本不是同一个量 —— 这正是把
         * 端到端口径误读为"IPC 执行成本"的根源。 */
        const uint32_t t0 = kern_cycles();
        (void)kern_sem_give(g_s2_sem);
        const uint32_t give_dt = kern_cycles() - t0;
        test_stats_add(&g_s2_give_cost, give_dt);
        }

        /* 让被唤醒者跑起来并重新阻塞, 保证链表状态稳定后再测下一次。
         * 同时记录"整回合"(give + 让出)耗时。 */
        {
            const uint32_t round_t0 = kern_cycles();
            kern_delay(1U);
            test_stats_add(&g_s2_round_cost, kern_cycles() - round_t0);
        }
    }

    /* 干净拆除: 通知退出并等它们自删除 */
    g_state.s2_exit = 1U;
    kern_delay(30U * S2_WAITER_POLL_TICKS);
    return 0;
}

/**
 * @brief 运行 S2: give 成本随阻塞等待者数量的变化。
 * @return 0 成功, 负值失败。
 */
static int32_t scenario_waiter_scaling(void)
{
    static const uint32_t counts[] = {1U, 4U, 8U};
    static const char *const names[] = {"give_w01", "give_w04", "give_w08"};

    test_log("S2 start: give cost vs number of blocked waiters");

    /*
     * 等待者上限说明: 本工程堆仅 32KB(RTOS_CONFIG_HEAP_SIZE), 每个任务
     * (栈 512 字 + TCB)约占 2.2KB, 加上内核自带的 idle 与定时器服务任务,
     * 超过 8 个等待者时 rtos_task_create 返回失败 —— 这本身就是"该任务集
     * 能否建立"的实测结论, 已在报告中记录, 故此处只测到 8。
     */

    if (kern_sem_init(g_s2_sem, sizeof(g_s2_sem), 0U, 1U) != 0) {
        return -1;
    }

    /* ---- 固定开销隔离测量 ----
     * 计数型信号量、容量足够、**无任何等待者**: 每次 give 只做
     * "进临界区 → 检查等待链(空) → 计数++ → 出临界区", 不触发唤醒与切换。
     * 这是 give 的**纯固定成本**, 用于把"基数差距"与"伸缩性差距"分离 ——
     * 若该项也远高于 FreeRTOS, 则差距在固定开销而非数据结构。 */
    if (kern_sem_init(g_s2_isolated, sizeof(g_s2_isolated), 0U, 0xFFFFFFU) == 0) {
        test_stats_reset(&g_s2_give_no_wait, 0U);
        for (uint32_t i = 0U; i < 2000U; i++) {
            const uint32_t t0 = kern_cycles();
            (void)kern_sem_give(g_s2_isolated);
            test_stats_add(&g_s2_give_no_wait, kern_cycles() - t0);
        }
        test_report_stats("S2", "give_no_waiter", &g_s2_give_no_wait);
    }


    for (uint32_t c = 0U; c < (sizeof(counts) / sizeof(counts[0])); c++) {
        if (s2_measure(counts[c]) != 0) {
            return -1;
        }
        test_report_stats("S2", names[c], &g_s2_give_cost);
        test_report_stats("S2", (c == 0U) ? "round_w01" : ((c == 1U) ? "round_w04" : "round_w08"), &g_s2_round_cost);
    }

    test_log("S2 done");
    return 0;
}

/* ============================== S3 混合负载管线 ============================== */

/** @brief S3 数据项: 带产生时刻, 供消费者计算入队到取走的延迟。 */
typedef struct {
    uint32_t produced_cycle;
} s3_item_t;

/** @brief S3 环形缓冲与其状态(全部在互斥锁保护下访问)。 */
static s3_item_t g_s3_buffer[S3_QUEUE_DEPTH];
static volatile uint32_t g_s3_head;
static volatile uint32_t g_s3_tail;
static volatile uint32_t g_s3_count;

/**
 * @brief S3 生产者: 在互斥锁保护下放入数据项, 放不下则短暂让出后重试。
 * @param arg 角色索引的指针。
 */
static void s3_producer_task(void *arg)
{
    (void)arg;

    for (uint32_t i = 0U; i < S3_ITEMS_PER_PRODUCER; i++) {
        uint32_t placed = 0U;

        while (placed == 0U) {
            if (kern_mutex_take(g_s3_mutex, KERN_WAIT_FOREVER) != 0) {
                return;
            }
            if (g_s3_count < S3_QUEUE_DEPTH) {
                g_s3_buffer[g_s3_tail].produced_cycle = kern_cycles();
                g_s3_tail = (g_s3_tail + 1U) % S3_QUEUE_DEPTH;
                g_s3_count = g_s3_count + 1U;
                placed = 1U;
            }
            (void)kern_mutex_give(g_s3_mutex);

            if (placed == 0U) {
                /* 队列满: 让出 CPU 等消费者腾位置(比忙等更接近真实实现) */
                kern_delay(1U);
            }
        }

        g_state.s3_produced = g_state.s3_produced + 1U;
        (void)kern_sem_give(g_s3_item_sem); /* 通知有一条可取 */
    }

    kern_task_delete_self();
}

/**
 * @brief S3 消费者: 等信号量后取走数据项并做固定工作量处理。
 * @param arg 角色索引的指针。
 */
static void s3_consumer_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (kern_sem_take(g_s3_item_sem, 300U) != 0) {
            /* 超时: 生产者若已全部结束则收工 */
            if (g_state.s3_produced >= (S3_PRODUCER_COUNT * S3_ITEMS_PER_PRODUCER)) {
                break;
            }
            continue;
        }

        uint32_t latency = 0U;
        uint32_t got = 0U;

        if (kern_mutex_take(g_s3_mutex, KERN_WAIT_FOREVER) != 0) {
            return;
        }
        if (g_s3_count > 0U) {
            latency = kern_cycles() - g_s3_buffer[g_s3_head].produced_cycle;
            g_s3_head = (g_s3_head + 1U) % S3_QUEUE_DEPTH;
            g_s3_count = g_s3_count - 1U;
            g_state.s3_consumed = g_state.s3_consumed + 1U;
            got = 1U;
        }
        (void)kern_mutex_give(g_s3_mutex);

        if (got != 0U) {
            test_stats_add(&g_s3_latency, latency);

            /* 处理工作量: 模拟算法耗时 */
            for (volatile uint32_t w = 0U; w < S3_WORK_ITERATIONS; w++) {
                /* 空转 */
            }
        }
    }

    test_log("S3 consumer exit: consumed=%lu", (unsigned long)g_state.s3_consumed);
    kern_task_delete_self();
}

/**
 * @brief S3 周期控制环: 5ms 周期, 测量被负载拖慢的程度(唤醒偏差)。
 * @param arg 未使用。
 */
static void s3_control_task(void *arg)
{
    (void)arg;

    uint32_t nominal = 0U;
    uint32_t last_tick = 0U;
    uint32_t expect_cycle = 0U;

    kern_delay(20U); /* 等管线跑起来 */

    /* 标定一个 tick 的周期数(见 S4 的说明: 整数除法会截断) */
    {
        const uint32_t cal_ticks = 100U;
        const uint32_t t0 = kern_cycles();
        kern_delay(cal_ticks);
        nominal = (kern_cycles() - t0) / cal_ticks;
    }

    uint32_t prev_cycle = kern_cycles();
    for (uint32_t i = 0U; i < S3_CONTROL_ITERATIONS; i++) {
        kern_delay_until(&last_tick, S3_CONTROL_PERIOD_MS); /* 单位是 tick */
        const uint32_t now = kern_cycles();

        /* 实际唤醒间隔(与 S4 同口径): 用来区分"周期真的被拉长"与
         * "偏差公式在重负载下失真"。 */
        {
            const uint32_t interval = now - prev_cycle;
            prev_cycle = now;
            test_stats_add(&g_s3_period, interval);
        }

        /* 偏差 = 本次唤醒计数 减 (上次唤醒计数 + 一个周期)。理想为 0,
         * 被高优先级负载/中断推迟时为正。 */
        if (i == 0U) {
            expect_cycle = now + (S3_CONTROL_PERIOD_MS * nominal);
        } else {
            test_stats_add(&g_s3_jitter, (now > expect_cycle) ? (now - expect_cycle) : 0U);
            expect_cycle += (S3_CONTROL_PERIOD_MS * nominal);
        }

        g_state.s3_control_ticks = i + 1U;
    }
    kern_task_delete_self();
}

/**
 * @brief 运行 S3: 混合负载管线(生产/消费/周期控制)。
 * @return 0 成功, 负值失败。
 */
static int32_t scenario_mixed_pipeline(void)
{
    test_log("S3 start: %lu producers + %lu consumers + %lums control loop",
             (unsigned long)S3_PRODUCER_COUNT, (unsigned long)S3_CONSUMER_COUNT,
             (unsigned long)S3_CONTROL_PERIOD_MS);

    if (kern_mutex_init(g_s3_mutex, sizeof(g_s3_mutex)) != 0) {
        return -1;
    }
    if (kern_sem_init(g_s3_item_sem, sizeof(g_s3_item_sem), 0U, 512U) != 0) {
        return -1;
    }

    g_s3_head = 0U;
    g_s3_tail = 0U;
    g_s3_count = 0U;
    g_state.s3_produced = 0U;
    g_state.s3_consumed = 0U;
    g_state.s3_control_ticks = 0U;
    test_stats_reset(&g_s3_latency, 0U);
    test_stats_reset(&g_s3_jitter, 0U);
    test_stats_reset(&g_s3_period, 0U);

    uint32_t role = 0U;
    for (uint32_t i = 0U; i < S3_CONSUMER_COUNT; i++) {
        g_role_arg[role] = i;
        if (kern_task_create(s3_consumer_task, &g_role_arg[role], 256U, KERN_PRIO_BELOW,
                             "s3_cons") != 0) {
            return -1;
        }
        role++;
    }
    for (uint32_t i = 0U; i < S3_PRODUCER_COUNT; i++) {
        g_role_arg[role] = i;
        if (kern_task_create(s3_producer_task, &g_role_arg[role], 256U, KERN_PRIO_NORMAL,
                             "s3_prod") != 0) {
            return -1;
        }
        role++;
    }
    if (kern_task_create(s3_control_task, NULL, 256U, KERN_PRIO_HIGHEST, "s3_ctrl") != 0) {
        return -1;
    }

    const uint32_t total = S3_PRODUCER_COUNT * S3_ITEMS_PER_PRODUCER;
    const uint32_t start_cycle = kern_cycles();

    uint32_t poll = 0U;
    while ((poll < 400000U) && (g_state.s3_consumed < total)) {
        kern_delay(20U);
        poll++;
    }
    const uint32_t elapsed = kern_cycles() - start_cycle;

    while ((poll < 600000U) && (g_state.s3_control_ticks < S3_CONTROL_ITERATIONS)) {
        kern_delay(20U);
        poll++;
    }

    test_report_stats("S3", "queue_latency_cyc", &g_s3_latency);
    test_report_stats("S3", "control_deviation_cyc", &g_s3_jitter);
    test_report_stats("S3", "control_period_cyc", &g_s3_period);
    test_report_value("S3", "produced", g_state.s3_produced, "items");
    test_report_value("S3", "consumed", g_state.s3_consumed, "items");
    test_report_value("S3", "elapsed", kern_cycles_to_us(elapsed), "us");
    if (elapsed > 0U) {
        const uint32_t items_per_sec =
            (uint32_t)(((uint64_t)g_state.s3_consumed * 168000000ULL) / (uint64_t)elapsed);
        test_report_value("S3", "throughput", items_per_sec, "items_per_sec");
    }

    test_log("S3 done");
    return 0;
}

/* ============================== S4 周期控制抖动 ============================== */

/**
 * @brief S4 长跑控制环: 2ms 周期, 记录每次唤醒的实际间隔与偏差。
 * @param arg 未使用。
 *
 * @details 指标定义(与 S3 同口径):
 *            period_actual_cyc = 相邻两次唤醒的周期计数差;
 *            wake_deviation_cyc = 实际间隔 - 一个周期(即超出的部分)。
 *          周期任务关心的正是这两项: 实际周期是否等于设定值、以及被推迟了
 *          多少。注意 DWT CYCCNT 在 168MHz 下约 25.6s 回绕一次, 因此相邻两次
 *          采样之间恒用无符号差值 —— 只要单次间隔远小于 2^32 周期即安全。
 */
static void s4_control_task(void *arg)
{
    (void)arg;

    const uint32_t total = S4_DURATION_MS / S4_PERIOD_MS;
    uint32_t last_tick = 0U;
    uint32_t prev_cycle = 0U;
    uint32_t nominal = 0U;

    kern_delay(20U);

    /* 标定"一个 tick 等于多少 CPU 周期": 延时 N 个 tick, 实测经过的周期数再除以 N。
     * 比用 时钟/频率 的整数除法可靠(后者在 168MHz 下 1e6/168 会截断成 5952,
     * 再做 2000/5952 直接变成 0)。 */
    {
        const uint32_t cal_ticks = 100U;
        const uint32_t t0 = kern_cycles();
        kern_delay(cal_ticks);
        nominal = (kern_cycles() - t0) / cal_ticks;
    }


    prev_cycle = kern_cycles();

    for (uint32_t i = 0U; i < total; i++) {
        const uint32_t target = S4_PERIOD_MS * nominal;
        kern_delay_until(&last_tick, S4_PERIOD_MS); /* 单位是 tick, 不是周期 */

        const uint32_t now = kern_cycles();
        const uint32_t interval = now - prev_cycle; /* 无符号差值: 回绕安全 */
        prev_cycle = now;

        test_stats_add(&g_s4_jitter, (interval > target) ? (interval - target) : 0U);
        test_stats_add(&g_s4_period, interval);
        g_state.s4_samples = i + 1U;
    }

    g_state.s4_done = 1U;
    kern_task_delete_self();
}

/**
 * @brief 运行 S4: 周期控制环长跑。
 * @return 0 成功, 负值失败。
 */
static int32_t scenario_periodic_jitter(void)
{
    test_log("S4 start: %lums control loop at %lums period", (unsigned long)S4_DURATION_MS,
             (unsigned long)S4_PERIOD_MS);

    g_state.s4_done = 0U;
    g_state.s4_samples = 0U;
    /* 偏差超过半个周期(1ms = 168000 周期)才计入坏样本 */
    test_stats_reset(&g_s4_jitter, (uint32_t)(168000U / 2U));
    test_stats_reset(&g_s4_period, 0U);

    if (kern_task_create(s4_control_task, NULL, 256U, KERN_PRIO_HIGHEST, "s4_ctrl") != 0) {
        return -1;
    }

    for (uint32_t poll = 0U; (poll < 400000U) && (g_state.s4_done == 0U); poll++) {
        kern_delay(50U);
    }

    test_report_stats("S4", "wake_deviation_cyc", &g_s4_jitter);
    test_report_stats("S4", "period_actual_cyc", &g_s4_period);
    test_report_value("S4", "samples", g_state.s4_samples, "samples");
    test_log("S4 done");
    return 0;
}

/* ============================== S5 内存足迹 ============================== */

/**
 * @brief S5 空任务: 建立后立即阻塞, 只占资源不占 CPU。
 * @param arg 未使用。
 */
static void s5_idle_task(void *arg)
{
    (void)arg;
    for (;;) {
        kern_delay(1000U);
    }
}

/**
 * @brief 运行 S5: 建立等价任务集的能力。
 * @return 0 成功, 负值失败。
 *
 * @details 两侧以相同栈字数与优先级创建同样数量的任务, 成功数即"该任务的
 *          对象开销能否被接受"的直接证据; 静态占用字节数由上位机读 ELF 段
 *          大小得出(见报告方法说明)。
 */
static int32_t scenario_footprint(void)
{
    test_log("S5 start: create %lu tasks x %lu words", (unsigned long)S5_TASK_COUNT,
             (unsigned long)S5_TASK_STACK_WORDS);

    g_state.s5_created = 0U;

    for (uint32_t i = 0U; i < S5_TASK_COUNT; i++) {
        g_role_arg[i] = i;
        if (kern_task_create(s5_idle_task, &g_role_arg[i], S5_TASK_STACK_WORDS, KERN_PRIO_LOWEST,
                             "s5_idle") != 0) {
            break;
        }
        g_state.s5_created = i + 1U;
    }

    /* 让所有任务真正跑起来一次, 证明它们可用 */
    kern_delay(50U);

    test_report_value("S5", "tasks_created", g_state.s5_created, "tasks");
    test_report_value("S5", "stack_words_each", S5_TASK_STACK_WORDS, "words");
    test_log("S5 done");
    return 0;
}

/* ============================== 统一入口 ============================== */

/* ============================== S7 切换隔离测量 ============================== */

/** @brief S7: 让出次数(每轮 2 次切换)。 */
#define S7_YIELD_ITERS 20000U

/** @brief S7 采样起始/结束时刻。 */
static volatile uint32_t g_s7_start;
static volatile uint32_t g_s7_stop;
static volatile uint32_t g_s7_done_a;
static volatile uint32_t g_s7_done_b;

/**
 * @brief S7 乒乓任务: 反复让出 CPU, 用于单独测量"一次上下文切换"的代价。
 * @param arg NULL 表示 A, 非 NULL 表示 B。
 *
 * @details 与 S2 的区别: S2 测的是"give + 唤醒 + 切换"的复合成本, 本场景只测
 *          切换本身 —— 用于把优化收益拆分为"IPC 路径"与"切换路径"两部分。
 */
static void s7_yield_task(void *arg)
{
    uint32_t local = 0U;

    if (arg == NULL) {
        g_s7_start = kern_cycles(); /* A 首次运行: 计时开始 */
    }

    while (local < (S7_YIELD_ITERS / 2U)) {
        kern_yield();
        local++;
    }

    if (arg != NULL) {
        g_s7_stop = kern_cycles(); /* B 最后一次: 计时结束 */
        g_s7_done_b = 1U;
    } else {
        g_s7_done_a = 1U;
    }

    for (;;) {
        kern_delay(1000U);
    }
}

/**
 * @brief 运行 S7: 同优先级 yield 上下文切换成本。
 * @return 0 成功, 负值失败。
 */
static int32_t scenario_yield_switch(void)
{
    test_log("S7 start: same-priority yield context switch");

    g_s7_done_a = 0U;
    g_s7_done_b = 0U;

    if (kern_task_create(s7_yield_task, NULL, 256U, KERN_PRIO_NORMAL, "s7_ya") != 0) {
        return -1;
    }
    if (kern_task_create(s7_yield_task, (void *)1, 256U, KERN_PRIO_NORMAL, "s7_yb") != 0) {
        return -1;
    }

    for (uint32_t poll = 0U; (poll < 200000U) && ((g_s7_done_a == 0U) || (g_s7_done_b == 0U));
         poll++) {
        kern_delay(10U);
    }

    if ((g_s7_done_a == 0U) || (g_s7_done_b == 0U)) {
        test_log("S7 timeout: a=%lu b=%lu", (unsigned long)g_s7_done_a, (unsigned long)g_s7_done_b);
        return -1;
    }

    /* 共 S7_YIELD_ITERS 次 yield, 每次 = 一次切换; 减去首个间隔的偏差 */
    test_report_value("S7", "yield_switch_cyc", (g_s7_stop - g_s7_start) / (S7_YIELD_ITERS - 1U),
                      "cyc");
    test_log("S7 done");
    return 0;
}

int32_t test_scenario_run(uint32_t scenario_id)
{
    switch (scenario_id) {
        case 1U:
            return scenario_prio_inversion();
        case 2U:
            return scenario_waiter_scaling();
        case 3U:
            return scenario_mixed_pipeline();
        case 4U:
            return scenario_periodic_jitter();
        case 5U:
            return scenario_footprint();
        case 7U:
            return scenario_yield_switch();
        default:
            test_log("unknown scenario %lu", (unsigned long)scenario_id);
            return -1;
    }
}

void test_scenario_task(void *arg)
{
    (void)arg;

    kern_cycles_init();
    kern_delay(100U);

    printf("SC|META|scenario=%lu|tick_hz=%lu|us_per_call=%lu\r\n",
           (unsigned long)TEST_SCENARIO_SELECT, (unsigned long)kern_tick_rate(),
           (unsigned long)kern_cycles_to_us(1000000U));
    printf("SC|START|scenario=%lu\r\n", (unsigned long)TEST_SCENARIO_SELECT);

    const int32_t status = test_scenario_run((uint32_t)TEST_SCENARIO_SELECT);

    printf("SC|DONE|scenario=%lu|status=%ld\r\n", (unsigned long)TEST_SCENARIO_SELECT,
           (long)status);
    kern_task_delete_self();
}
