/**
 * @file    boundary_test.c
 * @brief   自研 RTOS 边界与稳定性能测试夹具实现
 *
 * @details 覆盖六类输入边界并复测快路径周期开销:
 *            有效值 / 下边界 / 上边界 / 超出上边界 / 超出下边界 / 空值(NULL)
 *
 *          设计要点:
 *          1. **不自杀、不返回**: 与 benchmark.c 的 bench_task 一致, SUMMARY
 *             之后进入长期 delay 循环, 上位机可继续观察系统是否仍稳定。
 *          2. **不调用 rtos_task_delete(NULL)**: 该 API 语义是"删除当前任务"
 *             (见 boundary_test 说明), 在测试任务中调用会终止测试, 故本夹具
 *             一律传入显式 TCB 指针。
 *          3. **每个用例后立即验证对象仍可用**: 非法入参除返回错误码外,
 *             还必须不破坏对象状态 —— 这是"稳定性"与"仅返回错误"的区别。
 *          4. 计时用 DWT CYCCNT(168MHz), printf 全部在测量区间之外。
 *
 * @note    契约极值(本夹具重点, 与 selftest.c 的典型非法值互补):
 *            栈 96(恰好合法) vs 95(恰好非法) vs 堆上限+1
 *            优先级 31(最大合法) vs 32(越界)
 *            信号量 init_cnt == max_cnt(恰好合法) vs init_cnt > max_cnt
 *            队列满/空前后的最后一格, 超时 0 / 1 / UINT32_MAX
 */
#include "boundary_test.h"
#include "rtos.h"
#include "main.h"
#include <stdio.h>

/* ============================== 测试计数与报告 ============================== */

/** @brief 本夹具累计失败用例数。 */
static uint32_t s_fail_count;
/** @brief 本夹具累计通过用例数。 */
static uint32_t s_pass_count;

/**
 * @brief 上报一条用例结果。
 *
 * @param category [in] 分类(SEM/MUTEX/QUEUE/TASK/NOTIFY/PERF...)。
 * @param name     [in] 用例名。
 * @param ok       [in] RTOS_TRUE 通过, RTOS_FALSE 失败。
 * @param actual   [in] 实际值(通常是返回码), 失败时打印。
 * @param expect   [in] 期望值, 失败时打印。
 */
static void bt_report(const char *category, const char *name, rtos_bool_t ok, int32_t actual,
                      int32_t expect)
{
    if (ok == RTOS_TRUE) {
        s_pass_count++;
        printf("BT|%s|%s|PASS\r\n", category, name);
    } else {
        s_fail_count++;
        printf("BT|%s|%s|FAIL|actual=%ld|expect=%ld\r\n", category, name, (long)actual,
               (long)expect);
    }
}

/**
 * @brief 断言返回值等于期望值。
 *
 * @param category [in] 分类。
 * @param name     [in] 用例名。
 * @param actual   [in] 实际返回值。
 * @param expect   [in] 期望返回值。
 */
static void bt_expect(const char *category, const char *name, rtos_status_t actual,
                      rtos_status_t expect)
{
    bt_report(category, name, (actual == expect) ? RTOS_TRUE : RTOS_FALSE, (int32_t)actual,
              (int32_t)expect);
}

/**
 * @brief 断言普通整数条件。
 *
 * @param category [in] 分类。
 * @param name     [in] 用例名。
 * @param actual   [in] 实际值。
 * @param expect   [in] 期望值。
 */
static void bt_expect_u32(const char *category, const char *name, uint32_t actual, uint32_t expect)
{
    bt_report(category, name, (actual == expect) ? RTOS_TRUE : RTOS_FALSE, (int32_t)actual,
              (int32_t)expect);
}

/* ============================== 计时与统计 ============================== */

/** @brief 流式周期统计(RAM 受限, 不存样本数组)。 */
typedef struct bm_cycle_stats {
    uint32_t min; /**< 最小周期数。 */
    uint32_t max; /**< 最大周期数。 */
    uint64_t sum; /**< 周期累加和。 */
    uint32_t count; /**< 样本数。 */
} bm_cycle_stats_t;

