/* Target-side regressions for the 2026-10-12 memory/timer audit. */
#include "audit_memory_timer.h"
#include "rtos.h"
#include "kern_port.h"
#include <stdio.h>
#include <string.h>

static uint32_t s_pass;
static uint32_t s_fail;

static void check(const char *name, int ok, int32_t actual, int32_t expected)
{
    if (ok) {
        s_pass++;
    } else {
        s_fail++;
    }
    printf("AT|MEMTIMER|%s|%s|actual=%ld|expect=%ld\r\n", name,
           ok ? "PASS" : "FAIL", (long)actual, (long)expected);
}

static rtos_mempool_t s_pool;
static uint32_t s_pool_storage[16];

static void memory_cases(void)
{
    rtos_status_t st;
    void *a;
    void *b;

    /* The original implementation wraps to a tiny block, but writes only the
     * first next pointer for count=1. This reproducer stays inside the buffer. */
    st = rtos_mempool_init(&s_pool, s_pool_storage, UINT32_MAX, 1U);
    check("pool_align_overflow", st == RTOS_ERR_PARAM, st, RTOS_ERR_PARAM);

    st = rtos_mempool_init(&s_pool, s_pool_storage, 16U, 4U);
    check("pool_valid_init", st == RTOS_OK, st, RTOS_OK);
    a = rtos_mempool_alloc(&s_pool);
    st = rtos_mempool_free(&s_pool, (uint8_t *)a + 1U);
    check("pool_interior_free", st == RTOS_ERR_PARAM, st, RTOS_ERR_PARAM);
    /* The invalid free corrupts the old list. Reinitialise the isolated pool
     * immediately, without following that list. */
    (void)rtos_mempool_init(&s_pool, s_pool_storage, 16U, 4U);
    a = rtos_mempool_alloc(&s_pool);
    (void)rtos_mempool_free(&s_pool, a);
    st = rtos_mempool_free(&s_pool, a);
    check("pool_double_free_rejected", st != RTOS_OK, st, RTOS_ERR_ALREADY);
    check("pool_free_count_bounded", s_pool.free_count == 4U,
          (int32_t)s_pool.free_count, 4);
    a = rtos_mempool_alloc(&s_pool);
    b = rtos_mempool_alloc(&s_pool);
    check("pool_allocations_distinct", a != NULL && b != NULL && a != b,
          a == b, 0);
    (void)rtos_mempool_init(&s_pool, s_pool_storage, 16U, 4U);
    a = rtos_mempool_alloc(&s_pool);
    b = rtos_mempool_alloc(&s_pool);
    (void)rtos_mempool_free(&s_pool, a);
    st = rtos_mempool_free(&s_pool, a);
    check("pool_double_free_with_other_allocated", st == RTOS_ERR_ALREADY,
          st, RTOS_ERR_ALREADY);
    (void)b;
    (void)rtos_mempool_init(&s_pool, s_pool_storage, 16U, 4U);
}

static rtos_timer_t s_same_a;
static rtos_timer_t s_same_b;
static rtos_timer_t s_future;
static volatile uint32_t s_a_count;
static volatile uint32_t s_b_count;
static volatile uint32_t s_future_count;
static volatile rtos_tick_t s_future_tick;
static volatile rtos_status_t s_restart_status;

static void same_a_cb(void *arg)
{
    (void)arg;
    s_a_count++;
}

static void same_b_cb(void *arg)
{
    (void)arg;
    s_b_count++;
    if (s_b_count == 1U) {
        s_restart_status = rtos_timer_start(&s_same_b);
    }
}

static void future_cb(void *arg)
{
    (void)arg;
    s_future_count++;
    s_future_tick = rtos_sched_get_tick_count();
}

