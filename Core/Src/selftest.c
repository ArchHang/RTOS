/**
 * @file    selftest.c
 * @brief   RTOS 真机自测试固件(HIL) —— 覆盖 sched/task/queue/sem/mutex/event/
 *          timer/mempool/heap/perf 全模块, 含边界与非法用法用例。
 *
 * @details 测试结果经 USART2(115200) 输出, 每行:
 *            ST|<模块>|<用例>|<PASS|FAIL|INFO>|<详情>
 *
 *          设计要点:
 *            - 纯黑盒: 只调用 RTOS 公开 API, 不修改内核代码。
 *            - 危险项分级: 会 assert 死循环/HardFault 的用法(调度器启动前
 *              调阻塞 API 等)仅打印 SKIP 说明, 不实际触发。
 *            - ISR 上下文: 借用 USART2 RX 中断真实执行 API 矩阵,
 *              由上位机回发 'X' 触发。
 *            - 栈溢出: 检测机制校验 TCB.stack_magic 字段, 直接破坏该字段
 *              即可在零内存污染下触发 rtos_stack_overflow_hook。
 */
#include "selftest.h"
#include "rtos.h"
#include <stdio.h>
#include <string.h>

/* ==================================================================== */
/*                              统计与宏                                */
/* ==================================================================== */

static volatile uint32_t st_pass, st_fail, st_info, st_total;