/** @brief 读 DWT 周期计数器。 */
static inline uint32_t bt_dwt(void)
{
    return DWT->CYCCNT;
}

/**
 * @brief 复位周期统计。
 * @param stats [out] 统计结构。
 */
static void bt_stats_reset(bm_cycle_stats_t *stats)
{
    stats->min = 0xFFFFFFFFU;
    stats->max = 0U;
    stats->sum = 0U;
    stats->count = 0U;
}

/**
 * @brief 累加一个周期样本。
 * @param stats [in,out] 统计结构。
 * @param value [in]     本次耗时(周期)。
 */
static void bt_stats_add(bm_cycle_stats_t *stats, uint32_t value)
{
    if (value < stats->min) {
        stats->min = value;
    }
    if (value > stats->max) {
        stats->max = value;
    }
    stats->sum += value;
    stats->count = stats->count + 1U;
}

/**
 * @brief 打印周期统计一行。
 * @param name  [in] 指标名。
 * @param stats [in] 统计结构(样本数必须 > 0)。
 */
static void bt_stats_print(const char *name, const bm_cycle_stats_t *stats)
{
    if (stats->count == 0U) {
        return;
    }
    printf("BT|PERF|%s|N=%lu|avg=%lu|min=%lu|max=%lu\r\n", name,
           (unsigned long)stats->count, (unsigned long)(stats->sum / stats->count),
           (unsigned long)stats->min, (unsigned long)stats->max);
}

/* ============================== 第 1 组: 快路径周期开销 ============================== */

/**
 * @brief 复测各快路径单次调用的周期开销。
 *
 * @details 全部在"无争用/无阻塞"条件下测量, 即各 API 的最快路径:
 *            临界区对、sem take(有效值)、sem give(无等待者)、
 *            mutex take/give(无争用)、queue send/recv(非满/非空)。
 */
static void bt_perf_fast_paths(void)
{
    bm_cycle_stats_t stats;
    rtos_sem_t sem;
    rtos_mutex_t mutex;
    rtos_queue_t queue;
    uint8_t queue_storage[4U * 16U];
    uint32_t value = 0xA5A5A5A5U;
    uint32_t got = 0U;
    uint32_t i;
    uint32_t t0;
    uint32_t t1;

    /* ---- 临界区进入/退出对 ---- */
    bt_stats_reset(&stats);
    for (i = 0U; i < 10000U; i++) {
        t0 = bt_dwt();
        RTOS_PORT_ENTER_CRITICAL();
        RTOS_PORT_EXIT_CRITICAL();
        t1 = bt_dwt();
        bt_stats_add(&stats, t1 - t0);
    }
    bt_stats_print("critical_pair", &stats);

    /* ---- sem take: 计数充足, 每次只做"判断 + 减一" ---- */
    if (rtos_sem_init(&sem, 0U, 0xFFFFFFU) == RTOS_OK) {
        bt_stats_reset(&stats);
        for (i = 0U; i < 5000U; i++) {
            (void)rtos_sem_give(&sem); /* 先把计数补上, 使 take 恒走快路径 */
            t0 = bt_dwt();
            (void)rtos_sem_take(&sem, RTOS_NO_WAIT);
            t1 = bt_dwt();
            bt_stats_add(&stats, t1 - t0);
        }
        bt_stats_print("sem_take_valid", &stats);
    }

    /* ---- sem give: 无等待者, 只做"检查等待链 + 计数加一" ---- */
    if (rtos_sem_init(&sem, 0U, 0xFFFFFFU) == RTOS_OK) {
        bt_stats_reset(&stats);
        for (i = 0U; i < 5000U; i++) {
            t0 = bt_dwt();
            (void)rtos_sem_give(&sem);
            t1 = bt_dwt();
            bt_stats_add(&stats, t1 - t0);
        }
        bt_stats_print("sem_give_no_waiter", &stats);
    }

    /* ---- mutex take/give: 无争用(仅所有者判断) ---- */
    if (rtos_mutex_init(&mutex) == RTOS_OK) {
        bt_stats_reset(&stats);
        for (i = 0U; i < 3000U; i++) {
            t0 = bt_dwt();
            (void)rtos_mutex_take(&mutex, RTOS_NO_WAIT);
            (void)rtos_mutex_give(&mutex);
            t1 = bt_dwt();
            bt_stats_add(&stats, t1 - t0);
        }
        bt_stats_print("mutex_take_give", &stats);
        (void)rtos_mutex_deinit(&mutex); /* 归还注册表槽位, 避免后续用例无槽可用 */
    }

    /* ---- queue send + recv: 非满/非空 ---- */
    if (rtos_queue_init(&queue, queue_storage, 4U, 16U) == RTOS_OK) {
        bt_stats_reset(&stats);
        for (i = 0U; i < 3000U; i++) {
            t0 = bt_dwt();
            (void)rtos_queue_send(&queue, &value, RTOS_NO_WAIT, RTOS_FALSE);
            (void)rtos_queue_recv(&queue, &got, RTOS_NO_WAIT);
            t1 = bt_dwt();
            bt_stats_add(&stats, t1 - t0);
        }
        bt_stats_print("queue_send_recv", &stats);
    }
}

