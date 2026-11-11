/** @file h743_lifecycle_stress.c
 * @brief Exercise sustained task, IRQ, IPC and heap contention.
 */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "production_timer_target.h"
#include "rtos.h"

#ifndef H743_LIFECYCLE_SECONDS
#define H743_LIFECYCLE_SECONDS 60U
#endif

#ifndef H743_LIFECYCLE_SEED
#define H743_LIFECYCLE_SEED UINT32_C(3735928559)
#endif

#if (H743_LIFECYCLE_SECONDS < 2U) || (H743_LIFECYCLE_SECONDS > 300U)
#error "H743_LIFECYCLE_SECONDS must be between 2 and 300"
#endif

enum {
    H7L_SUPERVISOR_PRIO = 4U,
    H7L_CONSUMER_PRIO = 6U,
    H7L_WORKER_PRIO = 8U,
    H7L_PRODUCER_PRIO = 17U,
    H7L_PRESSURE_PRIO = 24U,
    H7L_REALTIME_IRQ_PRIO = 1U,
    H7L_PRODUCERS = 3U,
    H7L_SOURCES = H7L_PRODUCERS + 1U,
    H7L_TIMER_SOURCE = H7L_PRODUCERS,
    H7L_TRAFFIC_CAPACITY = 5U,
    H7L_WAIT_KINDS = 4U,
    H7L_WAITERS = H7L_WAIT_KINDS * 2U,
    H7L_STACK_WORDS = 192U,
    H7L_PRESSURE_STACK_WORDS = 128U,
    H7L_PRESSURE_LIMIT = 80U,
    H7L_BOUND_TICKS = 100U,
    H7L_FINITE_TICKS = 6U,
    H7L_TIMER_COUNTER_HZ = 1000000U,
    H7L_IRQ_PERIOD_US = 400U,
    H7L_IRQ_MIN_RATE_HZ = 2000U,
    H7L_IRQ_NEST_INTERVAL = 8U
};

_Static_assert(PRODUCTION_TIMER_CLOCK_HZ % H7L_TIMER_COUNTER_HZ == 0U,
               "TIM2 must divide exactly to the lifecycle counter clock");

typedef enum wait_kind {
    WAIT_QUEUE,
    WAIT_SEM,
    WAIT_EVENT,
    WAIT_NOTIFY
} wait_kind_t;

typedef enum round_mode {
    ROUND_DELIVER,
    ROUND_REOPEN,
    ROUND_TIMEOUT,
    ROUND_DROP_WAITERS,
    ROUND_MODES
} round_mode_t;

typedef enum run_phase {
    PHASE_NORMAL,
    PHASE_OVERLOAD,
    PHASE_RECOVERY,
    PHASE_COUNT
} run_phase_t;

typedef struct packet {
    uint32_t source;
    uint32_t sequence;
    uint32_t tag;
} packet_t;

typedef struct source_stats {
    _Atomic uint32_t attempts;
    _Atomic uint32_t accepted;
    _Atomic uint32_t refused;
    _Atomic uint32_t received;
    _Atomic bool stopped;
    uint32_t random;
} source_stats_t;

typedef struct waiter {
    rtos_tcb_t *task;
    wait_kind_t kind;
    rtos_tick_t timeout;
    rtos_status_t status;
    uint32_t value;
    rtos_status_t followup_status;
    uint32_t followup_value;
    bool has_followup;
    bool self_delete;
    _Atomic bool finished;
} waiter_t;

static source_stats_t s_sources[H7L_SOURCES];
static rtos_queue_t s_traffic_queue;
static packet_t s_traffic_storage[H7L_TRAFFIC_CAPACITY];
static rtos_tcb_t s_traffic_tasks[H7L_PRODUCERS + 1U];
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT)
    rtos_stack_t s_traffic_stacks[H7L_PRODUCERS + 1U][H7L_STACK_WORDS];
static _Atomic bool s_stop_producers;
static _Atomic bool s_drain_consumer;
static _Atomic bool s_consumer_stopped;
static _Atomic run_phase_t s_phase;
static _Atomic uint32_t s_worker_failure;
static _Atomic uint32_t s_irq_entries;
static _Atomic uint32_t s_irq_max_gap_cycles;
static uint32_t s_last_irq_cycle;
static _Atomic uint32_t s_nested_irq_entries;
static _Atomic bool s_timer_active;