#define ST_CHECK(mod, name, cond, fmt, ...)                                                        \
    do {                                                                                            \
        st_total++;                                                                                 \
        if (cond) {                                                                                 \
            st_pass++;                                                                              \
            printf("ST|%s|%s|PASS|" fmt "\r\n", mod, name, ##__VA_ARGS__);                          \
        } else {                                                                                    \
            st_fail++;                                                                              \
            printf("ST|%s|%s|FAIL|" fmt "\r\n", mod, name, ##__VA_ARGS__);                          \
        }                                                                                           \
    } while (0)

#define ST_INFO(mod, name, fmt, ...)                                                                \
    do {                                                                                            \
        st_total++;                                                                                 \
        st_info++;                                                                                  \
        printf("ST|%s|%s|INFO|" fmt "\r\n", mod, name, ##__VA_ARGS__);                              \
    } while (0)

/** 断言 rtos_status_t 返回值等于期望值。 */
static void st_expect(const char *mod, const char *name, rtos_status_t got, rtos_status_t want)
{
    ST_CHECK(mod, name, got == want, "ret=%d want=%d", (int)got, (int)want);
}

/* ==================================================================== */
/*                       内核钩子重定义(安全化)                          */
/* ==================================================================== */

/* 断言失败: 记录后停机(默认实现也是死循环, 这里至少留下现场定位信息)。 */
void rtos_assert_fail(const char *cond, const char *file, int line)
{
    printf("ST|ASSERT|TRAP|cond=%s file=%s line=%d\r\n", cond ? cond : "?",
           file ? file : "?", line);
    for (;;) { }
}

/* 栈溢出: 记录后返回(不死循环), 使测试流程可以继续。 */
static volatile uint8_t     st_ovf_flag;
static volatile rtos_tcb_t *st_ovf_tcb;
void rtos_stack_overflow_hook(rtos_tcb_t *tcb)
{
    st_ovf_flag = 1;
    st_ovf_tcb  = tcb;
}

/* ==================================================================== */
/*                          ISR 矩阵测试(USART2)                         */
/* ==================================================================== */

/* 矩阵结果索引 */
enum {
    ISR_Q_RECV_NOWAIT = 0, /* 队列有数据, NO_WAIT 接收 → 期望 OK       */
    ISR_Q_RECV_TMO,        /* ISR 中带超时接收       → 期望 ERR_ISR   */
    ISR_Q_SEND_NOWAIT,    /* 队列有空位, NO_WAIT 发送 → 期望 OK       */
    ISR_Q_SEND_TMO,        /* ISR 中带超时发送       → 期望 ERR_ISR   */
    ISR_S_TAKE_NOWAIT,    /* 空信号量 NO_WAIT 获取  → 期望 ERR_TIMEOUT*/
    ISR_S_TAKE_TMO,        /* ISR 中带超时获取       → 期望 ERR_ISR   */
    ISR_S_GIVE,            /* ISR 中释放             → 期望 OK        */
    ISR_M_TAKE,            /* ISR 中取互斥锁         → 期望 ERR_ISR   */
    ISR_M_GIVE,            /* ISR 中放互斥锁         → 期望 ERR_ISR   */
    ISR_TASK_DEL,          /* ISR 中删任务           → 期望 ERR_ISR   */
    ISR_NOTIFY_WAIT,      /* ISR 中等通知           → 期望 ERR_ISR   */
    ISR_T_START,           /* ISR 中启动定时器       → 期望 OK(入队)  */
    ISR_NOTIFY,            /* ISR 中通知任务         → 期望 OK        */
    ISR_SUSPEND,           /* ISR 中挂起他人         → 期望 OK        */
    ISR_RESUME,            /* ISR 中恢复他人         → 期望 OK        */
    ISR_N
};

static volatile rtos_status_t st_isr_r[ISR_N];
static volatile uint32_t     st_isr_ev_wait_ret;
static volatile uint32_t     st_isr_ev_set_ret;
static volatile uint32_t     st_isr_perf_cpu;
static volatile uint8_t      st_isr_armed, st_isr_done, st_isr_delay_ok;
static volatile uint8_t      st_isr_recv_val;
static uint8_t               st_isr_item[2] = {0xEE, 0xEE}; /* item_size=2 */

static rtos_queue_t st_isr_q;
static uint8_t      st_isr_q_storage[2 * 2]; /* item_size=2, capacity=2 */
static rtos_sem_t   st_isr_sem;
static rtos_mutex_t st_isr_mutex;
static rtos_event_t st_isr_ev;
static rtos_timer_t st_isr_timer;
static rtos_tcb_t  *st_isr_victim_tcb;
static volatile uint32_t st_isr_victim_notified;
static volatile uint8_t  st_isr_timer_fired;

/* ISR 矩阵常驻 victim: 低优先级, 永远等通知 */
static void st_isr_victim_fn(void *arg)
{
    uint32_t v;
    (void)arg;
    for (;;) {
        if (rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v) == RTOS_OK) {
            st_isr_victim_notified = v;
        }
    }
}

static void st_isr_timer_cb(void *arg)
{
    (void)arg;
    st_isr_timer_fired = 1;
}

/* 由 HAL_UART_RxCpltCallback 调用(真实 USART2 中断上下文) */
uint8_t selftest_uart_isr_hook(uint8_t byte)
{
    uint8_t buf[2];

    if ((st_isr_armed == 0U) || (byte != (uint8_t)'X')) {
        return 0U;
    }
    st_isr_armed = 0U;

    st_isr_r[ISR_Q_RECV_NOWAIT] = rtos_queue_recv(&st_isr_q, buf, RTOS_NO_WAIT);
    st_isr_recv_val             = buf[0];
    st_isr_r[ISR_Q_RECV_TMO]    = rtos_queue_recv(&st_isr_q, buf, 100);
    st_isr_r[ISR_Q_SEND_NOWAIT] = rtos_queue_send(&st_isr_q, st_isr_item, RTOS_NO_WAIT, RTOS_FALSE);
    st_isr_r[ISR_Q_SEND_TMO]    = rtos_queue_send(&st_isr_q, st_isr_item, 100, RTOS_FALSE);
    st_isr_r[ISR_S_TAKE_NOWAIT] = rtos_sem_take(&st_isr_sem, RTOS_NO_WAIT);
    st_isr_r[ISR_S_TAKE_TMO]    = rtos_sem_take(&st_isr_sem, 100);
    st_isr_r[ISR_S_GIVE]        = rtos_sem_give(&st_isr_sem);
    st_isr_r[ISR_M_TAKE]        = rtos_mutex_take(&st_isr_mutex, RTOS_NO_WAIT);
    st_isr_r[ISR_M_GIVE]        = rtos_mutex_give(&st_isr_mutex);
    st_isr_r[ISR_TASK_DEL]      = rtos_task_delete(st_isr_victim_tcb);
    {
        uint32_t v = 0;
        st_isr_r[ISR_NOTIFY_WAIT] = rtos_task_notify_wait(0, &v);
    }
    st_isr_ev_wait_ret = rtos_event_wait(&st_isr_ev, 0x01U, RTOS_EVENT_WAIT_ANY, 100);
    st_isr_ev_set_ret = rtos_event_set(&st_isr_ev, 0x02U);
    st_isr_r[ISR_T_START] = rtos_timer_start(&st_isr_timer);
    st_isr_r[ISR_NOTIFY]  = rtos_task_notify(st_isr_victim_tcb, 0xABU, RTOS_NOTIFY_VALUE);
    rtos_task_delay(10); /* ISR 中应静默空操作, 能返回说明未卡死 */
    st_isr_delay_ok        = 1U;
    st_isr_r[ISR_SUSPEND]  = rtos_task_suspend(st_isr_victim_tcb);
    st_isr_r[ISR_RESUME]   = rtos_task_resume(st_isr_victim_tcb);
    st_isr_perf_cpu        = rtos_perf_get_cpu_usage();

    st_isr_done = 1U;
    return 1U;
}

/* ==================================================================== */
/*                        阶段1: 调度器启动前测试                        */
/* ==================================================================== */

static volatile uint32_t st_p31_flag;   /* prio=31 小栈任务跑过 */
static volatile uint8_t  st_pre_timer_fired;
static rtos_tcb_t       *st_p31_tcb;
static rtos_timer_t     st_pre_timer;

static void st_p31_fn(void *arg)
{
    *(volatile uint32_t *)arg = 1U;
    /* 注意: 不允许此任务调用 rtos_task_delete(NULL) 自删 —— 20 字(80B)最小栈
     * 走 delete 调用链会栈溢出, 溢出写会破坏其在堆内的块 header(实测)。
     * 改为长延时挂起, 由主测试任务从外部删除。 */
    for (;;) { rtos_task_delay(1000); }
}

static void st_pre_timer_cb(void *arg)
{
    (void)arg;
    st_pre_timer_fired = 1U;
}

void selftest_pre_start(void)
{
    rtos_status_t r;
    rtos_tcb_t   *t = NULL;
    rtos_sem_t    s;
    rtos_queue_t  q;
    uint8_t       qbuf[8];
    uint32_t      v32;
    rtos_event_t  e;
    rtos_timer_t  bad_t;

    printf("ST|META|phase1|INFO|pre-start context (before rtos_start)\r\n");

    /* ---- 调度器状态 ---- */
    ST_CHECK("SCHED", "pre_state_not_started", rtos_sched_get_state() == RTOS_SCHED_NOT_STARTED,
             "state=%d", (int)rtos_sched_get_state());
    ST_CHECK("SCHED", "pre_tick_is_zero", rtos_sched_get_tick_count() == 0U,
             "tick=%u", (unsigned)rtos_sched_get_tick_count());

    /* ---- task: 启动前查询 ---- */
    ST_CHECK("TASK", "pre_get_current_null", rtos_task_get_current() == NULL, "");
    ST_CHECK("TASK", "pre_get_priority_none", rtos_task_get_priority(NULL) == RTOS_CONFIG_MAX_PRIORITIES,
             "prio=%u", (unsigned)rtos_task_get_priority(NULL));
    st_expect("TASK", "pre_set_priority_no_cur", rtos_task_set_priority(NULL, 5U), RTOS_ERR_PARAM);

    /* ---- task: 创建参数检查(边界外) ---- */
    st_expect("TASK", "create_null_out", rtos_task_create(NULL, 256, st_p31_fn, NULL, 5, "x"), RTOS_ERR_NULL);
    st_expect("TASK", "create_null_entry", rtos_task_create(&t, 256, NULL, NULL, 5, "x"), RTOS_ERR_NULL);
    st_expect("TASK", "create_prio_32", rtos_task_create(&t, 256, st_p31_fn, NULL, 32, "x"), RTOS_ERR_PARAM);
    st_expect("TASK", "create_prio_9999", rtos_task_create(&t, 256, st_p31_fn, NULL, 9999, "x"), RTOS_ERR_PARAM);
    st_expect("TASK", "create_stack_39", rtos_task_create(&t, 39, st_p31_fn, NULL, 5, "x"), RTOS_ERR_PARAM);
    /* [fix BUG-4] stack_size*4 乘法回绕: 0x40000001 通过 >=40 检查但乘 4 回绕成 4 */
    st_expect("TASK", "create_stack_wrap", rtos_task_create(&t, 0x40000001U, st_p31_fn, NULL, 5, "x"), RTOS_ERR_PARAM);
    st_expect("TASK", "static_null_tcb", rtos_task_create_static(NULL, NULL, 64, st_p31_fn, NULL, 5, "x"), RTOS_ERR_NULL);
    st_expect("TASK", "static_stack_39", rtos_task_create_static(&t, NULL, 39, st_p31_fn, NULL, 5, "x"), RTOS_ERR_NULL);

    /* ---- task: 极限参数创建成功(启动后验证运行) ----
     * [fix BUG-4] 最小栈 20→96 字: init 18 + FPU 阻塞压栈 52 + 调用链裕量
     * (实测 20 字 FPU 任务首次切换即越界写穿堆元数据)。 */
    st_expect("TASK", "create_prio31_stack128", rtos_task_create(&st_p31_tcb, 128, st_p31_fn,
             (void *)&st_p31_flag, 31, "p31"), RTOS_OK);

    /* ---- 通知: 启动前发给未运行任务(安全) ---- */
    st_expect("NOTIFY", "pre_notify_bad_type", rtos_task_notify(st_p31_tcb, 1U, RTOS_NOTIFY_NONE), RTOS_ERR_PARAM);
    st_expect("NOTIFY", "pre_notify_bad_type2", rtos_task_notify(st_p31_tcb, 1U, (rtos_notify_type_t)9), RTOS_ERR_PARAM);
    st_expect("NOTIFY", "pre_notify_null", rtos_task_notify(NULL, 1U, RTOS_NOTIFY_VALUE), RTOS_ERR_NULL);

    /* ---- sem: 非阻塞路径 + 参数检查 ---- */
    st_expect("SEM", "init_null", rtos_sem_init(NULL, 0, 1), RTOS_ERR_PARAM);
    st_expect("SEM", "init_max0", rtos_sem_init(&s, 0, 0), RTOS_ERR_PARAM);
    st_expect("SEM", "init_gt_max", rtos_sem_init(&s, 3, 2), RTOS_ERR_PARAM);
    st_expect("SEM", "init_ok", rtos_sem_init(&s, 1, 2), RTOS_OK);
    st_expect("SEM", "pre_take_ok", rtos_sem_take(&s, RTOS_NO_WAIT), RTOS_OK);
    st_expect("SEM", "pre_take_empty_nowait", rtos_sem_take(&s, RTOS_NO_WAIT), RTOS_ERR_TIMEOUT);
    st_expect("SEM", "pre_give_ok", rtos_sem_give(&s), RTOS_OK);
    ST_CHECK("SEM", "pre_count_1", rtos_sem_get_count(&s) == 1U, "cnt=%u", (unsigned)rtos_sem_get_count(&s));
    {
        rtos_sem_t dead; /* 全零 = 未初始化对象 */
        memset(&dead, 0, sizeof(dead));
        st_expect("SEM", "uninit_take", rtos_sem_take(&dead, RTOS_NO_WAIT), RTOS_ERR_PARAM);
        st_expect("SEM", "uninit_give", rtos_sem_give(&dead), RTOS_ERR_PARAM);
        st_expect("SEM", "null_take", rtos_sem_take(NULL, RTOS_NO_WAIT), RTOS_ERR_NULL);
    }

    /* ---- queue: 非满/非空路径 + 参数检查 ---- */
    st_expect("QUEUE", "init_null_q", rtos_queue_init(NULL, qbuf, 4, 2), RTOS_ERR_PARAM);
    st_expect("QUEUE", "init_null_storage", rtos_queue_init(&q, NULL, 4, 2), RTOS_ERR_PARAM);
    st_expect("QUEUE", "init_item0", rtos_queue_init(&q, qbuf, 0, 2), RTOS_ERR_PARAM);
    st_expect("QUEUE", "init_cap0", rtos_queue_init(&q, qbuf, 4, 0), RTOS_ERR_PARAM);
    st_expect("QUEUE", "init_ok", rtos_queue_init(&q, qbuf, 4, 2), RTOS_OK);
    v32 = 0x11223344;
    st_expect("QUEUE", "pre_send_ok", rtos_queue_send(&q, &v32, RTOS_NO_WAIT, RTOS_FALSE), RTOS_OK);
    v32 = 0;
    st_expect("QUEUE", "pre_recv_ok", rtos_queue_recv(&q, &v32, RTOS_NO_WAIT), RTOS_OK);
    ST_CHECK("QUEUE", "pre_data_match", v32 == 0x11223344U, "v=%x", (unsigned)v32);
    st_expect("QUEUE", "null_send", rtos_queue_send(NULL, &v32, RTOS_NO_WAIT, RTOS_FALSE), RTOS_ERR_NULL);
    st_expect("QUEUE", "null_recv", rtos_queue_recv(NULL, &v32, RTOS_NO_WAIT), RTOS_ERR_NULL);
    {
        rtos_queue_t dead;
        memset(&dead, 0, sizeof(dead));
        st_expect("QUEUE", "uninit_send", rtos_queue_send(&dead, &v32, RTOS_NO_WAIT, RTOS_FALSE), RTOS_ERR_PARAM);
    }

    /* ---- mutex: 启动前退化语义(owner 恒 NULL, take/give 均返回 OK) ---- */
    {
        rtos_mutex_t m;
        st_expect("MUTEX", "init_null", rtos_mutex_init(NULL), RTOS_ERR_NULL);
        st_expect("MUTEX", "init_ok", rtos_mutex_init(&m), RTOS_OK);
        st_expect("MUTEX", "pre_take_degenerate", rtos_mutex_take(&m, RTOS_NO_WAIT), RTOS_OK);
        st_expect("MUTEX", "pre_give_degenerate", rtos_mutex_give(&m), RTOS_OK);
    }

    /* ---- event: set/get 非阻塞路径 ---- */
    {
        rtos_event_bits_t b;
        st_expect("EVENT", "init_null", rtos_event_init(NULL), RTOS_ERR_NULL);
        st_expect("EVENT", "init_ok", rtos_event_init(&e), RTOS_OK);
        b = rtos_event_set(&e, 0x05U);
        ST_CHECK("EVENT", "pre_set_ret", b == 0x05U, "ret=%x", (unsigned)b);
        b = rtos_event_set(&e, 0x02U);
        ST_CHECK("EVENT", "pre_set_sticky", b == 0x07U, "ret=%x", (unsigned)b);
        b = rtos_event_clear(&e, 0x04U);
        ST_CHECK("EVENT", "pre_clear_ret", b == 0x03U, "ret=%x", (unsigned)b);
        b = rtos_event_wait(&e, 0x03U, RTOS_EVENT_WAIT_ALL, RTOS_NO_WAIT);
        ST_CHECK("EVENT", "pre_wait_all_ready", (b & 0x03U) == 0x03U, "ret=%x", (unsigned)b);
        st_expect("EVENT", "deinit_ok", rtos_event_deinit(&e), RTOS_OK);
    }

    /* ---- timer: create 参数检查 + 1 次安全 start ---- */
    st_expect("TIMER", "create_null_timer", rtos_timer_create(NULL, "x", st_pre_timer_cb, NULL, 100, RTOS_TIMER_ONE_SHOT), RTOS_ERR_PARAM);
    st_expect("TIMER", "create_null_cb", rtos_timer_create(&bad_t, "x", NULL, NULL, 100, RTOS_TIMER_ONE_SHOT), RTOS_ERR_PARAM);
    st_expect("TIMER", "create_period0", rtos_timer_create(&bad_t, "x", st_pre_timer_cb, NULL, 0, RTOS_TIMER_ONE_SHOT), RTOS_ERR_PARAM);
    st_expect("TIMER", "create_ok", rtos_timer_create(&st_pre_timer, "pre", st_pre_timer_cb, NULL, 100, RTOS_TIMER_ONE_SHOT), RTOS_OK);
    st_expect("TIMER", "pre_start_enqueue", rtos_timer_start(&st_pre_timer), RTOS_OK);

    /* ---- mempool / heap: 启动前完全可用 ---- */
    {
        static rtos_mempool_t pool;
        static uint8_t       base[4 * 4];
        void *p;
        st_expect("MEM", "pre_init_null_pool", rtos_mempool_init(NULL, base, 4, 4), RTOS_ERR_PARAM);
        st_expect("MEM", "pre_init_null_base", rtos_mempool_init(&pool, NULL, 4, 4), RTOS_ERR_PARAM);
        st_expect("MEM", "pre_init_sz0", rtos_mempool_init(&pool, base, 0, 4), RTOS_ERR_PARAM);
        st_expect("MEM", "pre_init_cnt0", rtos_mempool_init(&pool, base, 4, 0), RTOS_ERR_PARAM);
        st_expect("MEM", "pre_init_ok", rtos_mempool_init(&pool, base, 4, 4), RTOS_OK);
        p = rtos_mempool_alloc(&pool);
        ST_CHECK("MEM", "pre_alloc_ok", p != NULL, "");
        st_expect("MEM", "pre_free_ok", rtos_mempool_free(&pool, p), RTOS_OK);
    }
    {
        void *p = rtos_heap_alloc(16);
        ST_CHECK("HEAP", "pre_alloc_ok", p != NULL, "");
        if (p != NULL) {
            ST_CHECK("HEAP", "pre_align8", ((uint32_t)p % 8U) == 0U, "p=0x%x", (unsigned)(uint32_t)p);
            rtos_heap_free(p);
        }
        p = rtos_heap_alloc(0);
        ST_CHECK("HEAP", "pre_alloc0_null", p == NULL, "");
    }

    /* ---- perf: 启动前查询 ---- */
    {
        rtos_perf_task_t ts;
        st_expect("PERF", "pre_get_task_no_cur", rtos_perf_get_task(NULL, &ts), RTOS_ERR_PARAM);
    }

    /* ---- 危险项: 只记录不执行 ---- */
    ST_INFO("SCHED", "pre_delay_skip", "rtos_task_delay(>0) before start would assert(trap)");
    ST_INFO("QUEUE", "pre_block_skip", "blocking send/recv before start would assert(trap)");
    ST_INFO("SEM", "pre_block_take_skip", "sem_take(timeout>0) on empty sem before start would assert(trap)");
    ST_INFO("EVENT", "pre_wait_skip", "event_wait(not-ready,timeout>0) before start would HardFault");
    ST_INFO("NOTIFY", "pre_wait_skip", "notify_wait(timeout>0) before start would HardFault");

    r = rtos_get_version() != NULL ? RTOS_OK : RTOS_ERR_NULL;
    ST_CHECK("META", "version_str", r == RTOS_OK && strcmp(rtos_get_version(), "1.0.0") == 0,
             "ver=%s", rtos_get_version());
    (void)r;
}

/* ==================================================================== */
/*                     阶段2: 任务上下文全量测试                          */
/* ==================================================================== */

/* ---------------- 通用辅助任务 ---------------- */

/* 记录型 victim: 阻塞在 sem 上, 醒来记录返回值 */
typedef struct {
    volatile rtos_status_t result;
    volatile uint32_t     data;
    volatile uint32_t     count_after_wake;
    volatile uint8_t      done;
    rtos_sem_t           *sem;
    rtos_queue_t         *q;
    rtos_mutex_t         *mtx;
    rtos_event_t         *ev;
    rtos_event_bits_t     wait_bits;
    uint32_t              mode;
    rtos_tick_t           timeout;
    volatile uint8_t      stage;
} victim_ctx_t;

static victim_ctx_t vc;

static void st_victim_sem_fn(void *arg)
{
    victim_ctx_t *c = (victim_ctx_t *)arg;
    c->result = rtos_sem_take(c->sem, c->timeout);
    c->count_after_wake = rtos_sem_get_count(c->sem);
    c->done = 1;
    for (;;) { rtos_task_delay(100); }
}

static void st_victim_queue_fn(void *arg)
{
    victim_ctx_t *c = (victim_ctx_t *)arg;
    c->result = rtos_queue_recv(c->q, (void *)&c->data, c->timeout);
    c->done = 1;
    for (;;) { rtos_task_delay(100); }
}

static void st_victim_queue_send_fn(void *arg)
{
    victim_ctx_t *c = (victim_ctx_t *)arg;
    c->result = rtos_queue_send(c->q, (const void *)&c->data, c->timeout, RTOS_FALSE);
    c->done = 1;
    for (;;) { rtos_task_delay(100); }
}

static void st_victim_mutex_fn(void *arg)
{
    victim_ctx_t *c = (victim_ctx_t *)arg;
    c->result = rtos_mutex_take(c->mtx, c->timeout);
    c->done = 1;
    for (;;) { rtos_task_delay(100); }
}

static void st_victim_event_fn(void *arg)
{
    victim_ctx_t *c = (victim_ctx_t *)arg;
    c->data = (uint32_t)rtos_event_wait(c->ev, c->wait_bits, c->mode, c->timeout);
    c->done = 1;
    for (;;) { rtos_task_delay(100); }
}

/* 前向声明(定义在文件末尾) */
void st_returning_fn(void *arg);
void st_suicide_fn(void *arg);
void st_boost_fn(void *arg);

/* ---------------- SCHED 模块 ---------------- */

static void test_sched(void)
{
    rtos_tick_t t0, dt, last;
    uint32_t i;

    ST_CHECK("SCHED", "state_running", rtos_sched_get_state() == RTOS_SCHED_RUNNING,
             "state=%d", (int)rtos_sched_get_state());
    t0 = rtos_sched_get_tick_count();
    ST_CHECK("SCHED", "tick_nonzero_after_boot", t0 > 0U && t0 < 10000U, "tick=%u", (unsigned)t0);

    /* delay 精度: 100ms, 容差 +6 */
    t0 = rtos_sched_get_tick_count();
    rtos_task_delay(100);
    dt = rtos_sched_get_tick_count() - t0;
    ST_CHECK("SCHED", "delay100", dt >= 100U && dt <= 106U, "dt=%u", (unsigned)dt);

    /* delay(0) 等价 yield, 不死锁 */
    t0 = rtos_sched_get_tick_count();
    rtos_task_delay(0);
    dt = rtos_sched_get_tick_count() - t0;
    ST_CHECK("SCHED", "delay0_yield", dt <= 2U, "dt=%u", (unsigned)dt);

    /* delay_until 周期精度: 3 x 100ms */
    last = rtos_sched_get_tick_count();
    t0 = last;
    for (i = 0; i < 3U; i++) {
        rtos_task_delay_until(&last, 100);
    }
    dt = rtos_sched_get_tick_count() - t0;
    ST_CHECK("SCHED", "delay_until_3x100", dt >= 298U && dt <= 306U, "dt=%u", (unsigned)dt);

    /* delay_until 非法参数静默返回 */
    rtos_task_delay_until(NULL, 100);
    ST_CHECK("SCHED", "delay_until_null_ok", 1, "");

    /* yield 正常返回 */
    rtos_task_yield();
    ST_CHECK("SCHED", "yield_ok", 1, "");

    /* 大 delay 单调性 */
    t0 = rtos_sched_get_tick_count();
    rtos_task_delay(200);
    dt = rtos_sched_get_tick_count() - t0;
    ST_CHECK("SCHED", "delay200", dt >= 200U && dt <= 206U, "dt=%u", (unsigned)dt);
}

/* ---------------- TASK 模块 ---------------- */

static void test_task(void)
{
    rtos_tcb_t *t;
    rtos_status_t r;
    uint32_t v;

    /* 启动前 start 的预置定时器已触发(one-shot 100ms), 等待触发 */
    rtos_task_delay(200);
    ST_CHECK("TIMER", "prestart_timer_fired", st_pre_timer_fired == 1U, "fired=%u", (unsigned)st_pre_timer_fired);

    /* get_current / get_priority */
    t = rtos_task_get_current();
    ST_CHECK("TASK", "get_current_ok", t != NULL, "");
    ST_CHECK("TASK", "get_priority_self", rtos_task_get_priority(NULL) == 9U,
             "prio=%u", (unsigned)rtos_task_get_priority(NULL));
    ST_CHECK("TASK", "get_priority_tcb", rtos_task_get_priority(t) == 9U, "");

    /* set_priority: 非法值 */
    st_expect("TASK", "set_priority_32", rtos_task_set_priority(NULL, 32), RTOS_ERR_PARAM);

    /* 通知: 自通知 + 三种类型语义 */
    v = 0;
    st_expect("NOTIFY", "value_send", rtos_task_notify(t, 0x1111U, RTOS_NOTIFY_VALUE), RTOS_OK);
    r = rtos_task_notify_wait(RTOS_NO_WAIT, &v);
    ST_CHECK("NOTIFY", "value_recv", r == RTOS_OK && v == 0x1111U, "r=%d v=%x", (int)r, (unsigned)v);

    st_expect("NOTIFY", "incr_send1", rtos_task_notify(t, 0U, RTOS_NOTIFY_INCREMENT), RTOS_OK);
    st_expect("NOTIFY", "incr_send2", rtos_task_notify(t, 0U, RTOS_NOTIFY_INCREMENT), RTOS_OK);
    v = 0;
    r = rtos_task_notify_wait(RTOS_NO_WAIT, &v);
    ST_CHECK("NOTIFY", "incr_recv_2", r == RTOS_OK && v == 2U, "r=%d v=%u", (int)r, (unsigned)v);

    st_expect("NOTIFY", "bit_send1", rtos_task_notify(t, 0x40U, RTOS_NOTIFY_BIT), RTOS_OK);
    st_expect("NOTIFY", "bit_send2", rtos_task_notify(t, 0x02U, RTOS_NOTIFY_BIT), RTOS_OK);
    v = 0;
    r = rtos_task_notify_wait(RTOS_NO_WAIT, &v);
    ST_CHECK("NOTIFY", "bit_recv_or", r == RTOS_OK && v == 0x42U, "r=%d v=%x", (int)r, (unsigned)v);

    /* notify_wait: 无通知 NO_WAIT / 超时 */
    r = rtos_task_notify_wait(RTOS_NO_WAIT, &v);
    st_expect("NOTIFY", "wait_nowait_empty", r, RTOS_ERR_TIMEOUT);
    {
        rtos_tick_t t0 = rtos_sched_get_tick_count();
        rtos_tick_t dt;
        r = rtos_task_notify_wait(40, &v);
        dt = rtos_sched_get_tick_count() - t0;
        ST_CHECK("NOTIFY", "wait_timeout40", r == RTOS_ERR_TIMEOUT && dt >= 40U && dt <= 46U,
                 "r=%d dt=%u", (int)r, (unsigned)dt);
    }

    /* 任务函数正常返回 → 兜底自删除 */
    {
        static volatile uint32_t returned_ran;
        returned_ran = 0;
        /* 返回即退出的小任务(靠内核 exit handler 兜底) */
        r = rtos_task_create(&t, 128, st_returning_fn, (void *)&returned_ran, 12, "ret");
        st_expect("TASK", "create_returning", r, RTOS_OK);
        if (r != RTOS_OK) {
            ST_INFO("TASK", "returning_skip", "create failed, module remainder skipped");
            return;
        }
        rtos_task_delay(80);
        ST_CHECK("TASK", "returning_ran", returned_ran == 1U, "");
        rtos_task_delay(200); /* idle 回收 */
    }

    /* delete: 删除阻塞中的任务 */
    {
        rtos_sem_t s;
        rtos_tcb_t *vt;
        rtos_perf_task_t arr[RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE];
        uint32_t n0 = 0, n1 = 0;

        rtos_perf_get_all_tasks(arr, RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE, &n0);
        rtos_sem_init(&s, 0, 1);
        memset((void *)&vc, 0, sizeof(vc));
        vc.sem = &s;
        vc.timeout = RTOS_WAIT_FOREVER;
        r = rtos_task_create(&vt, 256, st_victim_sem_fn, &vc, 12, "vdel");
        st_expect("TASK", "create_victim", r, RTOS_OK);
        if (r != RTOS_OK) {
            ST_INFO("TASK", "delete_blocked_skip", "create failed");
            return;
        }
        rtos_task_delay(30); /* victim 阻塞 */
        st_expect("TASK", "delete_blocked", rtos_task_delete(vt), RTOS_OK);
        rtos_task_delay(150); /* idle 回收 TCB */
        rtos_perf_get_all_tasks(arr, RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE, &n1);
        ST_CHECK("TASK", "delete_registry_sync", (n1 == n0), "n0=%u n1=%u", (unsigned)n0, (unsigned)n1);
    }

    /* delete(NULL) 自删: 由小任务自己做 */
    {
        extern void st_suicide_fn(void *arg);
        static volatile uint32_t dead_flag;
        dead_flag = 0;
        r = rtos_task_create(&t, 128, st_suicide_fn, (void *)&dead_flag, 12, "dead");
        st_expect("TASK", "create_suicide", r, RTOS_OK);
        if (r != RTOS_OK) {
            ST_INFO("TASK", "suicide_skip", "create failed");
            return;
        }
        rtos_task_delay(100);
        ST_CHECK("TASK", "self_delete_ran", dead_flag == 1U, "");
    }

    /* suspend/resume: 幂等 resume 未挂起任务 */
    {
        rtos_sem_t s;
        rtos_tcb_t *vt;
        rtos_sem_init(&s, 0, 1);
        memset((void *)&vc, 0, sizeof(vc));
        vc.sem = &s;
        vc.timeout = RTOS_WAIT_FOREVER;
        r = rtos_task_create(&vt, 256, st_victim_sem_fn, &vc, 12, "vsus");
        st_expect("TASK", "create_vsus", r, RTOS_OK);
        if (r != RTOS_OK) {
            ST_INFO("TASK", "vsus_skip", "create failed");
            return;
        }
        st_expect("TASK", "resume_not_suspended", rtos_task_resume(vt), RTOS_OK);
        rtos_task_delay(30); /* victim 阻塞中 */

        /* 挂起阻塞中任务 → resume → 其 take 返回 ERR_DELETED */
        st_expect("TASK", "suspend_blocked", rtos_task_suspend(vt), RTOS_OK);
        st_expect("TASK", "resume_blocked", rtos_task_resume(vt), RTOS_OK);
        rtos_task_delay(80);
        ST_CHECK("TASK", "blocked_api_deleted", vc.done == 1U && vc.result == RTOS_ERR_DELETED,
                 "done=%u r=%d", (unsigned)vc.done, (int)vc.result);
        rtos_task_delete(vt);
    }

    /* set_priority 生效 + 提升后可抢占 */
    {
        rtos_tcb_t *vt;
        static volatile uint32_t boosted_ran;
        extern void st_boost_fn(void *arg);
        boosted_ran = 0;
        r = rtos_task_create(&vt, 128, st_boost_fn, (void *)&boosted_ran, 11, "boost");
        st_expect("TASK", "create_boost", r, RTOS_OK);
        if (r != RTOS_OK) {
            ST_INFO("TASK", "boost_skip", "create failed");
            return;
        }
        rtos_task_delay(30); /* boost 阻塞等通知 */
        st_expect("TASK", "set_priority_up", rtos_task_set_priority(vt, 3), RTOS_OK);
        ST_CHECK("TASK", "priority_now_3", rtos_task_get_priority(vt) == 3U,
                 "prio=%u", (unsigned)rtos_task_get_priority(vt));
        st_expect("TASK", "notify_boosted", rtos_task_notify(vt, 1U, RTOS_NOTIFY_VALUE), RTOS_OK);
        rtos_task_delay(50);
        ST_CHECK("TASK", "boosted_ran", boosted_ran == 1U, "");
        st_expect("TASK", "set_priority_back", rtos_task_set_priority(vt, 11), RTOS_OK);
        rtos_task_delete(vt);
    }
}

/* 通用超时断言(供 queue/sem 等复用) */
static void st_timeout_check(const char *mod, const char *name, rtos_status_t r, rtos_tick_t dt)
{
    ST_CHECK(mod, name, r == RTOS_ERR_TIMEOUT && dt >= 40U && dt <= 46U,
             "r=%d dt=%u", (int)r, (unsigned)dt);
}

/* ---------------- QUEUE 模块 ---------------- */

static void test_queue(void)
{
    rtos_queue_t q;
    static uint8_t storage[4 * 4]; /* item_size=4, capacity=4 */
    uint32_t v;
    rtos_status_t r;
    rtos_tcb_t *vt;

    st_expect("QUEUE", "init_ok", rtos_queue_init(&q, storage, 4, 4), RTOS_OK);

    /* FIFO 顺序 */
    for (v = 1; v <= 4; v++) {
        st_expect("QUEUE", "send_fifo", rtos_queue_send(&q, &v, RTOS_NO_WAIT, RTOS_FALSE), RTOS_OK);
    }
    ST_CHECK("QUEUE", "count_4", rtos_queue_get_count(&q) == 4U, "cnt=%u", (unsigned)rtos_queue_get_count(&q));
    ST_CHECK("QUEUE", "free_0", rtos_queue_get_free(&q) == 0U, "free=%u", (unsigned)rtos_queue_get_free(&q));

    /* 满 + NO_WAIT / 超时 */
    v = 99;
    st_expect("QUEUE", "full_send_nowait", rtos_queue_send(&q, &v, RTOS_NO_WAIT, RTOS_FALSE), RTOS_ERR_TIMEOUT);
    {
        rtos_tick_t t0 = rtos_sched_get_tick_count();
        r = rtos_queue_send(&q, &v, 40, RTOS_FALSE);
        st_timeout_check("QUEUE", "full_send_timeout40", r, rtos_sched_get_tick_count() - t0);
    }

    for (v = 1; v <= 4; v++) {
        uint32_t got = 0;
        st_expect("QUEUE", "recv_fifo", rtos_queue_recv(&q, &got, RTOS_NO_WAIT), RTOS_OK);
        ST_CHECK("QUEUE", "fifo_order", got == v, "got=%u want=%u", (unsigned)got, (unsigned)v);
    }

    /* 空 + NO_WAIT / 超时 */
    st_expect("QUEUE", "empty_recv_nowait", rtos_queue_recv(&q, &v, RTOS_NO_WAIT), RTOS_ERR_TIMEOUT);
    {
        rtos_tick_t t0 = rtos_sched_get_tick_count();
        r = rtos_queue_recv(&q, &v, 40);
        st_timeout_check("QUEUE", "empty_recv_timeout40", r, rtos_sched_get_tick_count() - t0);
    }

    /* to_front 插队 */
    v = 1; rtos_queue_send(&q, &v, RTOS_NO_WAIT, RTOS_FALSE);
    v = 2; rtos_queue_send(&q, &v, RTOS_NO_WAIT, RTOS_FALSE);
    v = 9; rtos_queue_send(&q, &v, RTOS_NO_WAIT, RTOS_TRUE); /* 插队首 */
    {
        uint32_t got = 0;
        rtos_queue_recv(&q, &got, RTOS_NO_WAIT);
        ST_CHECK("QUEUE", "to_front_first", got == 9U, "got=%u", (unsigned)got);
        rtos_queue_recv(&q, &got, RTOS_NO_WAIT);
        ST_CHECK("QUEUE", "to_front_second", got == 1U, "got=%u", (unsigned)got);
        rtos_queue_recv(&q, &got, RTOS_NO_WAIT);
        ST_CHECK("QUEUE", "to_front_third", got == 2U, "got=%u", (unsigned)got);
    }

    /* 阻塞接收 + 唤醒数据传递 */
    memset((void *)&vc, 0, sizeof(vc));
    vc.q = &q;
    vc.timeout = RTOS_WAIT_FOREVER;
    r = rtos_task_create(&vt, 256, st_victim_queue_fn, &vc, 12, "vq");
    st_expect("QUEUE", "create_recv_victim", r, RTOS_OK);
    if (r != RTOS_OK) {
        ST_INFO("QUEUE", "recv_victim_skip", "create failed, module remainder skipped");
        return;
    }
    rtos_task_delay(30);
    v = 0x5A5AA5A5U;
    st_expect("QUEUE", "send_wakes_recv", rtos_queue_send(&q, &v, RTOS_NO_WAIT, RTOS_FALSE), RTOS_OK);
    rtos_task_delay(80);
    ST_CHECK("QUEUE", "recv_got_data", vc.done == 1U && vc.result == RTOS_OK && vc.data == 0x5A5AA5A5U,
             "done=%u r=%d d=%x", (unsigned)vc.done, (int)vc.result, (unsigned)vc.data);
    rtos_task_delete(vt);

    /* 阻塞发送(满) + 接收方取走后唤醒 */
    v = 1; rtos_queue_send(&q, &v, RTOS_NO_WAIT, RTOS_FALSE);
    v = 2; rtos_queue_send(&q, &v, RTOS_NO_WAIT, RTOS_FALSE);
    v = 3; rtos_queue_send(&q, &v, RTOS_NO_WAIT, RTOS_FALSE);
    v = 4; rtos_queue_send(&q, &v, RTOS_NO_WAIT, RTOS_FALSE); /* 满 */
    memset((void *)&vc, 0, sizeof(vc));
    vc.q = &q;
    vc.data = 0x77U;
    vc.timeout = RTOS_WAIT_FOREVER;
    r = rtos_task_create(&vt, 256, st_victim_queue_send_fn, &vc, 12, "vqs");
    st_expect("QUEUE", "create_send_victim", r, RTOS_OK);
    if (r != RTOS_OK) {
        ST_INFO("QUEUE", "send_victim_skip", "create failed, module remainder skipped");
        return;
    }
    rtos_task_delay(30); /* sender 阻塞在满队列 */
    {
        uint32_t got = 0;
        st_expect("QUEUE", "recv_open_slot", rtos_queue_recv(&q, &got, RTOS_NO_WAIT), RTOS_OK);
        rtos_task_delay(50); /* sender 醒来写入 0x77(FIFO 尾部) */
        ST_CHECK("QUEUE", "sender_woken", vc.done == 1U && vc.result == RTOS_OK,
                 "done=%u r=%d", (unsigned)vc.done, (int)vc.result);
        /* FIFO 语义: 剩余 2,3,4 依次读出, 0x77 最后到达队尾 */
        {
            uint32_t seq = 2;
            int ok = 1;
            while (seq <= 4U) {
                got = 0;
                if (rtos_queue_recv(&q, &got, RTOS_NO_WAIT) != RTOS_OK || got != seq) {
                    ok = 0;
                }
                seq++;
            }
            ST_CHECK("QUEUE", "fifo_drain_234", ok == 1, "");
            got = 0;
            st_expect("QUEUE", "recv_sent_data", rtos_queue_recv(&q, &got, RTOS_NO_WAIT), RTOS_OK);
            ST_CHECK("QUEUE", "sent_data_ok", got == 0x77U, "got=%x", (unsigned)got);
        }
    }
    rtos_task_delete(vt);

    /* 排空后 deinit: 唤醒等待者 */
    memset((void *)&vc, 0, sizeof(vc));
    vc.q = &q;
    vc.timeout = RTOS_WAIT_FOREVER;
    r = rtos_task_create(&vt, 256, st_victim_queue_fn, &vc, 12, "vqd");
    st_expect("QUEUE", "create_deinit_victim", r, RTOS_OK);
    if (r != RTOS_OK) {
        ST_INFO("QUEUE", "deinit_victim_skip", "create failed");
        return;
    }
    rtos_task_delay(30);
    st_expect("QUEUE", "deinit_ok", rtos_queue_deinit(&q), RTOS_OK);
    rtos_task_delay(80);
    ST_CHECK("QUEUE", "deinit_wakes_waiter", vc.done == 1U && vc.result == RTOS_ERR_DELETED,
             "done=%u r=%d", (unsigned)vc.done, (int)vc.result);
    v = 1;
    st_expect("QUEUE", "send_after_deinit", rtos_queue_send(&q, &v, RTOS_NO_WAIT, RTOS_FALSE), RTOS_ERR_PARAM);
    rtos_task_delete(vt);
}

/* ---------------- SEM 模块 ---------------- */

static void test_sem(void)
{
    rtos_sem_t s;
    rtos_status_t r;
    rtos_tcb_t *vt;

    /* 计数语义 */
    st_expect("SEM", "init_2_3", rtos_sem_init(&s, 2, 3), RTOS_OK);
    st_expect("SEM", "take1", rtos_sem_take(&s, RTOS_NO_WAIT), RTOS_OK);
    st_expect("SEM", "take2", rtos_sem_take(&s, RTOS_NO_WAIT), RTOS_OK);
    st_expect("SEM", "take3_empty", rtos_sem_take(&s, RTOS_NO_WAIT), RTOS_ERR_TIMEOUT);
    st_expect("SEM", "give_to_1", rtos_sem_give(&s), RTOS_OK);
    ST_CHECK("SEM", "count_1", rtos_sem_get_count(&s) == 1U, "cnt=%u", (unsigned)rtos_sem_get_count(&s));
    st_expect("SEM", "give_to_2", rtos_sem_give(&s), RTOS_OK);
    st_expect("SEM", "give_to_3", rtos_sem_give(&s), RTOS_OK);
    ST_CHECK("SEM", "count_3", rtos_sem_get_count(&s) == 3U, "cnt=%u", (unsigned)rtos_sem_get_count(&s));
    st_expect("SEM", "give_overflow", rtos_sem_give(&s), RTOS_ERR_OVERFLOW);
    st_expect("SEM", "take_from_max", rtos_sem_take(&s, RTOS_NO_WAIT), RTOS_OK);
    st_expect("SEM", "give_back_ok", rtos_sem_give(&s), RTOS_OK);

    /* 二值信号量 */
    st_expect("SEM", "binary_init", rtos_sem_init(&s, 0, 1), RTOS_OK);
    st_expect("SEM", "binary_take_empty", rtos_sem_take(&s, RTOS_NO_WAIT), RTOS_ERR_TIMEOUT);
    st_expect("SEM", "binary_give", rtos_sem_give(&s), RTOS_OK);
    st_expect("SEM", "binary_give_overflow", rtos_sem_give(&s), RTOS_ERR_OVERFLOW);
    st_expect("SEM", "binary_take", rtos_sem_take(&s, RTOS_NO_WAIT), RTOS_OK);

    /* 阻塞唤醒: give 直接移交, 计数不变 */
    st_expect("SEM", "wakeup_init", rtos_sem_init(&s, 0, 1), RTOS_OK);
    memset((void *)&vc, 0, sizeof(vc));
    vc.sem = &s;
    vc.timeout = RTOS_WAIT_FOREVER;
    r = rtos_task_create(&vt, 256, st_victim_sem_fn, &vc, 12, "vsw");
    st_expect("SEM", "create_taker", r, RTOS_OK);
    if (r != RTOS_OK) {
        ST_INFO("SEM", "taker_skip", "create failed, module remainder skipped");
        return;
    }
    rtos_task_delay(30);
    st_expect("SEM", "give_wakes", rtos_sem_give(&s), RTOS_OK);
    rtos_task_delay(80);
    ST_CHECK("SEM", "taker_ok_count0", vc.done == 1U && vc.result == RTOS_OK && vc.count_after_wake == 0U,
             "done=%u r=%d cnt=%u", (unsigned)vc.done, (int)vc.result, (unsigned)vc.count_after_wake);
    rtos_task_delete(vt);

    /* 阻塞超时精度 */
    memset((void *)&vc, 0, sizeof(vc));
    vc.sem = &s;
    vc.timeout = 40;
    r = rtos_task_create(&vt, 256, st_victim_sem_fn, &vc, 12, "vst");
    st_expect("SEM", "create_timeout_taker", r, RTOS_OK);
    if (r != RTOS_OK) {
        ST_INFO("SEM", "timeout_taker_skip", "create failed");
        return;
    }
    rtos_task_delay(100);
    ST_CHECK("SEM", "taker_timeout", vc.done == 1U && vc.result == RTOS_ERR_TIMEOUT,
             "done=%u r=%d", (unsigned)vc.done, (int)vc.result);
    rtos_task_delete(vt);

    /* deinit 唤醒 */
    memset((void *)&vc, 0, sizeof(vc));
    vc.sem = &s;
    vc.timeout = RTOS_WAIT_FOREVER;
    r = rtos_task_create(&vt, 256, st_victim_sem_fn, &vc, 12, "vsd");
    st_expect("SEM", "create_deinit_taker", r, RTOS_OK);
    if (r != RTOS_OK) {
        ST_INFO("SEM", "deinit_taker_skip", "create failed");
        return;
    }
    rtos_task_delay(30);
    st_expect("SEM", "deinit_ok", rtos_sem_deinit(&s), RTOS_OK);
    rtos_task_delay(80);
    ST_CHECK("SEM", "deinit_wakes_waiter", vc.done == 1U && vc.result == RTOS_ERR_DELETED,
             "done=%u r=%d", (unsigned)vc.done, (int)vc.result);
    rtos_task_delete(vt);
    st_expect("SEM", "take_after_deinit", rtos_sem_take(&s, RTOS_NO_WAIT), RTOS_ERR_PARAM);
}

/* sem 优先级唤醒顺序: 两个不同优先级 taker, 高优先级先拿到 */
static rtos_sem_t    wk_sem;
static volatile uint8_t wk_order[2];
static volatile uint8_t wk_n;
static void st_waker_fn(void *arg)
{
    uint8_t id = (uint8_t)(uintptr_t)arg;
    rtos_sem_take(&wk_sem, RTOS_WAIT_FOREVER);
    wk_order[wk_n] = id;
    wk_n++;
    for (;;) { rtos_task_delay(100); }
}

static void test_sem_priority_wake(void)
{
    rtos_tcb_t *lo, *hi;

    rtos_sem_init(&wk_sem, 0, 4);
    wk_n = 0;
    wk_order[0] = 0xFF;
    wk_order[1] = 0xFF;

    st_expect("SEM", "wake_create_lo", rtos_task_create(&lo, 128, st_waker_fn, (void *)(uintptr_t)2, 14, "wk_lo"), RTOS_OK);
    st_expect("SEM", "wake_create_hi", rtos_task_create(&hi, 128, st_waker_fn, (void *)(uintptr_t)1, 3, "wk_hi"), RTOS_OK);
    rtos_task_delay(30); /* 两个都阻塞(等待链表按优先级排序) */

    rtos_sem_give(&wk_sem); /* 高优先级先醒 */
    rtos_task_delay(30);
    rtos_sem_give(&wk_sem); /* 然后低优先级 */
    rtos_task_delay(50);

    ST_CHECK("SEM", "priority_wake_order", wk_n == 2U && wk_order[0] == 1U && wk_order[1] == 2U,
             "n=%u o0=%u o1=%u", (unsigned)wk_n, (unsigned)wk_order[0], (unsigned)wk_order[1]);
    rtos_task_delete(lo);
    rtos_task_delete(hi);
}

/* ---------------- MUTEX 模块 ---------------- */

/* 递归锁连环 take 的 victim: 每步经 notify 与主任务同步, 主任务逐层 give */
static rtos_mutex_t rec_m;
static volatile rtos_status_t rec_v_results[4];
static volatile uint8_t rec_v_step;
static void st_rec_victim_fn(void *arg)
{
    uint32_t v;
    (void)arg;
    rec_v_results[0] = rtos_mutex_take(&rec_m, RTOS_NO_WAIT); /* 主递归持有 → TIMEOUT */
    rec_v_step = 1;
    rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v); /* 等 give1 */
    rec_v_results[1] = rtos_mutex_take(&rec_m, RTOS_NO_WAIT); /* 仍持有 → TIMEOUT */
    rec_v_step = 2;
    rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v); /* 等 give2 */
    rec_v_results[2] = rtos_mutex_take(&rec_m, RTOS_NO_WAIT); /* 仍持有 → TIMEOUT */
    rec_v_step = 3;
    rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v); /* 等 give3(归零) */
    rec_v_results[3] = rtos_mutex_take(&rec_m, RTOS_NO_WAIT); /* 已释放 → OK+移交 */
    rec_v_step = 4;
    rtos_mutex_give(&rec_m); /* victim 已是 owner, give OK */
    rec_v_step = 5;
    for (;;) { rtos_task_delay(100); }
}