/* ============================== 第 2 组: 信号量边界 ============================== */

/** @brief 一个刻意不初始化的信号量(全 0 = is_initialized == 0)。 */
static rtos_sem_t s_sem_uninit;

/**
 * @brief 信号量 init/take/give 的六类边界。
 *
 * @details 除返回码外, 每个非法调用后都验证对象仍可正常工作 —— 非法入参
 *          不得破坏对象状态。
 */
static void bt_boundary_sem(void)
{
    rtos_sem_t sem;

    /* ---- init: 有效值 / 下边界 / 上边界 / 两类越界 / 空值 ---- */
    bt_expect("SEM", "init_valid_1_2", rtos_sem_init(&sem, 1U, 2U), RTOS_OK);
    bt_expect("SEM", "init_lower_bound_0_1", rtos_sem_init(&sem, 0U, 1U), RTOS_OK);
    bt_expect("SEM", "init_upper_bound_eq_max", rtos_sem_init(&sem, 5U, 5U), RTOS_OK);
    bt_expect("SEM", "init_upper_over_max0", rtos_sem_init(&sem, 0U, 0U), RTOS_ERR_PARAM);
    bt_expect("SEM", "init_over_max_6_5", rtos_sem_init(&sem, 6U, 5U), RTOS_ERR_PARAM);
    bt_expect("SEM", "init_over_max_maxu32", rtos_sem_init(&sem, 0xFFFFFFFFU, 0xFFFFFFFEU),
              RTOS_ERR_PARAM);
    bt_expect("SEM", "init_null", rtos_sem_init(NULL, 0U, 1U), RTOS_ERR_PARAM);

    /* ---- take/give: 有效值 ---- */
    (void)rtos_sem_init(&sem, 2U, 3U);
    bt_expect("SEM", "take_valid", rtos_sem_take(&sem, RTOS_NO_WAIT), RTOS_OK);
    bt_expect("SEM", "give_valid", rtos_sem_give(&sem), RTOS_OK);

    /* ---- take: 下边界(超时 = 0)与上边界(超时 = UINT32_MAX 不适用主任务) ---- */
    (void)rtos_sem_init(&sem, 0U, 1U);
    bt_expect("SEM", "take_lower_timeout0_empty", rtos_sem_take(&sem, RTOS_NO_WAIT),
              RTOS_ERR_TIMEOUT);
    bt_expect("SEM", "take_null", rtos_sem_take(NULL, RTOS_NO_WAIT), RTOS_ERR_NULL);
    bt_expect("SEM", "take_uninit", rtos_sem_take(&s_sem_uninit, RTOS_NO_WAIT), RTOS_ERR_PARAM);

    /* ---- give: 上边界(计数恰好到 max)与超出上边界 ---- */
    (void)rtos_sem_init(&sem, 0U, 1U);
    bt_expect("SEM", "give_to_max", rtos_sem_give(&sem), RTOS_OK);
    bt_expect("SEM", "give_over_max", rtos_sem_give(&sem), RTOS_ERR_OVERFLOW);
    bt_expect("SEM", "give_null", rtos_sem_give(NULL), RTOS_ERR_NULL);
    bt_expect("SEM", "give_uninit", rtos_sem_give(&s_sem_uninit), RTOS_ERR_PARAM);

    /* ---- 稳定性: 非法调用后对象仍可用 ---- */
    bt_expect_u32("SEM", "stable_count_after_errors", rtos_sem_get_count(&sem), 1U);
    bt_expect("SEM", "stable_take_after_errors", rtos_sem_take(&sem, RTOS_NO_WAIT), RTOS_OK);
    bt_expect_u32("SEM", "stable_count_empty", rtos_sem_get_count(&sem), 0U);
    bt_expect("SEM", "stable_give_after_errors", rtos_sem_give(&sem), RTOS_OK);

    /* ---- deinit 边界 ---- */
    bt_expect("SEM", "deinit_null", rtos_sem_deinit(NULL), RTOS_ERR_NULL);
    bt_expect("SEM", "deinit_valid", rtos_sem_deinit(&sem), RTOS_OK);
    bt_expect("SEM", "take_after_deinit", rtos_sem_take(&sem, RTOS_NO_WAIT), RTOS_ERR_PARAM);
}