static void callback_restart_cases(void)
{
    rtos_tick_t begin;
    s_a_count = 0U;
    s_b_count = 0U;
    s_future_count = 0U;
    s_future_tick = 0U;
    s_restart_status = RTOS_ERR_PARAM;
    (void)rtos_timer_create(&s_same_a, "audit_a", same_a_cb, NULL, 40U, RTOS_TIMER_ONE_SHOT);
    (void)rtos_timer_create(&s_same_b, "audit_b", same_b_cb, NULL, 40U, RTOS_TIMER_ONE_SHOT);
    (void)rtos_timer_create(&s_future, "audit_c", future_cb, NULL, 100U, RTOS_TIMER_ONE_SHOT);
    /* Keep all commands together so A and B receive the same expiry tick. */
    RTOS_PORT_ENTER_CRITICAL();
    begin = rtos_sched_get_tick_count();
    (void)rtos_timer_start(&s_same_a);
    (void)rtos_timer_start(&s_same_b);
    (void)rtos_timer_start(&s_future);
    RTOS_PORT_EXIT_CRITICAL();
    rtos_task_delay(150U);
    check("callback_restart_status", s_restart_status == RTOS_OK, s_restart_status, RTOS_OK);
    check("expired_peer_not_lost", s_a_count == 1U, (int32_t)s_a_count, 1);
    check("oneshot_self_restart", s_b_count == 2U, (int32_t)s_b_count, 2);
    check("future_callback_not_early", s_future_count == 1U &&
          (s_future_tick - begin) >= 100U,
          (int32_t)(s_future_tick - begin), 100);
    (void)rtos_timer_delete(&s_same_a);
    (void)rtos_timer_delete(&s_same_b);
    (void)rtos_timer_delete(&s_future);
    rtos_task_delay(5U);
}

static rtos_timer_t s_reuse_timer;
static volatile uint32_t s_reuse_count;

static void reuse_cb(void *arg)
{
    (void)arg;
    s_reuse_count++;
}

static void lifetime_cases(void)
{
    rtos_status_t st;
    s_reuse_count = 0U;
    st = rtos_timer_create(&s_reuse_timer, "audit_bad_mode", reuse_cb, NULL, 20U,
                           (rtos_timer_mode_t)2);
    check("timer_invalid_mode", st == RTOS_ERR_PARAM, st, RTOS_ERR_PARAM);

    RTOS_PORT_ENTER_CRITICAL();
    (void)rtos_timer_create(&s_reuse_timer, "audit_old", reuse_cb, NULL, 20U,
                            RTOS_TIMER_ONE_SHOT);
    (void)rtos_timer_start(&s_reuse_timer);
    (void)rtos_timer_delete(&s_reuse_timer);
    st = rtos_timer_create(&s_reuse_timer, "audit_new", reuse_cb, NULL, 30U,
                           RTOS_TIMER_ONE_SHOT);
    (void)rtos_timer_start(&s_reuse_timer);
    RTOS_PORT_EXIT_CRITICAL();
    rtos_task_delay(70U);
    check("timer_recreate_inactive", st == RTOS_OK, st, RTOS_OK);
    check("timer_old_delete_does_not_cancel_new", s_reuse_count == 1U,
          (int32_t)s_reuse_count, 1);
    (void)rtos_timer_delete(&s_reuse_timer);
    rtos_task_delay(5U);
}

static rtos_timer_t s_phase_timer;
static volatile uint32_t s_phase_count;
static volatile rtos_tick_t s_phase_ticks[6];

static void phase_cb(void *arg)
{
    (void)arg;
    if (s_phase_count < 6U) {
        s_phase_ticks[s_phase_count] = rtos_sched_get_tick_count();
    }
    s_phase_count++;
    /* Simulate a bounded callback workload using real target ticks. */
    rtos_task_delay(7U);
    if (s_phase_count >= 6U) {
        (void)rtos_timer_stop(&s_phase_timer);
    }
}