/* 优先级继承: low 持锁, high 阻塞 */
static rtos_mutex_t pi_m;
static rtos_tcb_t  *pi_low_tcb;
static volatile rtos_status_t pi_high_ret;
static volatile uint8_t      pi_locked, pi_released, pi_high_done;
static void st_pi_low_fn(void *arg)
{
    uint32_t v;
    (void)arg;
    rtos_mutex_take(&pi_m, RTOS_WAIT_FOREVER);
    pi_locked = 1;
    rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v); /* 等主任务通知释放 */
    rtos_mutex_give(&pi_m);
    pi_released = 1;
    for (;;) { rtos_task_delay(100); }
}
static void st_pi_high_fn(void *arg)
{
    (void)arg;
    pi_high_ret = rtos_mutex_take(&pi_m, RTOS_WAIT_FOREVER);
    rtos_mutex_give(&pi_m);
    pi_high_done = 1;
    for (;;) { rtos_task_delay(100); }
}

static void test_mutex(void)
{
    rtos_mutex_t m;
    rtos_status_t r;
    rtos_tcb_t *vt;

    st_expect("MUTEX", "init_ok", rtos_mutex_init(&m), RTOS_OK);

    /* 基本获取/释放 */
    st_expect("MUTEX", "take_ok", rtos_mutex_take(&m, RTOS_NO_WAIT), RTOS_OK);
    st_expect("MUTEX", "give_ok", rtos_mutex_give(&m), RTOS_OK);

    /* 未持锁释放(边界外) */
    st_expect("MUTEX", "give_unowned", rtos_mutex_give(&m), RTOS_ERR_PARAM);

    /* 他人持锁: NO_WAIT / 超时 */
    st_expect("MUTEX", "take_hold", rtos_mutex_take(&m, RTOS_NO_WAIT), RTOS_OK);
    memset((void *)&vc, 0, sizeof(vc));
    vc.mtx = &m;
    vc.timeout = RTOS_NO_WAIT;
    r = rtos_task_create(&vt, 256, st_victim_mutex_fn, &vc, 12, "vm");
    st_expect("MUTEX", "create_victim", r, RTOS_OK);
    rtos_task_delay(30);
    ST_CHECK("MUTEX", "other_held_nowait", vc.done == 1U && vc.result == RTOS_ERR_TIMEOUT,
             "done=%u r=%d", (unsigned)vc.done, (int)vc.result);
    rtos_task_delete(vt);

    memset((void *)&vc, 0, sizeof(vc));
    vc.mtx = &m;
    vc.timeout = 40;
    r = rtos_task_create(&vt, 256, st_victim_mutex_fn, &vc, 12, "vmt");
    st_expect("MUTEX", "create_timeout_victim", r, RTOS_OK);
    rtos_task_delay(100);
    ST_CHECK("MUTEX", "other_held_timeout", vc.done == 1U && vc.result == RTOS_ERR_TIMEOUT,
             "done=%u r=%d", (unsigned)vc.done, (int)vc.result);
    rtos_task_delete(vt);
    st_expect("MUTEX", "release", rtos_mutex_give(&m), RTOS_OK);

    /* 递归锁: take×3 → 逐层 give(与 victim 步进同步), 验证期间他人不可获取 */
    st_expect("MUTEX", "rec_init", rtos_mutex_init(&rec_m), RTOS_OK);
    st_expect("MUTEX", "rec_take1", rtos_mutex_take(&rec_m, RTOS_NO_WAIT), RTOS_OK);
    st_expect("MUTEX", "rec_take2", rtos_mutex_take(&rec_m, RTOS_NO_WAIT), RTOS_OK);
    st_expect("MUTEX", "rec_take3", rtos_mutex_take(&rec_m, RTOS_NO_WAIT), RTOS_OK);
    rec_v_step = 0;
    r = rtos_task_create(&vt, 256, st_rec_victim_fn, NULL, 12, "vrec");
    st_expect("MUTEX", "create_rec_victim", r, RTOS_OK);
    if (r != RTOS_OK) {
        ST_INFO("MUTEX", "rec_victim_skip", "create failed");
        return;
    }
    {
        uint32_t spin;
        for (spin = 0; (spin < 20000U) && (rec_v_step < 1U); spin++) { rtos_task_delay(2); }
        ST_CHECK("MUTEX", "rec_held_nowait", rec_v_results[0] == RTOS_ERR_TIMEOUT,
                 "r0=%d", (int)rec_v_results[0]);
        st_expect("MUTEX", "rec_give1_still_held", rtos_mutex_give(&rec_m), RTOS_OK);
        rtos_task_notify(vt, 1U, RTOS_NOTIFY_VALUE);
        for (spin = 0; (spin < 20000U) && (rec_v_step < 2U); spin++) { rtos_task_delay(2); }
        ST_CHECK("MUTEX", "rec_give1_victim_timeout", rec_v_results[1] == RTOS_ERR_TIMEOUT,
                 "r1=%d", (int)rec_v_results[1]);
        st_expect("MUTEX", "rec_give2_still_held", rtos_mutex_give(&rec_m), RTOS_OK);
        rtos_task_notify(vt, 1U, RTOS_NOTIFY_VALUE);
        for (spin = 0; (spin < 20000U) && (rec_v_step < 3U); spin++) { rtos_task_delay(2); }
        ST_CHECK("MUTEX", "rec_give2_victim_timeout", rec_v_results[2] == RTOS_ERR_TIMEOUT,
                 "r2=%d", (int)rec_v_results[2]);
        st_expect("MUTEX", "rec_give3_release", rtos_mutex_give(&rec_m), RTOS_OK); /* 归零 → 移交 */
        rtos_task_notify(vt, 1U, RTOS_NOTIFY_VALUE);
        for (spin = 0; (spin < 20000U) && (rec_v_step < 5U); spin++) { rtos_task_delay(2); }
        ST_CHECK("MUTEX", "recursion_transfer", rec_v_results[3] == RTOS_OK,
                 "r3=%d step=%u", (int)rec_v_results[3], (unsigned)rec_v_step);
    }
    rtos_task_delete(vt);

    /* 主任务已不持有 rec_m(victim 持有后已 give), 验证主任务未持锁 give */
    st_expect("MUTEX", "give_after_transfer", rtos_mutex_give(&rec_m), RTOS_ERR_PARAM);

    /* 优先级继承: low(20) 持锁 → high(4) 阻塞 → low 被提升 → 释放恢复 */
    st_expect("MUTEX", "pi_init", rtos_mutex_init(&pi_m), RTOS_OK);
    pi_locked = 0; pi_released = 0; pi_high_done = 0;
    r = rtos_task_create(&pi_low_tcb, 256, st_pi_low_fn, NULL, 20, "pi_low");
    st_expect("MUTEX", "pi_create_low", r, RTOS_OK);
    rtos_task_delay(30);
    ST_CHECK("MUTEX", "pi_low_locked", pi_locked == 1U, "");

    r = rtos_task_create(&vt, 256, st_pi_high_fn, NULL, 4, "pi_high");
    st_expect("MUTEX", "pi_create_high", r, RTOS_OK);
    rtos_task_delay(30); /* high 阻塞 → low 应被提升 */

    ST_CHECK("MUTEX", "pi_inherited", rtos_task_get_priority(pi_low_tcb) == 4U,
             "low_prio=%u want=4", (unsigned)rtos_task_get_priority(pi_low_tcb));
    ST_CHECK("MUTEX", "pi_high_still_wait", pi_high_done == 0U, "");

    st_expect("MUTEX", "pi_notify_release", rtos_task_notify(pi_low_tcb, 1U, RTOS_NOTIFY_VALUE), RTOS_OK);
    rtos_task_delay(100);
    ST_CHECK("MUTEX", "pi_released", pi_released == 1U, "");
    ST_CHECK("MUTEX", "pi_priority_restored", rtos_task_get_priority(pi_low_tcb) == 20U,
             "low_prio=%u want=20", (unsigned)rtos_task_get_priority(pi_low_tcb));
    ST_CHECK("MUTEX", "pi_high_got_lock", pi_high_done == 1U && pi_high_ret == RTOS_OK,
             "done=%u r=%d", (unsigned)pi_high_done, (int)pi_high_ret);
    rtos_task_delete(pi_low_tcb);
    rtos_task_delete(vt);

    /* deinit 唤醒 */
    st_expect("MUTEX", "di_init", rtos_mutex_init(&m), RTOS_OK);
    memset((void *)&vc, 0, sizeof(vc));
    vc.mtx = &m;
    vc.timeout = RTOS_WAIT_FOREVER;
    st_expect("MUTEX", "di_take", rtos_mutex_take(&m, RTOS_NO_WAIT), RTOS_OK);
    r = rtos_task_create(&vt, 256, st_victim_mutex_fn, &vc, 12, "vmd");
    st_expect("MUTEX", "create_deinit_victim", r, RTOS_OK);
    rtos_task_delay(30);
    st_expect("MUTEX", "deinit_ok", rtos_mutex_deinit(&m), RTOS_OK);
    rtos_task_delay(80);
    ST_CHECK("MUTEX", "deinit_wakes_waiter", vc.done == 1U && vc.result == RTOS_ERR_DELETED,
             "done=%u r=%d", (unsigned)vc.done, (int)vc.result);
    rtos_task_delete(vt);
}