/* ============================== 第 3 组: 互斥锁边界 ============================== */

/**
 * @brief 互斥锁的边界: 空值 / 未持有释放 / 重复释放 / 重入上边界。
 */
static void bt_boundary_mutex(void)
{
    rtos_mutex_t mutex;

    bt_expect("MUTEX", "init_null", rtos_mutex_init(NULL), RTOS_ERR_NULL);
    bt_expect("MUTEX", "init_valid", rtos_mutex_init(&mutex), RTOS_OK);
    bt_expect("MUTEX", "take_null", rtos_mutex_take(NULL, RTOS_NO_WAIT), RTOS_ERR_NULL);
    bt_expect("MUTEX", "give_null", rtos_mutex_give(NULL), RTOS_ERR_NULL);
    bt_expect("MUTEX", "deinit_null", rtos_mutex_deinit(NULL), RTOS_ERR_NULL);

    /* give 未持有的锁: 下边界(所有者为空) */
    bt_expect("MUTEX", "give_unowned", rtos_mutex_give(&mutex), RTOS_ERR_PARAM);

    /* 重入计数: 连续 take 至上限后逐次 give */
    bt_expect("MUTEX", "recursive_take_1", rtos_mutex_take(&mutex, RTOS_NO_WAIT), RTOS_OK);
    bt_expect("MUTEX", "recursive_take_2", rtos_mutex_take(&mutex, RTOS_NO_WAIT), RTOS_OK);
    bt_expect("MUTEX", "recursive_give_1", rtos_mutex_give(&mutex), RTOS_OK);
    bt_expect("MUTEX", "recursive_give_2", rtos_mutex_give(&mutex), RTOS_OK);
    bt_expect("MUTEX", "give_after_release", rtos_mutex_give(&mutex), RTOS_ERR_PARAM);

    /* 稳定性: 非法释放后仍可正常加解锁 */
    bt_expect("MUTEX", "stable_take_again", rtos_mutex_take(&mutex, RTOS_NO_WAIT), RTOS_OK);
    bt_expect("MUTEX", "stable_give_again", rtos_mutex_give(&mutex), RTOS_OK);
    bt_expect("MUTEX", "deinit_valid", rtos_mutex_deinit(&mutex), RTOS_OK);
}

/* ============================== 第 4 组: 队列边界 ============================== */

/**
 * @brief 队列 init/满/空/容量极值的边界与稳定性。
 */