static waiter_t s_waiters[H7L_WAITERS];
static rtos_queue_t s_wait_queue;
static uint32_t s_wait_storage[2];
static rtos_sem_t s_wait_sem;
static rtos_event_t s_wait_event;
static rtos_tcb_t *s_pressure_tasks[H7L_PRESSURE_LIMIT];
static _Atomic uint32_t s_pressure_parked;
static uint32_t s_heap_baseline;
static uint32_t s_heap_baseline_largest;
static uint32_t s_heap_baseline_live;
static uint32_t s_heap_minimum;
static uint32_t s_peak_pressure_tasks;
static uint32_t s_created;
static uint32_t s_deleted;
static uint32_t s_self_deleted;
static uint32_t s_dropped;
static uint32_t s_round_counts[ROUND_MODES];
static uint32_t s_rounds;

void h743_lifecycle_stress_run(void *argument);
void TIM2_IRQHandler(void);
void EXTI4_IRQHandler(void);

static void stop_irq_sources(void)
{
    if ((PRODUCTION_TIMER_APB_ENABLE & PRODUCTION_TIMER_TIM2_ENABLE) != 0U) {
        TIM2->DIER = 0U;
        TIM2->CR1 = 0U;
    }
    NVIC_DisableIRQ(TIM2_IRQn);
    NVIC_DisableIRQ(EXTI4_IRQn);
    NVIC_ClearPendingIRQ(TIM2_IRQn);
    NVIC_ClearPendingIRQ(EXTI4_IRQn);
    __DSB();
}

static _Noreturn void fail(const char *name, int32_t actual, int32_t expected)
{
    stop_irq_sources();
    (void)printf("H7L|FAIL|case=%s|actual=%" PRId32 "|expected=%" PRId32 "\r\n",
                 name, actual, expected);
    (void)printf("H7L|DONE|status=1\r\n");
    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}

static void require_equal(const char *name, int32_t actual, int32_t expected)
{
    if (actual != expected) {
        fail(name, actual, expected);
    }
}

static void require_true(const char *name, bool condition)
{
    require_equal(name, condition, true);
}

static void report_worker_failure(uint32_t code)
{
    uint32_t expected = 0U;
    (void)atomic_compare_exchange_strong(&s_worker_failure, &expected, code);
}