/* ---------------- EVENT 模块 ---------------- */

static void test_event(void)
{
    rtos_event_t e;
    rtos_event_bits_t b;
    rtos_status_t r;
    rtos_tcb_t *vt;

    st_expect("EVENT", "init_ok", rtos_event_init(&e), RTOS_OK);

    /* set/clear 返回值与粘性 */
    b = rtos_event_set(&e, 0x05U);
    ST_CHECK("EVENT", "set_ret", b == 0x05U, "b=%x", (unsigned)b);
    b = rtos_event_set(&e, 0x02U);
    ST_CHECK("EVENT", "set_sticky", b == 0x07U, "b=%x", (unsigned)b);
    b = rtos_event_clear(&e, 0x04U);
    ST_CHECK("EVENT", "clear_ret", b == 0x03U, "b=%x", (unsigned)b);
    ST_CHECK("EVENT", "get", rtos_event_get(&e) == 0x03U, "b=%x", (unsigned)rtos_event_get(&e));

    /* WAIT_ALL 不满足/满足 */
    b = rtos_event_wait(&e, 0x03U, RTOS_EVENT_WAIT_ALL, RTOS_NO_WAIT);
    ST_CHECK("EVENT", "wait_all_nowait_hit", (b & 0x03U) == 0x03U, "b=%x", (unsigned)b);
    b = rtos_event_wait(&e, 0x0BU, RTOS_EVENT_WAIT_ALL, RTOS_NO_WAIT);
    ST_CHECK("EVENT", "wait_all_nowait_miss", (b & 0x0BU) != 0x0BU, "b=%x", (unsigned)b);

    /* WAIT_ANY */
    rtos_event_clear(&e, 0x07U);
    rtos_event_set(&e, 0x04U);
    b = rtos_event_wait(&e, 0x0CU, RTOS_EVENT_WAIT_ANY, RTOS_NO_WAIT);
    ST_CHECK("EVENT", "wait_any_partial", (b & 0x0CU) == 0x04U, "b=%x", (unsigned)b);

    /* CLEAR_ON_EXIT: 只清等待位 */
    rtos_event_clear(&e, 0xFFFFFFFFU);
    rtos_event_set(&e, 0x03U);
    b = rtos_event_wait(&e, 0x01U, RTOS_EVENT_WAIT_ANY | RTOS_EVENT_CLEAR_ON_EXIT, RTOS_NO_WAIT);
    ST_CHECK("EVENT", "clear_on_exit_hit", (b & 0x01U) == 0x01U, "b=%x", (unsigned)b);
    ST_CHECK("EVENT", "clear_on_exit_cleared", (rtos_event_get(&e) & 0x01U) == 0U, "b=%x", (unsigned)rtos_event_get(&e));
    ST_CHECK("EVENT", "clear_on_exit_kept", (rtos_event_get(&e) & 0x02U) == 0x02U, "b=%x", (unsigned)rtos_event_get(&e));

    /* 非法参数(边界外): 返回 0 */
    ST_CHECK("EVENT", "wait_bits0", rtos_event_wait(&e, 0U, RTOS_EVENT_WAIT_ANY, RTOS_NO_WAIT) == 0U, "");
    ST_CHECK("EVENT", "wait_bad_mode", rtos_event_wait(&e, 0x01U, 0x02U, RTOS_NO_WAIT) == 0U, "");
    ST_CHECK("EVENT", "wait_null", rtos_event_wait(NULL, 0x01U, RTOS_EVENT_WAIT_ANY, RTOS_NO_WAIT) == 0U, "");
    {
        rtos_event_t dead;
        memset(&dead, 0, sizeof(dead));
        ST_CHECK("EVENT", "wait_uninit", rtos_event_wait(&dead, 0x01U, RTOS_EVENT_WAIT_ANY, RTOS_NO_WAIT) == 0U, "");
    }

    /* 24 位有效位宽 */
    rtos_event_clear(&e, 0xFFFFFFU);
    b = rtos_event_set(&e, 1UL << 23);
    ST_CHECK("EVENT", "bit23_usable", (b & (1UL << 23)) != 0U, "b=%x", (unsigned)b);

    /* 阻塞 + set 唤醒 */
    rtos_event_clear(&e, 0xFFFFFFU);
    memset((void *)&vc, 0, sizeof(vc));
    vc.ev = &e;
    vc.wait_bits = 0x01U;
    vc.mode = RTOS_EVENT_WAIT_ANY;
    vc.timeout = RTOS_WAIT_FOREVER;
    r = rtos_task_create(&vt, 256, st_victim_event_fn, &vc, 12, "ve");
    st_expect("EVENT", "create_waiter", r, RTOS_OK);
    rtos_task_delay(30);
    rtos_event_set(&e, 0x01U);
    rtos_task_delay(80);
    ST_CHECK("EVENT", "set_wakes_waiter", vc.done == 1U && (vc.data & 0x01U) != 0U,
             "done=%u d=%x", (unsigned)vc.done, (unsigned)vc.data);
    rtos_task_delete(vt);

    /* 超时: 返回当前位(不含等待位) */
    rtos_event_clear(&e, 0xFFFFFFU);
    rtos_event_set(&e, 0x80U); /* 等待位以外的位 */
    memset((void *)&vc, 0, sizeof(vc));
    vc.ev = &e;
    vc.wait_bits = 0x01U;
    vc.mode = RTOS_EVENT_WAIT_ANY;
    vc.timeout = 40;
    r = rtos_task_create(&vt, 256, st_victim_event_fn, &vc, 12, "vet");
    st_expect("EVENT", "create_timeout_waiter", r, RTOS_OK);
    rtos_task_delay(100);
    ST_CHECK("EVENT", "wait_timeout_cur_bits", vc.done == 1U && (vc.data & 0x01U) == 0U && (vc.data & 0x80U) != 0U,
             "done=%u d=%x", (unsigned)vc.done, (unsigned)vc.data);
    rtos_task_delete(vt);

    /* 一次 set 同时唤醒两个不同等待位的任务 */
    {
        victim_ctx_t vc2;
        rtos_event_clear(&e, 0xFFFFFFU);
        memset((void *)&vc, 0, sizeof(vc));
        memset((void *)&vc2, 0, sizeof(vc2));
        vc.ev = &e;  vc.wait_bits = 0x01U; vc.mode = RTOS_EVENT_WAIT_ANY; vc.timeout = RTOS_WAIT_FOREVER;
        vc2.ev = &e; vc2.wait_bits = 0x02U; vc2.mode = RTOS_EVENT_WAIT_ANY; vc2.timeout = RTOS_WAIT_FOREVER;
        r = rtos_task_create(&vt, 256, st_victim_event_fn, &vc, 12, "ve1");
        st_expect("EVENT", "create_multi1", r, RTOS_OK);
        {
            rtos_tcb_t *vt2;
            r = rtos_task_create(&vt2, 256, st_victim_event_fn, &vc2, 13, "ve2");
            st_expect("EVENT", "create_multi2", r, RTOS_OK);
            rtos_task_delay(30);
            rtos_event_set(&e, 0x03U); /* 两位同时置位 */
            rtos_task_delay(80);
            ST_CHECK("EVENT", "multi_wake_both",
                     vc.done == 1U && vc2.done == 1U && (vc.data & 0x01U) != 0U && (vc2.data & 0x02U) != 0U,
                     "d1=%u d2=%u", (unsigned)vc.done, (unsigned)vc2.done);
            rtos_task_delete(vt2);
        }
        rtos_task_delete(vt);
    }

    /* deinit: 等待者以返回 0 的形式醒来 */
    rtos_event_clear(&e, 0xFFFFFFU);
    memset((void *)&vc, 0, sizeof(vc));
    vc.ev = &e;
    vc.wait_bits = 0x01U;
    vc.mode = RTOS_EVENT_WAIT_ANY;
    vc.timeout = RTOS_WAIT_FOREVER;
    r = rtos_task_create(&vt, 256, st_victim_event_fn, &vc, 12, "ved");
    st_expect("EVENT", "create_deinit_waiter", r, RTOS_OK);
    rtos_task_delay(30);
    st_expect("EVENT", "deinit_ok", rtos_event_deinit(&e), RTOS_OK);
    rtos_task_delay(80);
    ST_CHECK("EVENT", "deinit_wakes_waiter_zero", vc.done == 1U && vc.data == 0U,
             "done=%u d=%x", (unsigned)vc.done, (unsigned)vc.data);
    rtos_task_delete(vt);
}