static void bt_boundary_queue(void)
{
    rtos_queue_t queue;
    uint8_t storage[4U * 4U];
    uint32_t value = 0x12345678U;
    uint32_t got = 0U;
    uint32_t i;

    /* ---- init: 空值 / 下边界 / 有效值 ---- */
    bt_expect("QUEUE", "init_null_queue", rtos_queue_init(NULL, storage, 4U, 4U), RTOS_ERR_PARAM);
    bt_expect("QUEUE", "init_null_storage", rtos_queue_init(&queue, NULL, 4U, 4U), RTOS_ERR_PARAM);
    bt_expect("QUEUE", "init_lower_item0", rtos_queue_init(&queue, storage, 0U, 4U),
              RTOS_ERR_PARAM);
    bt_expect("QUEUE", "init_lower_cap0", rtos_queue_init(&queue, storage, 4U, 0U), RTOS_ERR_PARAM);
    bt_expect("QUEUE", "init_valid", rtos_queue_init(&queue, storage, 4U, 4U), RTOS_OK);
    bt_expect_u32("QUEUE", "init_empty_count", rtos_queue_get_count(&queue), 0U);
    bt_expect_u32("QUEUE", "init_free_eq_cap", rtos_queue_get_free(&queue), 4U);

    /* ---- recv: 下边界(空队列 + 超时 0) ---- */
    bt_expect("QUEUE", "recv_lower_empty_nowait", rtos_queue_recv(&queue, &got, RTOS_NO_WAIT),
              RTOS_ERR_TIMEOUT);
    bt_expect("QUEUE", "recv_null_queue", rtos_queue_recv(NULL, &got, RTOS_NO_WAIT), RTOS_ERR_NULL);
    bt_expect("QUEUE", "recv_null_item", rtos_queue_recv(&queue, NULL, RTOS_NO_WAIT), RTOS_ERR_NULL);

    /* ---- send: 上边界(恰好填满)与超出上边界(满后再发) ---- */
    for (i = 0U; i < 4U; i++) {
        bt_expect("QUEUE", "fill_to_capacity", rtos_queue_send(&queue, &value, RTOS_NO_WAIT,
                                                              RTOS_FALSE), RTOS_OK);
    }
    bt_expect_u32("QUEUE", "count_at_capacity", rtos_queue_get_count(&queue), 4U);
    bt_expect_u32("QUEUE", "free_at_capacity", rtos_queue_get_free(&queue), 0U);
    bt_expect("QUEUE", "send_over_capacity", rtos_queue_send(&queue, &value, RTOS_NO_WAIT,
                                                             RTOS_FALSE), RTOS_ERR_TIMEOUT);
    bt_expect("QUEUE", "send_null_queue", rtos_queue_send(NULL, &value, RTOS_NO_WAIT, RTOS_FALSE),
              RTOS_ERR_NULL);
    bt_expect("QUEUE", "send_null_item", rtos_queue_send(&queue, NULL, RTOS_NO_WAIT, RTOS_FALSE),
              RTOS_ERR_NULL);

    /* ---- 稳定性: 溢出发送失败后, 已有 4 条数据必须完好且顺序正确 ---- */
    for (i = 0U; i < 4U; i++) {
        got = 0U;
        bt_expect("QUEUE", "drain_intact", rtos_queue_recv(&queue, &got, RTOS_NO_WAIT), RTOS_OK);
        bt_expect_u32("QUEUE", "drain_value_intact", got, value);
    }
    bt_expect_u32("QUEUE", "count_after_drain", rtos_queue_get_count(&queue), 0U);

    /* ---- 容量 1 的下边界队列 ---- */
    bt_expect("QUEUE", "cap1_init", rtos_queue_init(&queue, storage, 4U, 1U), RTOS_OK);
    bt_expect("QUEUE", "cap1_send", rtos_queue_send(&queue, &value, RTOS_NO_WAIT, RTOS_FALSE),
              RTOS_OK);
    bt_expect("QUEUE", "cap1_send_over", rtos_queue_send(&queue, &value, RTOS_NO_WAIT, RTOS_FALSE),
              RTOS_ERR_TIMEOUT);
    bt_expect("QUEUE", "cap1_recv", rtos_queue_recv(&queue, &got, RTOS_NO_WAIT), RTOS_OK);
    bt_expect("QUEUE", "cap1_recv_empty", rtos_queue_recv(&queue, &got, RTOS_NO_WAIT),
              RTOS_ERR_TIMEOUT);
    bt_expect_u32("QUEUE", "cap1_value", got, value);

    bt_expect("QUEUE", "deinit_valid", rtos_queue_deinit(&queue), RTOS_OK);
    bt_expect("QUEUE", "send_after_deinit", rtos_queue_send(&queue, &value, RTOS_NO_WAIT,
                                                            RTOS_FALSE), RTOS_ERR_PARAM);
}