static void periodic_phase_case(void)
{
    uint32_t drift;
    s_phase_count = 0U;
    memset((void *)s_phase_ticks, 0, sizeof(s_phase_ticks));
    (void)rtos_timer_create(&s_phase_timer, "audit_phase", phase_cb, NULL, 20U,
                            RTOS_TIMER_PERIODIC);
    (void)rtos_timer_start(&s_phase_timer);
    rtos_task_delay(200U);
    drift = s_phase_ticks[5] - s_phase_ticks[0];
    check("periodic_absolute_phase", s_phase_count == 6U && drift >= 98U && drift <= 104U,
          (int32_t)drift, 100);
    (void)rtos_timer_delete(&s_phase_timer);
    rtos_task_delay(5U);
}

static rtos_timer_t s_slow_timer;
static rtos_timer_t s_after_slow;
static volatile rtos_tick_t s_slow_start;
static volatile rtos_tick_t s_after_tick;

static void slow_cb(void *arg)
{
    (void)arg;
    s_slow_start = rtos_sched_get_tick_count();
    rtos_task_delay(30U);
}

static void after_slow_cb(void *arg)
{
    (void)arg;
    s_after_tick = rtos_sched_get_tick_count();
}

static void long_callback_case(void)
{
    uint32_t elapsed;
    s_slow_start = 0U;
    s_after_tick = 0U;
    (void)rtos_timer_create(&s_slow_timer, "audit_slow", slow_cb, NULL, 20U,
                            RTOS_TIMER_ONE_SHOT);
    (void)rtos_timer_create(&s_after_slow, "audit_after", after_slow_cb, NULL, 25U,
                            RTOS_TIMER_ONE_SHOT);
    RTOS_PORT_ENTER_CRITICAL();
    (void)rtos_timer_start(&s_slow_timer);
    (void)rtos_timer_start(&s_after_slow);
    RTOS_PORT_EXIT_CRITICAL();
    rtos_task_delay(90U);
    elapsed = s_after_tick - s_slow_start;
    check("long_callback_rechecks_deadline", s_slow_start != 0U && s_after_tick != 0U &&
          elapsed >= 30U && elapsed <= 33U, (int32_t)elapsed, 31);
    (void)rtos_timer_delete(&s_slow_timer);
    (void)rtos_timer_delete(&s_after_slow);
    rtos_task_delay(5U);
}

#if defined(FIRMWARE_AUDIT_REPEAT_TIMER_INIT) && FIRMWARE_AUDIT_REPEAT_TIMER_INIT
static rtos_timer_t s_init_driver;
static rtos_timer_t s_init_future;
static volatile rtos_status_t s_repeat_init_status;
static volatile uint32_t s_init_future_count;

static void repeat_init_driver_cb(void *arg)
{
    (void)arg;
    /* At this point TIMER is running, rather than waiting in its command
     * queue. A repeat init must fail before clearing the active future timer.
     * Do not enable this fixture with the archived pre-audit task module:
     * that module lacks duplicate-TCB protection and rebuilds this live task. */
    s_repeat_init_status = rtos_timer_init();
}

static void repeat_init_future_cb(void *arg)
{
    (void)arg;
    s_init_future_count++;
}

static void repeat_init_case(void)
{
    s_repeat_init_status = RTOS_ERR_PARAM;
    s_init_future_count = 0U;
    (void)rtos_timer_create(&s_init_driver, "audit_init_d", repeat_init_driver_cb,
                            NULL, 20U, RTOS_TIMER_ONE_SHOT);
    (void)rtos_timer_create(&s_init_future, "audit_init_f", repeat_init_future_cb,
                            NULL, 60U, RTOS_TIMER_ONE_SHOT);
    RTOS_PORT_ENTER_CRITICAL();
    (void)rtos_timer_start(&s_init_driver);
    (void)rtos_timer_start(&s_init_future);
    RTOS_PORT_EXIT_CRITICAL();
    rtos_task_delay(100U);
    check("timer_repeat_init_rejected", s_repeat_init_status == RTOS_ERR_ALREADY,
          s_repeat_init_status, RTOS_ERR_ALREADY);
    check("timer_repeat_init_keeps_active", s_init_future_count == 1U,
          (int32_t)s_init_future_count, 1);
    (void)rtos_timer_delete(&s_init_driver);
    (void)rtos_timer_delete(&s_init_future);
    rtos_task_delay(5U);
}
#endif