/* ---------------- TIMER 模块 ---------------- */

static rtos_timer_t tmr;
static volatile uint32_t tmr_cnt;
static volatile uint32_t tmr_ticks[8];
static volatile rtos_status_t tmr_cb_send_ret;

static void tmr_cb(void *arg)
{
    (void)arg;
    if (tmr_cnt < 8U) {
        tmr_ticks[tmr_cnt] = rtos_sched_get_tick_count();
    }
    tmr_cnt++;
}

/* 回调上下文验证: 回调(定时器服务任务)内做非阻塞 queue send */
static rtos_queue_t tmr_cb_q;
static uint8_t      tmr_cb_qbuf[4];
static void tmr_cb2(void *arg)
{
    uint32_t v = 0x66U;
    (void)arg;
    tmr_cb_send_ret = rtos_queue_send(&tmr_cb_q, &v, RTOS_NO_WAIT, RTOS_FALSE);
}

/* 回调内自重启(one-shot) */
static rtos_timer_t tmr_self;
static volatile uint32_t tmr_self_cnt;
static volatile rtos_status_t tmr_self_restart_ret;
static void tmr_self_cb(void *arg)
{
    (void)arg;
    tmr_self_cnt++;
    if (tmr_self_cnt == 1U) {
        tmr_self_restart_ret = rtos_timer_start(&tmr_self);
    }
}

/* delete 竞态测试用 */
static volatile uint32_t tmr_t3_cnt;
static void tmr_t3_cb(void *arg)
{
    (void)arg;
    tmr_t3_cnt++;
}

static void test_timer(void)
{
    rtos_timer_t t2;
    rtos_tick_t t0, tstart;
    uint32_t i;
    rtos_status_t r;

    /* 未初始化对象上的命令(边界外) */
    memset(&t2, 0, sizeof(t2));
    st_expect("TIMER", "uninit_start", rtos_timer_start(&t2), RTOS_ERR_PARAM);
    st_expect("TIMER", "uninit_stop", rtos_timer_stop(&t2), RTOS_ERR_PARAM);
    st_expect("TIMER", "uninit_change0", rtos_timer_change_period(&t2, 0), RTOS_ERR_PARAM);

    /* stop 未启动的定时器(幂等) */
    st_expect("TIMER", "create_t2", rtos_timer_create(&t2, "t2", tmr_cb, NULL, 100, RTOS_TIMER_ONE_SHOT), RTOS_OK);
    st_expect("TIMER", "stop_not_started", rtos_timer_stop(&t2), RTOS_OK);

    /* one-shot: 只触发一次, 时刻准确 */
    tmr_cnt = 0;
    st_expect("TIMER", "create_oneshot", rtos_timer_create(&tmr, "os", tmr_cb, NULL, 100, RTOS_TIMER_ONE_SHOT), RTOS_OK);
    t0 = rtos_sched_get_tick_count();
    st_expect("TIMER", "start_oneshot", rtos_timer_start(&tmr), RTOS_OK);
    rtos_task_delay(260);
    ST_CHECK("TIMER", "oneshot_once", tmr_cnt == 1U, "cnt=%u", (unsigned)tmr_cnt);
    ST_CHECK("TIMER", "oneshot_timing", (tmr_ticks[0] - t0) >= 95U && (tmr_ticks[0] - t0) <= 130U,
             "dt=%u", (unsigned)(tmr_ticks[0] - t0));

    /* 重复 start 重计时 */
    tmr_cnt = 0;
    tstart = rtos_sched_get_tick_count();
    rtos_timer_start(&tmr);
    rtos_task_delay(60);
    rtos_timer_start(&tmr); /* 重新计时: 到期时刻 ≈ tstart+60+100 */
    rtos_task_delay(250);
    ST_CHECK("TIMER", "restart_retiming",
             tmr_cnt == 1U && (tmr_ticks[0] - tstart) >= 150U && (tmr_ticks[0] - tstart) <= 190U,
             "cnt=%u dt=%u", (unsigned)tmr_cnt, (unsigned)(tmr_ticks[0] - tstart));

    /* periodic: 周期触发 + 间隔精度 */
    tmr_cnt = 0;
    st_expect("TIMER", "create_periodic", rtos_timer_create(&tmr, "per", tmr_cb, NULL, 100, RTOS_TIMER_PERIODIC), RTOS_OK);
    t0 = rtos_sched_get_tick_count();
    rtos_timer_start(&tmr);
    rtos_task_delay(420);
    ST_CHECK("TIMER", "periodic_multi", tmr_cnt >= 3U && tmr_cnt <= 5U, "cnt=%u", (unsigned)tmr_cnt);
    for (i = 0; (i + 1U) < tmr_cnt && i < 7U; i++) {
        uint32_t gap = tmr_ticks[i + 1] - tmr_ticks[i];
        ST_CHECK("TIMER", "periodic_gap", gap >= 95U && gap <= 115U,
                 "i=%u gap=%u", (unsigned)i, (unsigned)gap);
    }

    /* stop 生效 */
    {
        uint32_t n = tmr_cnt;
        st_expect("TIMER", "stop_ok", rtos_timer_stop(&tmr), RTOS_OK);
        rtos_task_delay(260);
        ST_CHECK("TIMER", "stopped_no_fire", tmr_cnt == n, "n=%u cnt=%u", (unsigned)n, (unsigned)tmr_cnt);
    }

    /* change_period: 先重启(周期100), 稳定后改为 50 */
    tmr_cnt = 0;
    rtos_timer_start(&tmr);
    rtos_task_delay(150); /* 以 100ms 周期跑 1 个周期 */
    tmr_cnt = 0;
    t0 = rtos_sched_get_tick_count();
    st_expect("TIMER", "change_period", rtos_timer_change_period(&tmr, 50), RTOS_OK);
    rtos_task_delay(230); /* 新周期下 4 次左右 */
    ST_CHECK("TIMER", "change_period_faster", tmr_cnt >= 3U && tmr_cnt <= 5U, "cnt=%u", (unsigned)tmr_cnt);
    {
        uint32_t gap = tmr_ticks[1] - tmr_ticks[0];
        ST_CHECK("TIMER", "new_gap_50", gap >= 45U && gap <= 70U, "gap=%u", (unsigned)gap);
    }
    rtos_timer_stop(&tmr);

    /* 回调上下文: 非阻塞 queue send OK */
    tmr_cb_send_ret = RTOS_ERR_TIMEOUT; /* 哨兵: 期望被覆盖为 OK */
    rtos_queue_init(&tmr_cb_q, tmr_cb_qbuf, 4, 1);
    st_expect("TIMER", "create_cb_ctx", rtos_timer_create(&t2, "cb", tmr_cb2, NULL, 80, RTOS_TIMER_ONE_SHOT), RTOS_OK);
    rtos_timer_start(&t2);
    rtos_task_delay(200);
    ST_CHECK("TIMER", "cb_queue_send_ok", tmr_cb_send_ret == RTOS_OK,
             "r=%d", (int)tmr_cb_send_ret);
    rtos_timer_stop(&t2);

    /* 回调内自重启 */
    tmr_self_cnt = 0;
    tmr_self_restart_ret = RTOS_ERR_TIMEOUT;
    st_expect("TIMER", "create_self", rtos_timer_create(&tmr_self, "self", tmr_self_cb, NULL, 120, RTOS_TIMER_ONE_SHOT), RTOS_OK);
    rtos_timer_start(&tmr_self);
    rtos_task_delay(400);
    ST_CHECK("TIMER", "cb_self_restart", tmr_self_cnt >= 2U && tmr_self_restart_ret == RTOS_OK,
             "cnt=%u r=%d", (unsigned)tmr_self_cnt, (int)tmr_self_restart_ret);
    rtos_timer_stop(&tmr_self);

    /* delete 后立即 start: 命令被服务任务忽略(已知异步语义) */
    {
        rtos_timer_t t3;
        tmr_t3_cnt = 0;
        st_expect("TIMER", "create_t3", rtos_timer_create(&t3, "t3", tmr_t3_cb, NULL, 100, RTOS_TIMER_ONE_SHOT), RTOS_OK);
        rtos_timer_start(&t3);
        rtos_task_delay(50);
        rtos_timer_stop(&t3);
        r = rtos_timer_delete(&t3);
        st_expect("TIMER", "delete_ok", r, RTOS_OK);
        r = rtos_timer_start(&t3); /* is_initialized 仍为 1(异步), 入队 OK */
        ST_INFO("TIMER", "start_after_delete", "start ret=%d (enqueued; cmd dropped by service task)", (int)r);
        rtos_task_delay(300);
        ST_CHECK("TIMER", "deleted_no_fire", tmr_t3_cnt == 0U, "cnt=%u", (unsigned)tmr_t3_cnt);
    }
}