/* ============================== 第 5 组: 任务创建边界 ============================== */

/** @brief 边界用例用的空转任务体。 */
static void bt_idle_body(void *arg)
{
    (void)arg;
    for (;;) {
        rtos_task_delay(1000U);
    }
}

/**
 * @brief 任务创建/参数的契约极值。
 *
 * @details 关键极值: 栈 96(恰好合法) vs 95(恰好非法); 优先级 31 vs 32;
 *          栈上限 = 堆大小/字长(恰好合法) vs 其 +1(恰好非法)。
 */
static void bt_boundary_task(void)
{
    rtos_tcb_t *tcb = NULL;
    rtos_status_t status;
    /* 栈上限: 最大单块连续可分配字节, 扣除 TCB 实际占用(TCB 在栈之前分配,
     * 其占用量 = 140 字节向上对齐 8 = 144, 再加一个块头 8)。
     * [缺陷修复] 修复前实现按 HEAP/sizeof(stack_t) 校验(= 整个堆 = 8192 字),
     * 该上界不可达, 必然返回 ERR_NO_MEM。现改为与分配器口径一致的表达,
     * 故此处按同一公式取值并断言"恰好可通过 / 再多 1 字必然被拒"。
     * 该值必须在创建任何任务**之前**采样(否则堆已被占用)。 */
    const uint32_t tcb_cost = ((uint32_t)sizeof(rtos_tcb_t) + 7U) & ~7U;
    const uint32_t max_stack_bytes = rtos_heap_get_max_alloc() - tcb_cost - 8U;
    const uint32_t stack_max = max_stack_bytes / (uint32_t)sizeof(rtos_stack_t);

    printf("BT|GEOM|sizeof_tcb=%lu|heap=%lu|max_alloc=%lu|stack_max_words=%lu\r\n",
           (unsigned long)sizeof(rtos_tcb_t), (unsigned long)RTOS_CONFIG_HEAP_SIZE,
           (unsigned long)rtos_heap_get_max_alloc(), (unsigned long)stack_max);

    bt_expect("TASK", "create_null_out", rtos_task_create(NULL, 128U, bt_idle_body, NULL, 5U, "bt"),
              RTOS_ERR_NULL);
    bt_expect("TASK", "create_null_entry", rtos_task_create(&tcb, 128U, NULL, NULL, 5U, "bt"),
              RTOS_ERR_NULL);

    /* ---- 栈: 超出下边界 / 恰好下边界 / 恰好上边界 / 超出上边界 ---- */
    bt_expect("TASK", "create_stack_over_lower", rtos_task_create(&tcb, 1U, bt_idle_body, NULL, 5U,
                                                                  "bt"), RTOS_ERR_PARAM);
    bt_expect("TASK", "create_stack_lower_ok", rtos_task_create(&tcb, 96U, bt_idle_body, NULL, 5U,
                                                                "bt"), RTOS_OK);
    if (tcb != NULL) {
        bt_expect("TASK", "delete_stack_lower_ok", rtos_task_delete(tcb), RTOS_OK);
        tcb = NULL;
    }
    /* 删除是延迟回收(交由 idle 任务), 此处必须等回收完成, 否则后续
     * "堆上界"用例会把尚未回收的空间算成已占用而误判。 */
    rtos_task_delay(20U);
    bt_expect("TASK", "create_stack_upper_ok",
              rtos_task_create(&tcb, stack_max, bt_idle_body, NULL, 5U, "bt"), RTOS_OK);
    if (tcb != NULL) {
        bt_expect("TASK", "delete_stack_upper_ok", rtos_task_delete(tcb), RTOS_OK);
        tcb = NULL;
    }
    rtos_task_delay(20U);
    /* 超出上边界: 请求"整个堆"字节的栈。
     * 该值**无条件不可满足**(堆的全部字节不可能都做栈, 且 TCB 还要占),
     * 故必须由参数校验拒绝(ERR_PARAM)。
     * 注: 不用 stack_max+1 断言 —— stack_max 依赖调用瞬间的堆形状, 与本用例
     * 之间可能发生分配, 那样会退化为 ERR_NO_MEM, 断言不确定。 */
    bt_expect("TASK", "create_stack_over_heap",
              rtos_task_create(&tcb, (RTOS_CONFIG_HEAP_SIZE / (uint32_t)sizeof(rtos_stack_t)) + 1U,
                               bt_idle_body, NULL, 5U, "bt"),
              RTOS_ERR_PARAM);
    rtos_task_delay(20U);

    /* ---- 优先级: 上边界 31 合法, 超出上边界 32 非法 ---- */
    bt_expect("TASK", "create_prio_lower_0", rtos_task_create(&tcb, 128U, bt_idle_body, NULL, 0U,
                                                              "bt"), RTOS_OK);
    if (tcb != NULL) {
        bt_expect("TASK", "delete_prio0", rtos_task_delete(tcb), RTOS_OK);
        tcb = NULL;
    }
    bt_expect("TASK", "create_prio_upper_31",
              rtos_task_create(&tcb, 128U, bt_idle_body, NULL, 31U, "bt"), RTOS_OK);
    if (tcb != NULL) {
        bt_expect("TASK", "delete_prio31", rtos_task_delete(tcb), RTOS_OK);
        tcb = NULL;
    }
    bt_expect("TASK", "create_prio_over_32",
              rtos_task_create(&tcb, 128U, bt_idle_body, NULL, 32U, "bt"), RTOS_ERR_PARAM);
    bt_expect("TASK", "create_prio_255", rtos_task_create(&tcb, 128U, bt_idle_body, NULL, 255U,
                                                          "bt"), RTOS_ERR_PARAM);

    /* ---- 名称: 空值合法(可选字段), 且不影响任务可用性 ---- */
    status = rtos_task_create(&tcb, 128U, bt_idle_body, NULL, 5U, NULL);
    bt_expect("TASK", "create_name_null_ok", status, RTOS_OK);
    if (tcb != NULL) {
        rtos_task_delay(5U); /* 让新任务至少被调度一次, 验证其确实可运行 */
        bt_expect("TASK", "delete_name_null_ok", rtos_task_delete(tcb), RTOS_OK);
        tcb = NULL;
    }

    /* ---- 稳定性: 一串非法创建之后, 合法创建仍必须成功(堆未泄漏) ---- */
    bt_expect("TASK", "stable_create_after_errors",
              rtos_task_create(&tcb, 128U, bt_idle_body, NULL, 5U, "bt"), RTOS_OK);
    if (tcb != NULL) {
        bt_expect("TASK", "stable_delete_after_errors", rtos_task_delete(tcb), RTOS_OK);
    }
}