static uint32_t next_random(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

static uint32_t packet_tag(uint32_t source, uint32_t sequence)
{
    return UINT32_C(0x5381C4AF) ^ (source * UINT32_C(0x9E3779B9)) ^ sequence;
}

static void submit_packet(uint32_t source, rtos_tick_t timeout)
{
    source_stats_t *stats = &s_sources[source];
    const uint32_t sequence = stats->accepted;
    const packet_t packet = {source, sequence, packet_tag(source, sequence)};
    ++stats->attempts;
    const rtos_status_t status = rtos_queue_send(&s_traffic_queue, &packet, timeout, RTOS_FALSE);
    if (status == RTOS_OK) {
        ++stats->accepted;
    } else if (status == RTOS_ERR_TIMEOUT) {
        ++stats->refused;
    } else {
        report_worker_failure(10U + source);
    }
}

void EXTI4_IRQHandler(void)
{
    if ((s_timer_active == false) || (__get_IPSR() != (uint32_t)EXTI4_IRQn + 16U)) {
        report_worker_failure(20U);
    }
    ++s_nested_irq_entries;
}

void TIM2_IRQHandler(void)
{
    if ((TIM2->SR & TIM_SR_UIF) == 0U) {
        return;
    }
    TIM2->SR = 0U;
    (void)TIM2->SR;
    const uint32_t entry = ++s_irq_entries;
    const uint32_t cycle = DWT->CYCCNT;
    const uint32_t gap = cycle - s_last_irq_cycle;
    s_last_irq_cycle = cycle;
    if (entry > 1U && gap > s_irq_max_gap_cycles) {
        s_irq_max_gap_cycles = gap;
    }
    if ((entry % H7L_IRQ_NEST_INTERVAL) == 0U) {
        s_timer_active = true;
        NVIC_SetPendingIRQ(EXTI4_IRQn);
        __DSB();
        __ISB();
        s_timer_active = false;
    }
    submit_packet(H7L_TIMER_SOURCE, RTOS_NO_WAIT);
}

static void start_irq_sources(void)
{
    const uint32_t priority_shift = 8U - __NVIC_PRIO_BITS;
    const uint32_t syscall_prio = RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY >> priority_shift;
    require_true("irq_priority", syscall_prio > H7L_REALTIME_IRQ_PRIO);
    require_true("dwt_enabled", (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U);
    require_true("timer_clock_profile", PRODUCTION_TIMER_PROFILE_VALID());
    PRODUCTION_TIMER_APB_ENABLE |= PRODUCTION_TIMER_TIM2_ENABLE;
    (void)PRODUCTION_TIMER_APB_ENABLE;
    stop_irq_sources();
    TIM2->PSC = (PRODUCTION_TIMER_CLOCK_HZ / H7L_TIMER_COUNTER_HZ) - 1U;
    TIM2->ARR = H7L_IRQ_PERIOD_US - 1U;
    TIM2->EGR = TIM_EGR_UG;
    for (uint32_t spin = 0U; spin < 10000U && (TIM2->SR & TIM_SR_UIF) == 0U; ++spin) {
    }
    require_true("timer_update", (TIM2->SR & TIM_SR_UIF) != 0U);
    TIM2->SR = 0U;
    NVIC_SetPriority(EXTI4_IRQn, H7L_REALTIME_IRQ_PRIO);
    NVIC_SetPriority(TIM2_IRQn, syscall_prio);
    NVIC_EnableIRQ(EXTI4_IRQn);
    NVIC_EnableIRQ(TIM2_IRQn);
    TIM2->DIER = TIM_DIER_UIE;
    TIM2->CR1 = TIM_CR1_CEN;
}

static void run_producer(void *argument)
{
    const uint32_t source = (uint32_t)(uintptr_t)argument;
    source_stats_t *stats = &s_sources[source];
    while (!s_stop_producers) {
        const uint32_t random = next_random(&stats->random);
        submit_packet(source, random % 3U);
        rtos_task_delay(1U + (random % 3U));
    }
    stats->stopped = true;
    rtos_task_delay(RTOS_WAIT_FOREVER);
    report_worker_failure(30U + source);
}

static void run_consumer(void *argument)
{
    (void)argument;
    for (;;) {
        const bool draining = s_drain_consumer;
        packet_t packet;
        const rtos_tick_t timeout = draining ? RTOS_NO_WAIT : 2U;
        const rtos_status_t status = rtos_queue_recv(&s_traffic_queue, &packet, timeout);
        if (status == RTOS_ERR_TIMEOUT) {
            if (draining) {
                break;
            }
            continue;
        }
        if (status != RTOS_OK || packet.source >= H7L_SOURCES) {
            report_worker_failure(40U);
            break;
        }
        source_stats_t *stats = &s_sources[packet.source];
        if (packet.sequence != stats->received ||
            packet.tag != packet_tag(packet.source, packet.sequence)) {
            report_worker_failure(41U + packet.source);
            break;
        }
        ++stats->received;
        if (!draining && s_phase == PHASE_OVERLOAD) {
            rtos_task_delay(3U);
        }
    }
    s_consumer_stopped = true;
    rtos_task_delay(RTOS_WAIT_FOREVER);
    report_worker_failure(45U);
}

static void start_traffic(uint32_t seed)
{
    require_equal("traffic_queue_init",
                  rtos_queue_init(&s_traffic_queue, s_traffic_storage, sizeof(packet_t),
                                  H7L_TRAFFIC_CAPACITY), RTOS_OK);
    for (uint32_t i = 0U; i < H7L_PRODUCERS; ++i) {
        s_sources[i].random = next_random(&seed);
        require_equal("producer_create",
                      rtos_task_create_static(&s_traffic_tasks[i], s_traffic_stacks[i],
                                              H7L_STACK_WORDS, run_producer,
                                              (void *)(uintptr_t)i,
                                              H7L_PRODUCER_PRIO + (rtos_prio_t)i, "h7prod"),
                      RTOS_OK);
    }
    require_equal("consumer_create",
                  rtos_task_create_static(&s_traffic_tasks[H7L_PRODUCERS],
                                          s_traffic_stacks[H7L_PRODUCERS], H7L_STACK_WORDS,
                                          run_consumer, NULL, H7L_CONSUMER_PRIO, "h7cons"),
                  RTOS_OK);
    start_irq_sources();
}

static void stop_traffic(void)
{
    stop_irq_sources();
    s_stop_producers = true;
    const rtos_tick_t deadline = rtos_sched_get_tick_count();
    for (uint32_t i = 0U; i < H7L_PRODUCERS; ++i) {
        while (!s_sources[i].stopped) {
            require_true("producer_stop_bound",
                         rtos_sched_get_tick_count() - deadline < H7L_BOUND_TICKS);
            rtos_task_delay(1U);
        }
    }
    s_drain_consumer = true;
    while (!s_consumer_stopped) {
        require_true("consumer_stop_bound",
                     rtos_sched_get_tick_count() - deadline < H7L_BOUND_TICKS);
        rtos_task_delay(1U);
    }
    for (uint32_t i = 0U; i <= H7L_PRODUCERS; ++i) {
        require_equal("traffic_task_delete", rtos_task_delete(&s_traffic_tasks[i]), RTOS_OK);
    }
    require_equal("queue_drained", (int32_t)rtos_queue_get_count(&s_traffic_queue), 0);
    require_equal("traffic_queue_deinit", rtos_queue_deinit(&s_traffic_queue), RTOS_OK);
}

static void capture_heap_minimum(void)
{
    const uint32_t free_bytes = rtos_heap_get_free();
    if (free_bytes < s_heap_minimum) {
        s_heap_minimum = free_bytes;
    }
}

static uint32_t heap_live_allocations(void)
{
    return rtos_heap_get_alloc_count() - rtos_heap_get_free_count();
}

static void wait_heap_recovery(void)
{
    const rtos_tick_t start = rtos_sched_get_tick_count();
    uint32_t live = heap_live_allocations();
    while ((live != s_heap_baseline_live) &&
           (rtos_sched_get_tick_count() - start < H7L_BOUND_TICKS)) {
        rtos_task_delay(1U);
        live = heap_live_allocations();
    }
    /* Lazy coalescing can change the raw free counter by leaving block headers. */
    require_equal("heap_live_recovered", (int32_t)live, (int32_t)s_heap_baseline_live);
    require_equal("heap_largest_recovered", (int32_t)rtos_heap_get_max_alloc(),
                  (int32_t)s_heap_baseline_largest);
}

static void park_pressure_task(void *argument)
{
    (void)argument;
    ++s_pressure_parked;
    rtos_task_delay(RTOS_WAIT_FOREVER);
    report_worker_failure(50U);
}

static void check_fragmented_heap(void)
{
    const uint32_t free_bytes = rtos_heap_get_free();
    const uint32_t largest = rtos_heap_get_max_alloc();
    const uint32_t live_before = heap_live_allocations();
    require_true("pressure_fragmented", largest > 0U && largest < free_bytes);
    require_true("pressure_reject_oversize", rtos_heap_alloc(largest + 1U) == NULL);
    require_equal("pressure_oversize_no_leak", (int32_t)rtos_heap_get_free(), (int32_t)free_bytes);
    void *const allocation = rtos_heap_alloc(largest);
    require_true("pressure_exact_fit", allocation != NULL);
    rtos_heap_free(allocation);
    require_equal("pressure_exact_fit_free", (int32_t)heap_live_allocations(),
                  (int32_t)live_before);
    require_equal("pressure_exact_fit_max", (int32_t)rtos_heap_get_max_alloc(), (int32_t)largest);
}

static void probe_heap_pressure(run_phase_t phase)
{
    uint32_t count = 0U;
    s_pressure_parked = 0U;
    while (count < H7L_PRESSURE_LIMIT) {
        rtos_tcb_t *const sentinel = rtos_task_get_current();
        rtos_tcb_t *created = sentinel;
        const uint32_t live_before = heap_live_allocations();
        const rtos_status_t status = rtos_task_create(&created, H7L_PRESSURE_STACK_WORDS,
                                                      park_pressure_task, NULL,
                                                      H7L_PRESSURE_PRIO, "h7pressure");
        if (status == RTOS_ERR_NO_MEM) {
            require_true("oom_output_untouched", created == sentinel);
            require_equal("oom_no_leak", (int32_t)heap_live_allocations(), (int32_t)live_before);
            break;
        }
        require_equal("pressure_create", status, RTOS_OK);
        require_true("pressure_output_valid", created != NULL && created != sentinel);
        s_pressure_tasks[count++] = created;
        ++s_created;
    }
    require_true("pressure_hit_oom", count > H7L_WAITERS && count < H7L_PRESSURE_LIMIT);
    capture_heap_minimum();
    if (count > s_peak_pressure_tasks) {
        s_peak_pressure_tasks = count;
    }
    const rtos_tick_t start = rtos_sched_get_tick_count();
    while (s_pressure_parked != count) {
        require_true("pressure_start_bound", rtos_sched_get_tick_count() - start < H7L_BOUND_TICKS);
        rtos_task_delay(1U);
    }
    /* Free alternating tasks first to force fragmentation before complete cleanup. */
    for (uint32_t parity = 0U; parity < 2U; ++parity) {
        for (uint32_t i = parity; i < count; i += 2U) {
            require_equal("pressure_delete", rtos_task_delete(s_pressure_tasks[i]), RTOS_OK);
            s_pressure_tasks[i] = NULL;
            ++s_deleted;
        }
        if (parity == 0U) {
            check_fragmented_heap();
        }
    }
    wait_heap_recovery();
    (void)printf("H7L|PRESSURE|phase=%u|tasks=%" PRIu32 "|heap_min=%" PRIu32 "\r\n",
                 (unsigned int)phase, count, s_heap_minimum);
}

static void run_waiter(void *argument)
{
    waiter_t *const waiter = argument;
    switch (waiter->kind) {
    case WAIT_QUEUE:
        waiter->status = rtos_queue_recv(&s_wait_queue, &waiter->value, waiter->timeout);
        break;
    case WAIT_SEM:
        waiter->status = rtos_sem_take(&s_wait_sem, waiter->timeout);
        break;
    case WAIT_EVENT:
        waiter->value = rtos_event_wait(&s_wait_event, waiter->value,
                                        RTOS_EVENT_WAIT_ALL | RTOS_EVENT_CLEAR_ON_EXIT,
                                        waiter->timeout);
        break;
    case WAIT_NOTIFY:
        waiter->status = rtos_task_notify_wait(waiter->timeout, &waiter->value);
        if (waiter->has_followup) {
            waiter->followup_status = rtos_task_notify_wait(RTOS_NO_WAIT, &waiter->followup_value);
        }
        break;
    }
    waiter->finished = true;
    if (waiter->self_delete) {
        (void)rtos_task_delete(NULL);
        report_worker_failure(60U);
    }
    rtos_task_delay(RTOS_WAIT_FOREVER);
    report_worker_failure(61U);
}

static void init_wait_objects(rtos_status_t expected)
{
    require_equal("wait_queue_init",
                  rtos_queue_init(&s_wait_queue, s_wait_storage, sizeof(s_wait_storage[0]), 2U),
                  expected);
    require_equal("wait_sem_init", rtos_sem_init(&s_wait_sem, 0U, 2U), expected);
    require_equal("wait_event_init", rtos_event_init(&s_wait_event), expected);
}

static void close_wait_objects(void)
{
    require_equal("wait_queue_deinit", rtos_queue_deinit(&s_wait_queue), RTOS_OK);
    require_equal("wait_sem_deinit", rtos_sem_deinit(&s_wait_sem), RTOS_OK);
    require_equal("wait_event_deinit", rtos_event_deinit(&s_wait_event), RTOS_OK);
}

static void reopen_wait_objects(uint32_t payload)
{
    close_wait_objects();
    init_wait_objects(RTOS_OK);
    require_equal("reopen_queue_send",
                  rtos_queue_send(&s_wait_queue, &payload, RTOS_NO_WAIT, RTOS_FALSE), RTOS_OK);
    require_equal("reopen_sem_give", rtos_sem_give(&s_wait_sem), RTOS_OK);
    (void)rtos_event_set(&s_wait_event, 3U);
    for (uint32_t rank = 0U; rank < 2U; ++rank) {
        rtos_tcb_t *const task = s_waiters[(rank * H7L_WAIT_KINDS) + WAIT_NOTIFY].task;
        require_equal("wait_notify_suspend", rtos_task_suspend(task), RTOS_OK);
        require_equal("wait_notify_resume", rtos_task_resume(task), RTOS_OK);
        require_equal("wait_notify_new_value",
                      rtos_task_notify(task, payload + rank, RTOS_NOTIFY_VALUE), RTOS_OK);
    }
}

static void check_reopened_objects(uint32_t payload)
{
    uint32_t value = 0U;
    require_equal("reopen_queue_recv", rtos_queue_recv(&s_wait_queue, &value, RTOS_NO_WAIT),
                  RTOS_OK);
    require_equal("reopen_queue_data", (int32_t)value, (int32_t)payload);
    require_equal("reopen_sem_take", rtos_sem_take(&s_wait_sem, RTOS_NO_WAIT), RTOS_OK);
    require_equal("reopen_event_bits",
                  (int32_t)rtos_event_wait(&s_wait_event, 3U,
                                           RTOS_EVENT_WAIT_ALL | RTOS_EVENT_CLEAR_ON_EXIT,
                                           RTOS_NO_WAIT), 3);
}

static void wait_all_blocked(rtos_task_state_t expected)
{
    const rtos_tick_t start = rtos_sched_get_tick_count();
    for (;;) {
        uint32_t waiting = 0U;
        RTOS_PORT_ENTER_CRITICAL();
        for (uint32_t i = 0U; i < H7L_WAITERS; ++i) {
            if (!s_waiters[i].finished && s_waiters[i].task->state == expected) {
                ++waiting;
            }
        }
        RTOS_PORT_EXIT_CRITICAL();
        if (waiting == H7L_WAITERS) {
            return;
        }
        if (rtos_sched_get_tick_count() - start >= H7L_BOUND_TICKS) {
            fail("waiters_blocked_bound", (int32_t)waiting, H7L_WAITERS);
        }
        rtos_task_delay(1U);
    }
}

static void deliver_waiters(uint32_t first_rank, uint32_t payload)
{
    for (uint32_t rank = first_rank; rank < 2U; ++rank) {
        const uint32_t data = payload + rank;
        const uint32_t bit = 1UL << rank;
        require_equal("wait_queue_send",
                      rtos_queue_send(&s_wait_queue, &data, RTOS_NO_WAIT, RTOS_FALSE), RTOS_OK);
        require_equal("wait_sem_give", rtos_sem_give(&s_wait_sem), RTOS_OK);
        (void)rtos_event_set(&s_wait_event, bit);
        require_equal("wait_event_consumed", (int32_t)rtos_event_get(&s_wait_event), 0);
        require_equal("wait_notify_send",
                      rtos_task_notify(s_waiters[(rank * H7L_WAIT_KINDS) + WAIT_NOTIFY].task,
                                       data, RTOS_NOTIFY_VALUE), RTOS_OK);
    }
    require_equal("wait_queue_consumed", (int32_t)rtos_queue_get_count(&s_wait_queue), 0);
    require_equal("wait_sem_consumed", (int32_t)rtos_sem_get_count(&s_wait_sem), 0);
}

static void complete_waiter(uint32_t i, round_mode_t mode, uint32_t payload, rtos_tick_t start)
{
    waiter_t *const waiter = &s_waiters[i];
    if (waiter->task == NULL) {
        require_true("deleted_waiter_never_ran", !waiter->finished);
        return;
    }
    while (!waiter->finished) {
        require_true("waiter_finish_bound", rtos_sched_get_tick_count() - start < H7L_BOUND_TICKS);
        rtos_task_delay(1U);
    }
    const bool delivered = mode == ROUND_DELIVER || mode == ROUND_DROP_WAITERS;
    const rtos_status_t expected = delivered ? RTOS_OK :
        (mode == ROUND_TIMEOUT ? RTOS_ERR_TIMEOUT : RTOS_ERR_DELETED);
    const uint32_t rank = i / H7L_WAIT_KINDS;
    if (waiter->kind == WAIT_EVENT) {
        require_equal("wait_event_result", (int32_t)waiter->value,
                      delivered ? (int32_t)(1UL << rank) : 0);
    } else {
        require_equal("wait_result", waiter->status, expected);
        if (delivered && (waiter->kind == WAIT_QUEUE || waiter->kind == WAIT_NOTIFY)) {
            require_equal("wait_data", (int32_t)waiter->value, (int32_t)(payload + rank));
        }
        if (waiter->has_followup) {
            require_equal("wait_notify_followup", waiter->followup_status, RTOS_OK);
            require_equal("wait_notify_followup_data", (int32_t)waiter->followup_value,
                          (int32_t)(payload + rank));
        }
    }
    if (waiter->self_delete) {
        ++s_self_deleted;
    } else {
        require_equal("wait_task_delete", rtos_task_delete(waiter->task), RTOS_OK);
        ++s_deleted;
    }
    waiter->task = NULL;
}

static void run_lifecycle_round(round_mode_t mode, uint32_t random)
{
    init_wait_objects(RTOS_OK);
    const uint32_t payload = UINT32_C(0x36000000) | ((s_rounds & UINT32_C(0xFFFF)) << 1U);
    for (uint32_t i = 0U; i < H7L_WAITERS; ++i) {
        waiter_t *const waiter = &s_waiters[i];
        const uint32_t rank = i / H7L_WAIT_KINDS;
        waiter->kind = (wait_kind_t)(i % H7L_WAIT_KINDS);
        waiter->timeout = mode == ROUND_TIMEOUT ? H7L_FINITE_TICKS : RTOS_WAIT_FOREVER;
        waiter->status = RTOS_OK;
        waiter->value = 1UL << rank;
        waiter->has_followup = mode == ROUND_REOPEN && waiter->kind == WAIT_NOTIFY;
        waiter->self_delete = ((i + random) & 1U) == 0U;
        waiter->finished = false;
        const rtos_prio_t priority = H7L_WORKER_PRIO + (rtos_prio_t)(rank * H7L_WAIT_KINDS) +
            (rtos_prio_t)((i + random) % H7L_WAIT_KINDS);
        require_equal("wait_task_create",
                      rtos_task_create(&waiter->task, H7L_STACK_WORDS + ((i % 3U) * 32U),
                                       run_waiter, waiter, priority, "h7wait"), RTOS_OK);
        ++s_created;
    }
    capture_heap_minimum();
    wait_all_blocked(mode == ROUND_TIMEOUT ? RTOS_TASK_DELAYED : RTOS_TASK_BLOCKED);
    init_wait_objects(RTOS_ERR_ALREADY);

    if (mode == ROUND_REOPEN) {
        reopen_wait_objects(payload);
    } else if (mode == ROUND_DROP_WAITERS) {
        for (uint32_t i = 0U; i < H7L_WAIT_KINDS; ++i) {
            require_equal("delete_blocked", rtos_task_delete(s_waiters[i].task), RTOS_OK);
            s_waiters[i].task = NULL;
            ++s_deleted;
            ++s_dropped;
        }
        deliver_waiters(1U, payload);
    } else if (mode == ROUND_DELIVER) {
        deliver_waiters(0U, payload);
    }

    const rtos_tick_t finish_start = rtos_sched_get_tick_count();
    for (uint32_t i = 0U; i < H7L_WAITERS; ++i) {
        complete_waiter(i, mode, payload, finish_start);
    }
    if (mode == ROUND_REOPEN) {
        check_reopened_objects(payload);
    }
    close_wait_objects();
    wait_heap_recovery();
    ++s_round_counts[mode];
    ++s_rounds;
}

static void print_progress(rtos_tick_t elapsed)
{
    (void)printf("H7L|PROGRESS|ticks=%" PRIu32 "|phase=%u|rounds=%" PRIu32 "|irq=%" PRIu32
                 "|irq_refused=%" PRIu32 "\r\n", elapsed, (unsigned int)s_phase, s_rounds,
                 (uint32_t)s_irq_entries, (uint32_t)s_sources[H7L_TIMER_SOURCE].refused);
}

static void check_phase_irq_cadence(run_phase_t phase, rtos_tick_t start, uint32_t entries_before)
{
    const rtos_tick_t elapsed = rtos_sched_get_tick_count() - start;
    const uint32_t entries = s_irq_entries - entries_before;
    const uint32_t minimum = (uint32_t)(((uint64_t)elapsed * H7L_IRQ_MIN_RATE_HZ) /
                                        RTOS_CONFIG_TICK_RATE_HZ);
    const uint32_t max_gap = s_irq_max_gap_cycles;
    (void)printf("H7L|PHASE|phase=%u|ticks=%" PRIu32 "|irq=%" PRIu32 "|max_gap_cycles=%" PRIu32
                 "\r\n", (unsigned int)phase, elapsed, entries, max_gap);
    if (entries < minimum) {
        fail("phase_irq_rate", (int32_t)entries, (int32_t)minimum);
    }
    const uint32_t maximum_gap_cycles = SystemCoreClock / 100U;
    if (max_gap > maximum_gap_cycles) {
        fail("irq_service_gap", (int32_t)max_gap, (int32_t)maximum_gap_cycles);
    }
}

static void verify_totals(uint32_t overload_refused)
{
    require_equal("worker_failure", (int32_t)s_worker_failure, 0);
    require_equal("task_conservation", (int32_t)(s_deleted + s_self_deleted), (int32_t)s_created);
    require_true("drop_coverage", s_dropped > 0U);
    require_true("self_delete_coverage", s_self_deleted > 0U);
    require_true("overload_refusal_coverage", overload_refused > 0U);
    require_true("irq_coverage", s_nested_irq_entries > 0U);
    require_equal("irq_conservation", (int32_t)s_irq_entries,
                  (int32_t)s_sources[H7L_TIMER_SOURCE].attempts);
    require_equal("nested_irq_conservation", (int32_t)s_nested_irq_entries,
                  (int32_t)(s_irq_entries / H7L_IRQ_NEST_INTERVAL));
    wait_heap_recovery();
    for (uint32_t mode = 0U; mode < ROUND_MODES; ++mode) {
        require_true("round_mode_coverage", s_round_counts[mode] > 0U);
    }
    for (uint32_t i = 0U; i < H7L_SOURCES; ++i) {
        const source_stats_t *stats = &s_sources[i];
        const uint32_t accepted = stats->accepted;
        const uint32_t refused = stats->refused;
        const uint32_t received = stats->received;
        require_true("source_coverage", accepted > 0U);
        require_equal("attempt_conservation", (int32_t)stats->attempts,
                      (int32_t)(accepted + refused));
        require_equal("packet_conservation", (int32_t)received, (int32_t)accepted);
        (void)printf("H7L|SOURCE|id=%" PRIu32 "|accepted=%" PRIu32 "|refused=%" PRIu32
                     "|received=%" PRIu32 "\r\n", i, accepted, refused, received);
    }
}

void h743_lifecycle_stress_run(void *argument)
{
    (void)argument;
    require_equal("supervisor_priority", rtos_task_get_current()->priority, H7L_SUPERVISOR_PRIO);
    const uint32_t seed = (uint32_t)H743_LIFECYCLE_SEED;
    uint32_t random = seed == 0U ? 1U : seed;
    const rtos_tick_t duration = H743_LIFECYCLE_SECONDS * RTOS_CONFIG_TICK_RATE_HZ;
    s_heap_baseline = rtos_heap_get_free();
    s_heap_baseline_largest = rtos_heap_get_max_alloc();
    s_heap_baseline_live = heap_live_allocations();
    s_heap_minimum = s_heap_baseline;
    (void)printf("H7L|START|seed=%" PRIu32 "|seconds=%u|heap=%" PRIu32 "\r\n",
                 seed, H743_LIFECYCLE_SECONDS, s_heap_baseline);
    start_traffic(random);

    const rtos_tick_t start = rtos_sched_get_tick_count();
    rtos_tick_t last_report = 0U;
    uint32_t overload_refused = 0U;
    for (uint32_t phase = 0U; phase < PHASE_COUNT; ++phase) {
        s_phase = (run_phase_t)phase;
        const rtos_tick_t phase_start = rtos_sched_get_tick_count();
        const uint32_t entries_before = s_irq_entries;
        const uint32_t refused_before = s_sources[H7L_TIMER_SOURCE].refused;
        probe_heap_pressure((run_phase_t)phase);
        const rtos_tick_t phase_end = (duration * (phase + 1U)) / PHASE_COUNT;
        do {
            const uint32_t value = next_random(&random);
            const uint32_t mode = s_rounds < ROUND_MODES ? s_rounds : value % ROUND_MODES;
            run_lifecycle_round((round_mode_t)mode, value);
            require_equal("worker_failure", (int32_t)s_worker_failure, 0);
            const rtos_tick_t elapsed = rtos_sched_get_tick_count() - start;
            if (elapsed - last_report >= RTOS_CONFIG_TICK_RATE_HZ) {
                print_progress(elapsed);
                last_report = elapsed;
            }
        } while (rtos_sched_get_tick_count() - start < phase_end);
        if (phase == PHASE_OVERLOAD) {
            overload_refused = s_sources[H7L_TIMER_SOURCE].refused - refused_before;
        }
        check_phase_irq_cadence((run_phase_t)phase, phase_start, entries_before);
    }

    stop_traffic();
    verify_totals(overload_refused);
    (void)printf("H7L|TOTAL|rounds=%" PRIu32 "|deliver=%" PRIu32 "|reopen=%" PRIu32
                 "|timeout=%" PRIu32 "|drop_rounds=%" PRIu32 "|tasks=%" PRIu32
                 "|self_delete=%" PRIu32 "|external_delete=%" PRIu32 "|blocked_delete=%" PRIu32
                 "|pressure_peak=%" PRIu32 "|heap_min=%" PRIu32 "|heap_raw_free=%" PRIu32
                 "|heap_max_alloc=%" PRIu32 "|heap_live=%" PRIu32
                 "|irq=%" PRIu32 "|nested=%" PRIu32 "|overload_refused=%" PRIu32 "\r\n",
                 s_rounds, s_round_counts[ROUND_DELIVER], s_round_counts[ROUND_REOPEN],
                 s_round_counts[ROUND_TIMEOUT], s_round_counts[ROUND_DROP_WAITERS],
                 s_created, s_self_deleted, s_deleted, s_dropped, s_peak_pressure_tasks,
                 s_heap_minimum, rtos_heap_get_free(), rtos_heap_get_max_alloc(),
                 heap_live_allocations(), (uint32_t)s_irq_entries,
                 (uint32_t)s_nested_irq_entries, overload_refused);
    (void)printf("H7L|DONE|status=0\r\n");
    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}