/* ---------------- MEMPOOL 模块 ---------------- */

static void test_mempool(void)
{
    static rtos_mempool_t pool;
    static uint8_t        base[6 * 4]; /* 4B 块 × 6 */
    void *p[6];
    int i;

    st_expect("MEM", "init_ok", rtos_mempool_init(&pool, base, 4, 6), RTOS_OK);
    ST_CHECK("MEM", "free_is_6", rtos_mempool_get_free(&pool) == 6U, "f=%u", (unsigned)rtos_mempool_get_free(&pool));

    /* 全部分配 + 指针互异 */
    for (i = 0; i < 6; i++) {
        p[i] = rtos_mempool_alloc(&pool);
    }
    ST_CHECK("MEM", "alloc_all", rtos_mempool_get_free(&pool) == 0U, "f=%u", (unsigned)rtos_mempool_get_free(&pool));
    {
        int distinct = 1;
        for (i = 0; i < 6; i++) {
            if (p[i] == NULL) { distinct = 0; }
        }
        ST_CHECK("MEM", "alloc_nonnull", distinct == 1, "");
    }
    ST_CHECK("MEM", "alloc_empty_null", rtos_mempool_alloc(&pool) == NULL, "");
    ST_CHECK("MEM", "alloc_null_pool", rtos_mempool_alloc(NULL) == NULL, "");

    /* 释放非法指针(边界外) */
    st_expect("MEM", "free_null", rtos_mempool_free(&pool, NULL), RTOS_ERR_NULL);
    st_expect("MEM", "free_out_of_range", rtos_mempool_free(&pool, (void *)0x20020000UL), RTOS_ERR_PARAM);

    /* 释放两个, 再分配成功(空闲链表头插: 先得 p[1]) */
    st_expect("MEM", "free_ok1", rtos_mempool_free(&pool, p[0]), RTOS_OK);
    st_expect("MEM", "free_ok2", rtos_mempool_free(&pool, p[1]), RTOS_OK);
    ST_CHECK("MEM", "free_is_2", rtos_mempool_get_free(&pool) == 2U, "f=%u", (unsigned)rtos_mempool_get_free(&pool));
    {
        void *rp = rtos_mempool_alloc(&pool);
        ST_CHECK("MEM", "realloc_ok", (rp == p[0]) || (rp == p[1]), "");
        ST_CHECK("MEM", "free_is_1", rtos_mempool_get_free(&pool) == 1U, "f=%u", (unsigned)rtos_mempool_get_free(&pool));
    }

    /* 非对齐 block_size(3) 向上取整到 4, 功能正常(存储区按对齐后块大小分配) */
    {
        static rtos_mempool_t pool2;
        static uint8_t        base2[8 * 4]; /* 8 块 × 对齐后 4B */
        void *a, *b;
        st_expect("MEM", "init_bs3", rtos_mempool_init(&pool2, base2, 3, 8), RTOS_OK);
        a = rtos_mempool_alloc(&pool2);
        b = rtos_mempool_alloc(&pool2);
        {
            uint32_t diff = (uint32_t)b - (uint32_t)a;
            ST_CHECK("MEM", "bs3_spacing4", a != NULL && b != NULL && (diff == 4U || diff == 0xFFFFFFFCU),
                     "d=%u", (unsigned)diff);
        }
        rtos_mempool_free(&pool2, a);
        rtos_mempool_free(&pool2, b);
    }

    /* 双重释放(已知不检测, 验证损坏后果供报告) —— 使用牺牲池 */
    {
        static rtos_mempool_t pool3;
        static uint8_t        base3[4 * 4];
        void *q1;
        st_expect("MEM", "sacrifice_init", rtos_mempool_init(&pool3, base3, 4, 4), RTOS_OK);
        q1 = rtos_mempool_alloc(&pool3);
        rtos_mempool_free(&pool3, q1);
        ST_CHECK("MEM", "pre_dblfree_f4", rtos_mempool_get_free(&pool3) == 4U,
                 "f=%u", (unsigned)rtos_mempool_get_free(&pool3));
        st_expect("MEM", "dblfree_ret", rtos_mempool_free(&pool3, q1), RTOS_OK); /* 第二次释放: 未拒绝 */
        ST_INFO("MEM", "dblfree_undetected",
                "free_count=%u > block_count=4 (freelist corrupted, pool abandoned)",
                (unsigned)rtos_mempool_get_free(&pool3));
    }
}

/* ---------------- HEAP 模块 ---------------- */

static void test_heap(void)
{
    void *p, *keep[40];
    uint32_t n = 0, f0, fc_before, fc_after, i;
    uint32_t buf32[4];

    p = rtos_heap_alloc(16);
    ST_CHECK("HEAP", "alloc_ok", p != NULL, "");
    if (p != NULL) {
        ST_CHECK("HEAP", "align8", ((uint32_t)p % 8U) == 0U, "p=0x%x", (unsigned)(uint32_t)p);
        *(volatile uint32_t *)p = 0x1234ABCDU;
        ST_CHECK("HEAP", "write_read", *(volatile uint32_t *)p == 0x1234ABCDU, "");
        rtos_heap_free(p);
    }

    ST_CHECK("HEAP", "alloc0_null", rtos_heap_alloc(0) == NULL, "");
    ST_CHECK("HEAP", "alloc_huge_null", rtos_heap_alloc(0x10000000U) == NULL, ""); /* 256MB, 安全失败 */

    /* 耗尽 + 全部回收 */
    f0 = rtos_heap_get_free();
    for (i = 0; i < 40U; i++) {
        keep[i] = rtos_heap_alloc(1024);
        if (keep[i] == NULL) { break; }
        n++;
    }
    ST_CHECK("HEAP", "exhaust_allocs", n >= 20U && n <= 33U, "n=%u", (unsigned)n);
    for (i = 0; i < n; i++) {
        rtos_heap_free(keep[i]);
    }
    /* [fix BUG-7 后账目精确] 顺序 free 26 块互不合并(仅前向合并的设计弱点),
     * 每块 header 8B 保留: 26×8=208B 碎片开销是真实物理占用而非账目错误 */
    ST_CHECK("HEAP", "free_recovered", rtos_heap_get_free() >= (f0 - 400U),
             "f0=%u now=%u", (unsigned)f0, (unsigned)rtos_heap_get_free());

    /* 双重释放: 静默忽略, 计数不变 */
    p = rtos_heap_alloc(32);
    rtos_heap_free(p);
    fc_before = rtos_heap_get_free_count();
    rtos_heap_free(p); /* 第二次 */
    fc_after = rtos_heap_get_free_count();
    ST_CHECK("HEAP", "dblfree_ignored", fc_after == fc_before,
             "before=%u after=%u", (unsigned)fc_before, (unsigned)fc_after);

    /* 野指针 / NULL: 静默忽略无崩溃 */
    rtos_heap_free(NULL);
    rtos_heap_free((void *)0x20020000UL);
    rtos_heap_free(buf32); /* 栈地址(堆范围外) */
    ST_CHECK("HEAP", "bogus_free_safe", 1, "");

    /* 计数器单调性 */
    {
        uint32_t a0 = rtos_heap_get_alloc_count();
        p = rtos_heap_alloc(16);
        ST_CHECK("HEAP", "alloc_count_inc", rtos_heap_get_alloc_count() == a0 + 1U,
                 "a0=%u a1=%u", (unsigned)a0, (unsigned)rtos_heap_get_alloc_count());
        rtos_heap_free(p);
    }
}

/* ---------------- PERF 模块 ---------------- */

static void test_perf(void)
{
    rtos_perf_system_t sys, sys2;
    rtos_perf_task_t  ts, arr[4];
    rtos_perf_memory_t mem;
    rtos_status_t r;
    uint32_t n = 0, i;

    st_expect("PERF", "get_system_null", rtos_perf_get_system(NULL), RTOS_ERR_NULL);
    r = rtos_perf_get_system(&sys);
    st_expect("PERF", "get_system_ok", r, RTOS_OK);
    if (r == RTOS_OK) {
        ST_CHECK("PERF", "tick_rate_1000", sys.tick_rate_hz == 1000U, "hz=%u", (unsigned)sys.tick_rate_hz);
        ST_CHECK("PERF", "cpu_freq_168m", sys.cpu_freq_hz == 168000000U, "hz=%u", (unsigned)sys.cpu_freq_hz);
        ST_CHECK("PERF", "uptime_pos", sys.uptime_ticks > 0U, "t=%u", (unsigned)sys.uptime_ticks);
        ST_CHECK("PERF", "task_count_pos", sys.task_count >= 2U, "n=%u", (unsigned)sys.task_count);
        ST_CHECK("PERF", "cpu_usage_range", sys.cpu_usage_x100 <= 10000U, "cpu=%u", (unsigned)sys.cpu_usage_x100);
#if RTOS_CONFIG_PERF_HOTPATH_STATS
        ST_CHECK("PERF", "switches_pos", sys.total_switches > 0U, "s=%u", (unsigned)sys.total_switches);
#else
        ST_INFO("PERF", "switches_stats_off", "HOTPATH_STATS=0: total_switches/cpu 派生统计不可用(零开销档)");
#endif
    }

    /* 窗口推进: 两次查询之间只做 delay(printf 耗时会污染窗口), 断言后置 */
    {
        rtos_perf_system_t s1, s2;
        rtos_status_t r1 = rtos_perf_get_system(&s1);
        rtos_task_delay(200);
        rtos_status_t r2 = rtos_perf_get_system(&s2);
        st_expect("PERF", "get_system_2nd", r2, RTOS_OK);
        if ((r1 == RTOS_OK) && (r2 == RTOS_OK)) {
            uint32_t du = s2.uptime_ticks - s1.uptime_ticks;
            ST_CHECK("PERF", "window_advances", du >= 198U && du <= 206U, "du=%u", (unsigned)du);
        }
    }

    st_expect("PERF", "get_task_null_stats", rtos_perf_get_task(NULL, NULL), RTOS_ERR_NULL);
    r = rtos_perf_get_task(NULL, &ts); /* NULL = 当前任务 */
    st_expect("PERF", "get_task_self", r, RTOS_OK);
    if (r == RTOS_OK) {
#if RTOS_CONFIG_PERF_HOTPATH_STATS
        ST_CHECK("PERF", "self_run_time", ts.run_time_us > 0U, "us=%u", (unsigned)ts.run_time_us);
#else
        ST_INFO("PERF", "self_run_time_off", "HOTPATH_STATS=0: run_time_us 不可用(零开销档)");
#endif
        ST_CHECK("PERF", "self_switches", ts.switch_count > 0U, "sw=%u", (unsigned)ts.switch_count); /* TCB 级计数, 不依赖热路径 */
        ST_CHECK("PERF", "stack_watermark", ts.stack_used_bytes > 0U && ts.stack_used_bytes < ts.stack_total_bytes,
                 "used=%u total=%u", (unsigned)ts.stack_used_bytes, (unsigned)ts.stack_total_bytes);
        ST_CHECK("PERF", "stack_free_pos", ts.stack_free_bytes > 0U, "free=%u", (unsigned)ts.stack_free_bytes);
    }

    st_expect("PERF", "all_null_array", rtos_perf_get_all_tasks(NULL, 4, &n), RTOS_ERR_NULL);
    st_expect("PERF", "all_max0", rtos_perf_get_all_tasks(arr, 0, &n), RTOS_ERR_PARAM);
    r = rtos_perf_get_all_tasks(arr, 4, &n);
    st_expect("PERF", "all_ok", r, RTOS_OK);
    /* 此刻存活任务: idle + timer service + selftest = 3 */
    ST_CHECK("PERF", "all_count_3", n == 3U, "n=%u", (unsigned)n);
    r = rtos_perf_get_all_tasks(arr, 2, &n);
    st_expect("PERF", "all_truncate_ok", r, RTOS_OK);
    ST_CHECK("PERF", "all_truncated_2", n == 2U, "n=%u", (unsigned)n);
    for (i = 0; i < n; i++) {
        ST_CHECK("PERF", "task_name_valid", arr[i].name[0] != '\0', "i=%u", (unsigned)i);
    }

    st_expect("PERF", "get_memory_null", rtos_perf_get_memory(NULL), RTOS_ERR_NULL);
    r = rtos_perf_get_memory(&mem);
    st_expect("PERF", "get_memory_ok", r, RTOS_OK);
    if (r == RTOS_OK) {
        ST_CHECK("PERF", "pool_registered", mem.registered_pools >= 1U, "pools=%u", (unsigned)mem.registered_pools);
        ST_CHECK("PERF", "task_pool_used", mem.task_pool_used >= 2U, "used=%u", (unsigned)mem.task_pool_used);
    }

    /* ISR 安全快照接口 */
    ST_CHECK("PERF", "cpu_usage_snapshot", rtos_perf_get_cpu_usage() <= 10000U,
             "cpu=%u", (unsigned)rtos_perf_get_cpu_usage());
    rtos_perf_reset();
    r = rtos_perf_get_system(&sys);
    st_expect("PERF", "reset_then_query", r, RTOS_OK);
}

/* ---------------- ISR 矩阵(任务侧) ---------------- */