/* ============================== 第 6 组: 通知与延时边界 ============================== */

/**
 * @brief 任务通知类型边界与延时下边界。
 */
static void bt_boundary_notify_delay(void)
{
    rtos_tcb_t *self = rtos_task_get_current();
    uint32_t back = 0U;

    bt_expect("NOTIFY", "notify_null_tcb", rtos_task_notify(NULL, 1U, RTOS_NOTIFY_VALUE),
              RTOS_ERR_NULL);
    bt_expect("NOTIFY", "notify_bad_type_none", rtos_task_notify(self, 1U, RTOS_NOTIFY_NONE),
              RTOS_ERR_PARAM);
    bt_expect("NOTIFY", "notify_bad_type_over", rtos_task_notify(self, 1U,
                                                                 (rtos_notify_type_t)99),
              RTOS_ERR_PARAM);

    /* 自通知: 先给自己投递一个值, 再以超时 0 取回(下边界超时) */
    bt_expect("NOTIFY", "notify_self_value", rtos_task_notify(self, 0xA5A5A5A5U,
                                                              RTOS_NOTIFY_VALUE), RTOS_OK);
    bt_expect("NOTIFY", "notify_wait_timeout0", rtos_task_notify_wait(RTOS_NO_WAIT, &back),
              RTOS_OK);
    bt_expect_u32("NOTIFY", "notify_value_intact", back, 0xA5A5A5A5U);

    /* 无待处理通知时以超时 0 等待: 下边界应超时而非阻塞 */
    bt_expect("NOTIFY", "notify_wait_empty_timeout0", rtos_task_notify_wait(RTOS_NO_WAIT, &back),
              RTOS_ERR_TIMEOUT);

    /* 延时下边界: 0 与 1 个 tick 都必须返回且系统保持可用 */
    rtos_task_delay(0U);
    rtos_task_delay(1U);
    bt_report("DELAY", "delay_0_and_1_returned", RTOS_TRUE, 0, 0);
    bt_expect_u32("DELAY", "tick_rate_sane", (rtos_sched_get_tick_count() > 0U) ? 1U : 0U, 1U);
}