static void perf_disabled_case(void)
{
#if RTOS_CONFIG_USE_PERF_MONITOR && !RTOS_CONFIG_PERF_HOTPATH_STATS
    rtos_perf_system_t stats;
    rtos_perf_reset();
    rtos_task_delay(20U);
    (void)rtos_perf_get_system(&stats);
    check("perf_disabled_cpu_zero", stats.cpu_usage_x100 == 0U,
          (int32_t)stats.cpu_usage_x100, 0);
#endif
}

static rtos_timer_t s_irq_driver;
static rtos_timer_t s_irq_probe;
static volatile uint32_t s_irq_count;
static volatile uint32_t s_irq_probe_count;
static volatile uint8_t s_irq_probe_active_inside_isr;
static volatile rtos_status_t s_irq_start_status;

static void timer_interrupted_isr(void)
{
    s_irq_count++;
    s_irq_start_status = rtos_timer_start(&s_irq_probe);
    /* ISR must enqueue even when current_tcb points to TIMER. Only the service
     * task may make this newly created timer active after the ISR returns. */
    s_irq_probe_active_inside_isr = s_irq_probe.is_active;
    kern_timer_isr_stop();
}

static void irq_probe_cb(void *arg)
{
    (void)arg;
    s_irq_probe_count++;
}

static void irq_driver_cb(void *arg)
{
    rtos_tick_t begin;
    (void)arg;
    begin = rtos_sched_get_tick_count();
    kern_isr_set_handler(timer_interrupted_isr);
    (void)kern_timer_isr_start(500U, 6U);
    /* Busy for a few real ticks: using delay() would switch current_tcb to
     * IDLE and fail to exercise the interrupted TIMER-task path. */
    while ((rtos_sched_get_tick_count() - begin) < 4U) {
        __asm volatile("nop");
    }
    kern_timer_isr_stop();
    kern_isr_set_handler(NULL);
}

static void interrupted_timer_case(void)
{
    s_irq_count = 0U;
    s_irq_probe_count = 0U;
    s_irq_probe_active_inside_isr = 0xFFU;
    s_irq_start_status = RTOS_ERR_PARAM;
    (void)rtos_timer_create(&s_irq_probe, "audit_irq_p", irq_probe_cb, NULL, 15U,
                            RTOS_TIMER_ONE_SHOT);
    (void)rtos_timer_create(&s_irq_driver, "audit_irq_d", irq_driver_cb, NULL, 20U,
                            RTOS_TIMER_ONE_SHOT);
    (void)rtos_timer_start(&s_irq_driver);
    rtos_task_delay(70U);
    check("timer_irq6_fired_once", s_irq_count == 1U, (int32_t)s_irq_count, 1);
    check("timer_irq_start_accepted", s_irq_start_status == RTOS_OK,
          s_irq_start_status, RTOS_OK);
    check("timer_irq_only_enqueues", s_irq_probe_active_inside_isr == 0U,
          s_irq_probe_active_inside_isr, 0);
    check("timer_irq_started_callback", s_irq_probe_count == 1U,
          (int32_t)s_irq_probe_count, 1);
    (void)rtos_timer_delete(&s_irq_driver);
    (void)rtos_timer_delete(&s_irq_probe);
    rtos_task_delay(5U);
}

void audit_memory_timer_run(void)
{
    s_pass = 0U;
    s_fail = 0U;
    memory_cases();
    callback_restart_cases();
    lifetime_cases();
    periodic_phase_case();
    long_callback_case();
    interrupted_timer_case();
#if defined(FIRMWARE_AUDIT_REPEAT_TIMER_INIT) && FIRMWARE_AUDIT_REPEAT_TIMER_INIT
    repeat_init_case();
#endif
    perf_disabled_case();
    printf("AT|MEMTIMER|SUMMARY|pass=%lu|fail=%lu\r\n", (unsigned long)s_pass,
           (unsigned long)s_fail);
}