static void test_isr_matrix(void)
{
    uint16_t item16 = 0x5A01; /* item_size=2 */
    uint8_t  buf[2];
    uint32_t wait;

    /* 布置 ISR 专用对象 */
    st_expect("ISR", "setup_q", rtos_queue_init(&st_isr_q, st_isr_q_storage, 2, 2), RTOS_OK);
    st_expect("ISR", "setup_q_put", rtos_queue_send(&st_isr_q, &item16, RTOS_NO_WAIT, RTOS_FALSE), RTOS_OK);
    st_expect("ISR", "setup_sem", rtos_sem_init(&st_isr_sem, 0, 2), RTOS_OK);
    st_expect("ISR", "setup_mutex", rtos_mutex_init(&st_isr_mutex), RTOS_OK);
    st_expect("ISR", "setup_ev", rtos_event_init(&st_isr_ev), RTOS_OK);
    st_expect("ISR", "setup_timer", rtos_timer_create(&st_isr_timer, "isr", st_isr_timer_cb, NULL, 150, RTOS_TIMER_ONE_SHOT), RTOS_OK);
    st_expect("ISR", "setup_victim", rtos_task_create(&st_isr_victim_tcb, 128, st_isr_victim_fn, NULL, 26, "isrv"), RTOS_OK);

    /* 布防并等待上位机回发 'X' */
    st_isr_done = 0;
    st_isr_armed = 1;
    printf("ST|ISR|ARM|send 'X' now\r\n");

    wait = 0;
    while ((st_isr_done == 0U) && (wait < 8000U)) {
        rtos_task_delay(10);
        wait += 10;
    }
    ST_CHECK("ISR", "trigger_received", st_isr_done == 1U, "waited=%ums", (unsigned)wait);
    if (st_isr_done == 0U) {
        st_isr_armed = 0;
        return;
    }

    /* 错误码矩阵断言 */
    st_expect("ISR", "q_recv_nowait_ok", (rtos_status_t)st_isr_r[ISR_Q_RECV_NOWAIT], RTOS_OK);
    ST_CHECK("ISR", "q_recv_data", st_isr_recv_val == 0x01U && (st_isr_r[ISR_Q_RECV_NOWAIT] == RTOS_OK),
             "v=%02x r=%d", (unsigned)st_isr_recv_val, (int)st_isr_r[ISR_Q_RECV_NOWAIT]);
    st_expect("ISR", "q_recv_tmo_isr_err", (rtos_status_t)st_isr_r[ISR_Q_RECV_TMO], RTOS_ERR_ISR);
    st_expect("ISR", "q_send_nowait_ok", (rtos_status_t)st_isr_r[ISR_Q_SEND_NOWAIT], RTOS_OK);
    st_expect("ISR", "q_send_tmo_isr_err", (rtos_status_t)st_isr_r[ISR_Q_SEND_TMO], RTOS_ERR_ISR);
    st_expect("ISR", "s_take_nowait_timeout", (rtos_status_t)st_isr_r[ISR_S_TAKE_NOWAIT], RTOS_ERR_TIMEOUT);
    st_expect("ISR", "s_take_tmo_isr_err", (rtos_status_t)st_isr_r[ISR_S_TAKE_TMO], RTOS_ERR_ISR);
    st_expect("ISR", "s_give_ok", (rtos_status_t)st_isr_r[ISR_S_GIVE], RTOS_OK);
    st_expect("ISR", "m_take_isr_err", (rtos_status_t)st_isr_r[ISR_M_TAKE], RTOS_ERR_ISR);
    st_expect("ISR", "m_give_isr_err", (rtos_status_t)st_isr_r[ISR_M_GIVE], RTOS_ERR_ISR);
    st_expect("ISR", "task_del_isr_err", (rtos_status_t)st_isr_r[ISR_TASK_DEL], RTOS_ERR_ISR);
    st_expect("ISR", "notify_wait_isr_err", (rtos_status_t)st_isr_r[ISR_NOTIFY_WAIT], RTOS_ERR_ISR);
    st_expect("ISR", "timer_start_ok", (rtos_status_t)st_isr_r[ISR_T_START], RTOS_OK);
    st_expect("ISR", "notify_ok", (rtos_status_t)st_isr_r[ISR_NOTIFY], RTOS_OK);
    ST_CHECK("ISR", "victim_notified", st_isr_victim_notified == 0xABU, "v=%x", (unsigned)st_isr_victim_notified);
    st_expect("ISR", "suspend_other_ok", (rtos_status_t)st_isr_r[ISR_SUSPEND], RTOS_OK);
    st_expect("ISR", "resume_other_ok", (rtos_status_t)st_isr_r[ISR_RESUME], RTOS_OK);
    ST_CHECK("ISR", "delay_is_noop", st_isr_delay_ok == 1U, "");
    ST_CHECK("ISR", "event_wait_returns0", st_isr_ev_wait_ret == 0U, "ret=%x", (unsigned)st_isr_ev_wait_ret);
    ST_CHECK("ISR", "event_set_bits", (st_isr_ev_set_ret & 0x02U) != 0U, "ret=%x", (unsigned)st_isr_ev_set_ret);
    ST_CHECK("ISR", "perf_cpu_snapshot", st_isr_perf_cpu <= 10000U, "cpu=%u", (unsigned)st_isr_perf_cpu);

    /* ISR 中启动的定时器稍后触发 */
    rtos_task_delay(300);
    ST_CHECK("ISR", "timer_fired", st_isr_timer_fired == 1U, "");

    /* ISR 中 give 的信号量可被任务取走 */
    st_expect("ISR", "take_isr_given", rtos_sem_take(&st_isr_sem, RTOS_NO_WAIT), RTOS_OK);

    /* ISR 中 send 的消息可被任务收到 */
    st_expect("ISR", "recv_isr_sent", rtos_queue_recv(&st_isr_q, buf, RTOS_NO_WAIT), RTOS_OK);
    ST_CHECK("ISR", "isr_sent_data", buf[0] == 0xEEU, "b=%02x", (unsigned)buf[0]);

    rtos_task_delete(st_isr_victim_tcb);
}

/* ---------------- 栈溢出检测 ---------------- */

static rtos_tcb_t    ovf_tcb;
static rtos_stack_t  ovf_stack[128];

static void st_ovf_fn(void *arg)
{
    rtos_tcb_t *self = rtos_task_get_current();
    (void)arg;

    /* 等待 idle 扫描基线确认(无溢出时不触发 hook) */
    rtos_task_delay(300);
    *(volatile uint32_t *)&self->stack_magic = 0xDEADBEE0U; /* 破坏魔数 */
    /* 破坏窗口必须 > idle 栈扫描周期: idle 每 256 圈扫描, 默认 idle_hook
     * 为 wfi(每 SysTick 醒一次) → 最坏检测延迟 ≈ 256ms, 窗口取 700ms */
    rtos_task_delay(700);

    *(volatile uint32_t *)&self->stack_magic = 0xDEADBEEFU;  /* 修复 */
    rtos_task_delete(NULL);
}

static void test_stack_overflow(void)
{
    rtos_status_t r;

    st_ovf_flag = 0;
    st_ovf_tcb = NULL;
    r = rtos_task_create_static(&ovf_tcb, ovf_stack, 128, st_ovf_fn, NULL, 25, "ovf");
    st_expect("OVF", "create_static", r, RTOS_OK);

    rtos_task_delay(1400);
    ST_CHECK("OVF", "hook_invoked", st_ovf_flag == 1U, "");
    ST_CHECK("OVF", "hook_task_id", st_ovf_tcb == &ovf_tcb, "");
}

/* ---------------- 时间片轮转 ---------------- */

static volatile uint32_t ts_cnt_a, ts_cnt_b;
static void st_ts_fn(void *arg)
{
    uint32_t n;
    for (n = 0; n < 100U; n++) {
        (*(volatile uint32_t *)arg)++;
        rtos_task_delay(0); /* yield: 同优先级交替 */
    }
    for (;;) { rtos_task_delay(100); }
}

static void test_time_slice(void)
{
    rtos_tcb_t *ta, *tb;

    ts_cnt_a = 0;
    ts_cnt_b = 0;
    st_expect("SLICE", "create_a", rtos_task_create(&ta, 128, st_ts_fn, (void *)&ts_cnt_a, 10, "ts_a"), RTOS_OK);
    st_expect("SLICE", "create_b", rtos_task_create(&tb, 128, st_ts_fn, (void *)&ts_cnt_b, 10, "ts_b"), RTOS_OK);
    rtos_task_delay(400);
    ST_CHECK("SLICE", "both_ran", ts_cnt_a >= 100U && ts_cnt_b >= 100U,
             "a=%u b=%u", (unsigned)ts_cnt_a, (unsigned)ts_cnt_b);
    ST_INFO("SLICE", "counts", "a=%u b=%u (alternating)", (unsigned)ts_cnt_a, (unsigned)ts_cnt_b);
    rtos_task_delete(ta);
    rtos_task_delete(tb);
}

/* ==================================================================== */
/*                     修复验证用例 (对应 rtos_improvement_roadmap)      */
/* ==================================================================== */

/* ---- FIX-1: tick 回绕安全 ---- */
static rtos_tcb_t *tk_tcb;
static volatile uint32_t tk_fell_asleep_at, tk_woke_at, tk_never_flag;
static volatile rtos_tick_t tk_wt_fire_tick;

static void tk_wrap_timer_cb(void *arg)
{
    (void)arg;
    tk_wt_fire_tick = rtos_sched_get_tick_count();
}

static void tk_big_delay_fn(void *arg)
{
    (void)arg;
    tk_fell_asleep_at = rtos_sched_get_tick_count();
    rtos_task_delay(0xFFFFFF00U); /* [fix] 钳制为永久, 不再 1ms 内误醒 */
    tk_woke_at = rtos_sched_get_tick_count();
    tk_never_flag = 1U; /* 不应执行到这里(永久阻塞) */
    for (;;) { rtos_task_delay(1000); }
}

static void test_fix_tick_wrap(void)
{
    rtos_task_create(&tk_tcb, 128, tk_big_delay_fn, NULL, 12, "fx_tk");
    rtos_task_delay(80); /* 修复前: victim 会在 ~1ms 内醒来并置 flag */
    ST_CHECK("FIX1", "big_delay_not_early_wake", tk_never_flag == 0U, "");
    rtos_task_delete(tk_tcb);

    /* 回绕点仿真: 直接把系统 tick 推到回绕边界前, delay(200) 应精确 200ms 醒来 */
    {
        rtos_tick_t t0, dt;
        rtos_kernel.tick_count = 0xFFFFFF00U; /* 仿真 49.7 天运行点 */
        t0 = rtos_sched_get_tick_count();
        rtos_task_delay(200);
        dt = rtos_sched_get_tick_count() - t0;
        ST_CHECK("FIX1", "wrap_delay200", (dt >= 198U) && (dt <= 206U), "dt=%u", (unsigned)dt);
        /* 回绕点附近的定时器 */
        {
            static rtos_timer_t wt;
            tk_wt_fire_tick = 0;
            st_expect("FIX1", "wrap_timer_create",
                      rtos_timer_create(&wt, "fx_w", tk_wrap_timer_cb, NULL, 100, RTOS_TIMER_ONE_SHOT), RTOS_OK);
            t0 = rtos_sched_get_tick_count();
            rtos_timer_start(&wt);
            rtos_task_delay(160);
            ST_CHECK("FIX1", "wrap_timer_fired_ontime",
                     (tk_wt_fire_tick != 0U) && ((tk_wt_fire_tick - t0) >= 95U) && ((tk_wt_fire_tick - t0) <= 130U),
                     "dt=%u", (unsigned)(tk_wt_fire_tick - t0));
            rtos_timer_stop(&wt);
        }
        /* tick 移出回绕区, 恢复正常量程 */
        rtos_kernel.tick_count = 100000U;
    }

    /* 定时器周期上限: >= 2^31 拒绝 */
    {
        rtos_timer_t bad;
        st_expect("FIX1", "timer_period_max", rtos_timer_create(&bad, "x", tk_wrap_timer_cb, NULL, 0x80000000U,
                      RTOS_TIMER_PERIODIC), RTOS_ERR_PARAM);
    }
}

/* ---- FIX-2: resume TOCTOU 并发压力 ---- */
static rtos_tcb_t *rm_victim_tcb;
static volatile uint32_t rm_wake_count, rm_rounds;
static volatile uint8_t rm_isr_go;

static void rm_victim_fn(void *arg)
{
    (void)arg;
    for (;;) {
        rm_wake_count++;
        rtos_task_suspend(NULL); /* 每轮被唤醒后立刻再挂起 */
    }
}

static void rm_isr_fn(void *arg)
{
    /* 并发源: 高于主任务(9)的优先级 5, 每毫秒醒来 resume 同一 victim,
     * 与主任务的 resume 竞争"检查-入环"窗口(修复前检查在临界区外)。 */
    (void)arg;
    for (;;) {
        rtos_task_delay(1);
        if (rm_isr_go != 0U) {
            rtos_task_resume(rm_victim_tcb);
        }
    }
}

static void test_fix_resume_race(void)
{
    rtos_tcb_t *isr;
    uint32_t i;
    rtos_perf_task_t arr[4];
    uint32_t n = 0;

    rm_wake_count = 0;
    rm_rounds = 0;
    rm_isr_go = 0;
    rtos_task_create(&rm_victim_tcb, 256, rm_victim_fn, NULL, 5, "fx_rv");  /* 高于主任务: 被唤醒即运行 */
    rtos_task_create(&isr, 256, rm_isr_fn, NULL, 6, "fx_rh"); /* 高于主任务, 低于 victim */

    rtos_task_delay(30); /* victim 现已 SUSPENDED */
    for (i = 0; i < 3000U; i++) {
        rm_isr_go = 1U;
        rtos_task_resume(rm_victim_tcb); /* 与高优先级任务竞争同一窗口 */
        rm_isr_go = 0U;
        rm_rounds++;
    }
    rtos_task_delay(50);
    /* 存活性判据: 链表未坏 → 任务仍在注册表且可调度; 唤醒计数与轮次同量级 */
    rtos_perf_get_all_tasks(arr, 4, &n);
    ST_CHECK("FIX2", "resume_race_alive", (rm_wake_count >= rm_rounds / 2U) && (n >= 4U),
             "wake=%u rounds=%u n=%u", (unsigned)rm_wake_count, (unsigned)rm_rounds, (unsigned)n);
    rtos_task_delete(rm_victim_tcb);
    rtos_task_delete(isr);
}

/* ---- FIX-3: event deinit 后 UAF (栈上对象) ---- */
static volatile uint8_t ev2_done;

static void ev2_waiter_fn(void *arg)
{
    rtos_event_t *ev = (rtos_event_t *)arg; /* 指向主任务栈上的对象! */
    ev2_done = 0;
    (void)rtos_event_wait(ev, 0x01U, RTOS_EVENT_WAIT_ANY, RTOS_WAIT_FOREVER);
    ev2_done = 1U; /* 醒来后 delay 拉开与 deinit 的时序, 确保读到的是"修复后不回读" */
    rtos_task_delay(50);
    for (;;) { rtos_task_delay(1000); }
}

static void test_fix_event_uaf(void)
{
    rtos_tcb_t *vt;
    rtos_event_t ev_on_stack; /* 栈上对象: deinit 返回后即"销毁" */

    rtos_event_init(&ev_on_stack);
    rtos_task_create(&vt, 256, ev2_waiter_fn, (void *)&ev_on_stack, 12, "fx_ev");
    rtos_task_delay(30);
    rtos_event_deinit(&ev_on_stack); /* 等待者被置 DELETED; 此后本栈帧继续被复用 */
    rtos_task_delay(120); /* 等待者运行: 修复前会回读已"销毁"的栈内存 */
    ST_CHECK("FIX3", "event_uaf_survive", ev2_done == 1U, "done=%u", (unsigned)ev2_done);
    rtos_task_delete(vt);
}

/* ---- FIX-4: FPU 任务 40 字最小栈 ---- */
static volatile uint8_t fpu_ok;

static void fpu_small_fn(void *arg)
{
    volatile float x = 1.5f; /* 触发 FPU 帧建立 */
    (void)arg;
    x = x * 3.3f + 2.7f;
    rtos_task_delay(30); /* 阻塞 → PendSV 条件压 S16-S31 */
    if (x > 5.0f) { fpu_ok = 1U; }
    for (;;) { rtos_task_delay(1000); }
}

static void test_fix_min_stack_fpu(void)
{
    rtos_tcb_t *t;

    fpu_ok = 0;
    st_ovf_flag = 0; /* 清除前序 OVF 用例遗留(该用例只置不清) */
    st_expect("FIX4", "fpu_stack95_rejected", rtos_task_create(&t, 95, fpu_small_fn, NULL, 12, "fx_f"), RTOS_ERR_PARAM);
    st_expect("FIX4", "fpu_stack96_ok", rtos_task_create(&t, 96, fpu_small_fn, NULL, 12, "fx_f"), RTOS_OK);
    rtos_task_delay(100);
    ST_CHECK("FIX4", "fpu_small_stack_survive", fpu_ok == 1U, "");
    ST_CHECK("FIX4", "no_overflow_hook", st_ovf_flag == 0U, ""); /* 96 字栈不应触发溢出钩子 */
    rtos_task_delete(t);
}

/* ---- FIX-5: heap 输入域 ---- */
static void test_fix_heap_domain(void)
{
    ST_CHECK("FIX5", "alloc_wrap_null", rtos_heap_alloc(0xFFFFFFF8U) == NULL, "");
    ST_CHECK("FIX5", "alloc_huge_null", rtos_heap_alloc(0x7FFFFFFFU) == NULL, "");
}