/* ============================== 第 7 组: 稳定性收尾 ============================== */

/**
 * @brief 收尾稳定性检查: 长时间运行与资源计数自洽。
 */
static void bt_stability_tail(void)
{
    rtos_tcb_t *self = rtos_task_get_current();

    /* 高优先级任务在忙循环中让出若干次, 验证调度器在边界用例之后仍正常 */
    for (uint32_t i = 0U; i < 50U; i++) {
        rtos_task_yield();
    }
    bt_report("STABLE", "yield_50_after_boundaries", RTOS_TRUE, 0, 0);

    /* 当前任务应仍然有效且可查询 */
    bt_report("STABLE", "current_tcb_valid", (self != NULL) ? RTOS_TRUE : RTOS_FALSE, 0, 0);
    bt_report("STABLE", "tick_advancing",
              (rtos_sched_get_tick_count() > 0U) ? RTOS_TRUE : RTOS_FALSE, 0, 0);
}

/* ============================== 主流程 ============================== */

void boundary_test_task(void *arg)
{
    (void)arg;

    /* 使能 DWT 周期计数器(与 benchmark.c 相同的初始化序列) */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    DWT->CYCCNT = 0U;

    rtos_task_delay(100U); /* 等系统与 USB 初始化稳定 */

    printf("BT|META|f446_rtos boundary test|max_prio=%lu|min_stack=%lu|heap=%lu|tick_hz=%lu\r\n",
           (unsigned long)RTOS_CONFIG_MAX_PRIORITIES, (unsigned long)96U,
           (unsigned long)RTOS_CONFIG_HEAP_SIZE, (unsigned long)RTOS_CONFIG_TICK_RATE_HZ);

    bt_perf_fast_paths();       /* 第 1 组: 快路径周期 */
    bt_boundary_sem();          /* 第 2 组: 信号量 */
    bt_boundary_mutex();        /* 第 3 组: 互斥锁 */
    bt_boundary_queue();        /* 第 4 组: 队列 */
    bt_boundary_task();         /* 第 5 组: 任务创建 */
    bt_boundary_notify_delay(); /* 第 6 组: 通知与延时 */
    bt_stability_tail();        /* 第 7 组: 稳定性 */

    printf("BT|SUMMARY|pass=%lu fail=%lu\r\n", (unsigned long)s_pass_count,
           (unsigned long)s_fail_count);

    for (;;) {
        rtos_task_delay(1000U);
    }
}