/* ---- FIX-6: to_front 阻塞代办 ---- */
static void fx_tofront_sender_fn(void *arg)
{
    victim_ctx_t *c = (victim_ctx_t *)arg;
    /* 以 to_front=TRUE 阻塞: 紧急消息应插到队首, 而非代办到队尾 */
    c->result = rtos_queue_send(c->q, (const void *)&c->data, c->timeout, RTOS_TRUE);
    c->done = 1;
    for (;;) { rtos_task_delay(100); }
}

static void test_fix_tofront_defer(void)
{
    rtos_queue_t q;
    static uint8_t storage[2 * 4];
    uint32_t v;
    rtos_tcb_t *vt;

    rtos_queue_init(&q, storage, 4, 2);
    v = 1; rtos_queue_send(&q, &v, RTOS_NO_WAIT, RTOS_FALSE);
    v = 2; rtos_queue_send(&q, &v, RTOS_NO_WAIT, RTOS_FALSE); /* 满 */
    memset((void *)&vc, 0, sizeof(vc));
    vc.q = &q;
    vc.data = 0x99U; /* 紧急消息 */
    vc.timeout = RTOS_WAIT_FOREVER;
    /* sender 用高优先级(5): 创建即抢占主任务完成阻塞, 时序 100% 确定 */
    rtos_task_create(&vt, 256, fx_tofront_sender_fn, &vc, 5, "fx_tf");
    v = 0;
    rtos_queue_recv(&q, &v, RTOS_NO_WAIT); /* 腾 1 格 → 代办 sender 消息(应插队首) */
    rtos_task_delay(50);
    ST_CHECK("FIX6", "sender_woken", vc.done == 1U, "");
    {
        uint32_t got = 0;
        rtos_queue_recv(&q, &got, RTOS_NO_WAIT);
        ST_CHECK("FIX6", "tofront_defer_first", got == 0x99U, "got=%x", (unsigned)got); /* 紧急消息先于旧消息2 */
    }
    rtos_task_delete(vt);
}

/* ---- FIX-7: heap 合并统计不下溢 ---- */
static void test_fix_heap_merge_stats(void)
{
    uint32_t i;
    for (i = 0; i < 500U; i++) {
        void *p = rtos_heap_alloc(64);
        void *q2 = rtos_heap_alloc(64);
        void *r;
        if ((p == NULL) || (q2 == NULL)) { rtos_heap_free(p); rtos_heap_free(q2); break; }
        rtos_heap_free(q2); /* 先 free 后块 */
        rtos_heap_free(p);  /* 再 free 前块 → 前后相邻空闲合并(next 的 header 入账) */
        r = rtos_heap_alloc(120); /* 整块重新分配 */
        rtos_heap_free(r);
    }
    ST_CHECK("FIX7", "heap_stats_no_underflow", rtos_heap_get_free() <= RTOS_CONFIG_HEAP_SIZE,
             "free=%u", (unsigned)rtos_heap_get_free());
}

/* ---- FIX-RACE3: 双重删除 ---- */
static rtos_tcb_t   dd_tcb;    /* 静态 TCB: 自删后不被回收, 二次 delete 读到确定状态 */
static rtos_stack_t dd_stack[96];

static void test_fix_double_delete(void)
{
    static volatile uint32_t dead2;
    dead2 = 0;
    /* 静态任务自删: state=DELETED 且 TCB/栈不被回收 → 二次 delete 走防护分支 */
    st_expect("FIXR3", "dd_create", rtos_task_create_static(&dd_tcb, dd_stack, 96,
              st_suicide_fn, (void *)&dead2, 12, "fx_dd"), RTOS_OK);
    rtos_task_delay(80); /* 已自删: state=DELETED, 静态 TCB 内存仍在 */
    st_expect("FIXR3", "double_delete_rejected", rtos_task_delete(&dd_tcb), RTOS_ERR_PARAM);
    /* 拒删 idle */
    st_expect("FIXR3", "delete_idle_rejected", rtos_task_delete(rtos_kernel.idle_tcb), RTOS_ERR_PARAM);
}

/* ---- FIX-PI1: 多锁优先级继承 ---- */
static rtos_mutex_t pi_l1, pi_l2;
static rtos_tcb_t *pi2_low_tcb;
static volatile uint8_t pi2_h1_got, pi2_h2_got;

static void pi2_low_fn(void *arg)
{
    uint32_t v;
    (void)arg;
    rtos_mutex_take(&pi_l1, RTOS_WAIT_FOREVER);
    rtos_mutex_take(&pi_l2, RTOS_WAIT_FOREVER);
    for (;;) {
        rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v); /* 等主任务: 阶段1 检查后 give L1 */
        rtos_mutex_give(&pi_l1);
        rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v); /* 等主任务: 阶段2 检查后 give L2 */
        rtos_mutex_give(&pi_l2);
        rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v); /* 等主任务收尾 */
        break;
    }
    for (;;) { rtos_task_delay(1000); }
}

static void pi2_h1_fn(void *arg)
{
    (void)arg;
    rtos_mutex_take(&pi_l1, RTOS_WAIT_FOREVER);
    pi2_h1_got = 1U;
    rtos_mutex_give(&pi_l1);
    for (;;) { rtos_task_delay(1000); }
}

static void pi2_h2_fn(void *arg)
{
    (void)arg;
    rtos_mutex_take(&pi_l2, RTOS_WAIT_FOREVER);
    pi2_h2_got = 1U;
    rtos_mutex_give(&pi_l2);
    for (;;) { rtos_task_delay(1000); }
}

static void test_fix_multilock_inherit(void)
{
    rtos_tcb_t *h1, *h2;

    rtos_mutex_init(&pi_l1);
    rtos_mutex_init(&pi_l2);
    pi2_h1_got = 0;
    pi2_h2_got = 0;
    rtos_task_create(&pi2_low_tcb, 256, pi2_low_fn, NULL, 20, "fx_pl");
    rtos_task_delay(30); /* low(base 20) 持两锁 */
    rtos_task_create(&h1, 256, pi2_h1_fn, NULL, 4, "fx_p1");
    rtos_task_create(&h2, 256, pi2_h2_fn, NULL, 6, "fx_p2");
    rtos_task_delay(50); /* H1(4) 等 L1, H2(6) 等 L2 → low 应被提升到 4 */

    ST_CHECK("FIXP1", "multilock_boost", rtos_task_get_priority(pi2_low_tcb) == 4U,
             "prio=%u want=4", (unsigned)rtos_task_get_priority(pi2_low_tcb));

    rtos_task_notify(pi2_low_tcb, 1U, RTOS_NOTIFY_VALUE); /* 触发 give L1 */
    rtos_task_delay(80);
    /* [fix PI-1] 核心: 释放 L1 后应保持 L2 上 H2(6) 的提升, 而非回落 base 20 */
    ST_CHECK("FIXP1", "keep_l2_boost_after_give_l1", rtos_task_get_priority(pi2_low_tcb) == 6U,
             "prio=%u want=6", (unsigned)rtos_task_get_priority(pi2_low_tcb));
    ST_CHECK("FIXP1", "h1_got_lock", pi2_h1_got == 1U, "");

    rtos_task_notify(pi2_low_tcb, 1U, RTOS_NOTIFY_VALUE); /* 触发 give L2 */
    rtos_task_delay(80);
    ST_CHECK("FIXP1", "restore_base_after_all", rtos_task_get_priority(pi2_low_tcb) == 20U,
             "prio=%u want=20", (unsigned)rtos_task_get_priority(pi2_low_tcb));
    ST_CHECK("FIXP1", "h2_got_lock", pi2_h2_got == 1U, "");

    rtos_task_notify(pi2_low_tcb, 1U, RTOS_NOTIFY_VALUE);
    rtos_task_delay(30);
    rtos_task_delete(pi2_low_tcb);
    rtos_task_delete(h1);
    rtos_task_delete(h2);
}

/* ---- FIX-PI2: 等待链重排 ---- */
static rtos_sem_t rq_sem;
static volatile uint8_t rq_order[2];
static volatile uint8_t rq_n;

static void rq_w_fn(void *arg)
{
    uint8_t id = (uint8_t)(uintptr_t)arg;
    (void)arg;
    rtos_sem_take(&rq_sem, RTOS_WAIT_FOREVER);
    rq_order[rq_n] = id;
    rq_n++;
    for (;;) { rtos_task_delay(1000); }
}

static void test_fix_waitlist_requeue(void)
{
    rtos_tcb_t *tw, *tx;

    rtos_sem_init(&rq_sem, 0, 4);
    rq_n = 0;
    rtos_task_create(&tw, 128, rq_w_fn, (void *)(uintptr_t)1, 12, "fx_w"); /* W 先阻塞(prio 12) */
    rtos_task_delay(20);
    rtos_task_create(&tx, 128, rq_w_fn, (void *)(uintptr_t)2, 14, "fx_x"); /* X 后阻塞(prio 14) */
    rtos_task_delay(20);
    /* 把 W 降到 20(低于 X): 修复前等待链仍 [W,X] → give 唤醒 W(错); 修复后重排 [X,W] */
    rtos_task_set_priority(tw, 20);
    rtos_sem_give(&rq_sem);
    rtos_task_delay(50);
    rtos_sem_give(&rq_sem);
    rtos_task_delay(50);
    ST_CHECK("FIXP2", "waitlist_requeued", (rq_n == 2U) && (rq_order[0] == 2U) && (rq_order[1] == 1U),
             "n=%u o0=%u o1=%u", (unsigned)rq_n, (unsigned)rq_order[0], (unsigned)rq_order[1]);
    rtos_task_delete(tw);
    rtos_task_delete(tx);
}

/* ---- FIX-G1: mutex 注册表满 ---- */
static void test_fix_mutex_registry_full(void)
{
    static rtos_mutex_t pool[RTOS_CONFIG_MAX_MUTEXES];
    static rtos_mutex_t probe;
    uint32_t i, ok = 0;

    /* 注册表是全局共享的(前序用例的 rec_m/pi_m 等可能仍占位),
     * 逐把填充到满: 满后 init 必须显式报 ERR_NO_MEM 而非静默放弃注册。 */
    for (i = 0; i < RTOS_CONFIG_MAX_MUTEXES; i++) {
        if (rtos_mutex_init(&pool[i]) != RTOS_OK) {
            break;
        }
        ok++;
    }
    /* 填满(或本就满)后再 init 必须报错 */
    st_expect("FIXG1", "registry_full_error", rtos_mutex_init(&probe), RTOS_ERR_NO_MEM);
    /* deinit 全部成功者, 恢复注册表容量 */
    for (i = 0; i < ok; i++) {
        rtos_mutex_deinit(&pool[i]);
    }
    /* 回收后可重新注册 */
    st_expect("FIXG1", "registry_reusable", rtos_mutex_init(&probe), RTOS_OK);
    rtos_mutex_deinit(&probe);
}

/* ---- FIX-G6: timer 回调内自调用(超队列深度, 不自阻塞) ---- */
static rtos_timer_t gs_t;
static volatile uint32_t gs_restarts;

static void gs_cb(void *arg)
{
    uint32_t i;
    (void)arg;
    /* 回调上下文 = 服务任务自身: 连续 20 次 start(命令队列深度仅 16)。
     * 修复前: 第 17 次起 timer_send_cmd 以任务态阻塞 100ms 等自己 drain →
     * 自死锁 100ms×N; 修复后: 就地执行, 全部立即返回。 */
    for (i = 0; i < 20U; i++) {
        if (rtos_timer_start(&gs_t) != RTOS_OK) {
            return; /* 记录失败但不死锁 */
        }
        gs_restarts++;
    }
}

static void test_fix_timer_selfcall(void)
{
    rtos_tick_t t0, dt;

    gs_restarts = 0;
    rtos_timer_create(&gs_t, "fx_gs", gs_cb, NULL, 100, RTOS_TIMER_ONE_SHOT);
    t0 = rtos_sched_get_tick_count();
    rtos_timer_start(&gs_t);
    rtos_task_delay(150); /* 回调执行: 20 次 start 就地完成, 且 one-shot 被
                             自己重启 → 需再等 100ms; 只验证"不死锁" */
    dt = rtos_sched_get_tick_count() - t0;
    rtos_timer_stop(&gs_t);
    ST_CHECK("FIXG6", "no_self_deadlock", gs_restarts == 20U, "r=%u dt=%u",
             (unsigned)gs_restarts, (unsigned)dt);
}

static void test_fixes(void)
{
    test_fix_tick_wrap();          /* FIX1: BUG-1 tick 回绕 + 定时器 */
    test_fix_resume_race();       /* FIX2: BUG-2 resume TOCTOU */
    test_fix_event_uaf();          /* FIX3: BUG-3 event UAF */
    test_fix_min_stack_fpu();      /* FIX4: BUG-4 最小栈/FPU */
    test_fix_heap_domain();        /* FIX5: BUG-5 heap 输入域 */
    test_fix_tofront_defer();      /* FIX6: BUG-6 to_front 代办 */
    test_fix_heap_merge_stats();   /* FIX7: BUG-7 heap 统计 */
    test_fix_double_delete();     /* FIX8: RACE-3 防重入/拒删 idle */
    test_fix_multilock_inherit();  /* FIX9: PI-1 多锁继承 */
    test_fix_waitlist_requeue();  /* FIX10: PI-2 等待链重排 */
    test_fix_mutex_registry_full(); /* FIX11: G-1 注册表 */
    test_fix_timer_selfcall();    /* FIX12: G-6 自调用 */
}



void selftest_task(void *arg)
{
    (void)arg;

    printf("ST|META|phase2|INFO|scheduler running, full module tests\r\n");

    /* 立即删除启动前创建的极限任务(prio31/栈20字), 不给任何运行机会:
     * 一旦它被调度(哪怕只跑一次 delay 调用链), 80B 栈就会溢出并写坏堆
     * 元数据(实测故障链: 溢出 → heap header 损坏 → 后续 alloc 全部 NO_MEM)。 */
    st_expect("TASK", "p31_stack20_delete", rtos_task_delete(st_p31_tcb), RTOS_OK);
    ST_CHECK("TASK", "p31_never_ran", st_p31_flag == 0U, "");

    test_sched();
    test_task();
    test_queue();
    test_sem();
    test_sem_priority_wake();
    test_mutex();
    test_event();
    test_timer();
    test_mempool();
    test_heap();
    test_perf();
    test_isr_matrix();
    test_stack_overflow();
    test_time_slice();
    test_fixes(); /* 修复验证(对应 improvement roadmap 第 1~3 批) */

    printf("ST|SUMMARY|pass=%u fail=%u info=%u total=%u|END\r\n",
           (unsigned)st_pass, (unsigned)st_fail, (unsigned)st_info, (unsigned)st_total);
    printf("ST|DONE|selftest complete\r\n");

    for (;;) {
        rtos_task_delay(1000);
    }
}

/* ==================================================================== */
/*                      返回即退出 / 自删辅助任务                         */
/* ==================================================================== */

void st_returning_fn(void *arg)
{
    *(volatile uint32_t *)arg = 1U;
    /* 正常返回: 内核 exit handler 应兜底自删除 */
}

void st_suicide_fn(void *arg)
{
    *(volatile uint32_t *)arg = 1U;
    rtos_task_delete(NULL); /* 自删, 不返回 */
    for (;;) { }
}

void st_boost_fn(void *arg)
{
    uint32_t v;
    (void)arg;
    rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v);
    *(volatile uint32_t *)arg = 1U;
    for (;;) { rtos_task_delay(100); }
}
