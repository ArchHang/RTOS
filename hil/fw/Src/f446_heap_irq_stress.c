/**
 * @file f446_heap_irq_stress.c
 * @brief Exercise heap ownership across tasks, a timer IRQ and a nested legal IRQ.
 *
 * Queue messages transfer one allocated packet to the consumer. A failed send
 * leaves ownership with its producer. Per-source counters have one writer each
 * and use atomics for final observation. Stop/done flags and the first failure
 * are atomic. Packet contents have one owner and queue operations publish them.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "production_timer_target.h"
#include "rtos.h"

#ifndef HIRQ_SECONDS
#define HIRQ_SECONDS 20U
#endif

#if (HIRQ_SECONDS < 5U) || (HIRQ_SECONDS > 120U)
#error "HIRQ_SECONDS must be between 5 and 120"
#endif

enum {
    HIRQ_PRODUCERS = 2U,
    HIRQ_TIMER_SOURCE = HIRQ_PRODUCERS,
    HIRQ_NESTED_SOURCE = HIRQ_PRODUCERS + 1U,
    HIRQ_SOURCES = HIRQ_PRODUCERS + 2U,
    HIRQ_CONSUMER_TASK = HIRQ_PRODUCERS,
    HIRQ_PRESSURE_TASK = HIRQ_PRODUCERS + 1U,
    HIRQ_TASKS = HIRQ_PRODUCERS + 2U,
    HIRQ_STACK_WORDS = 512U,
    HIRQ_QUEUE_CAPACITY = 16U,
    HIRQ_TIMER_COUNTER_HZ = 1000000U,
    HIRQ_TIMER_PERIOD_US = 1000U,
    HIRQ_NEST_INTERVAL = 8U,
    HIRQ_WARMUP_SECONDS = 2U,
    HIRQ_BURST = 16U,
    HIRQ_SHUTDOWN_TICKS = 1000U,
    HIRQ_PACKET_TAG = 0x5381C4AFU
};

_Static_assert(PRODUCTION_TIMER_CLOCK_HZ % HIRQ_TIMER_COUNTER_HZ == 0U,
               "TIM2 must divide exactly to the stress counter clock");

typedef struct hirq_packet {
    uint32_t source;
    uint32_t sequence;
    uint32_t size;
    uint32_t tag;
    uint8_t bytes[];
} hirq_packet_t;

typedef struct hirq_source_stats {
    _Atomic uint32_t attempts;
    _Atomic uint32_t accepted;
    _Atomic uint32_t no_memory;
    _Atomic uint32_t queue_full;
    _Atomic uint32_t received;
} hirq_source_stats_t;

typedef struct hirq_mask {
    uint32_t basepri;
    uint32_t primask;
    uint32_t nesting;
} hirq_mask_t;

static const uint32_t s_sizes[] = {
    8U, 9U, 15U, 16U, 17U, 31U, 32U, 33U, 63U, 64U, 65U, 127U, 257U, 513U
};
static rtos_queue_t s_queue;
static hirq_packet_t *s_queue_storage[HIRQ_QUEUE_CAPACITY];
static rtos_tcb_t s_tasks[HIRQ_TASKS];
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT)
    rtos_stack_t s_stacks[HIRQ_TASKS][HIRQ_STACK_WORDS];
static hirq_source_stats_t s_sources[HIRQ_SOURCES];
static _Atomic bool s_stop;
static _Atomic bool s_drain;
static _Atomic bool s_load_phase;
static _Atomic bool s_done[HIRQ_TASKS];
static _Atomic bool s_timer_active;
static _Atomic uint32_t s_failure;
static _Atomic uint32_t s_irq_entries;
static _Atomic uint32_t s_nested_entries;
static _Atomic uint32_t s_pressure_holds;
static _Atomic uint32_t s_pressure_refused;
static _Atomic uint32_t s_nested_probes;
static uint32_t s_last_sequence[HIRQ_SOURCES];
static bool s_has_sequence[HIRQ_SOURCES];
static uintptr_t s_arena_begin;
static uintptr_t s_arena_end;
static bool s_irq_owned;
static bool s_timer_clock_was_enabled;
static uint32_t s_timer_priority;
static uint32_t s_nested_priority;

void f446_heap_irq_stress_run(void *argument);
void TIM2_IRQHandler(void);
void EXTI4_IRQHandler(void);

static hirq_mask_t read_mask(void)
{
    return (hirq_mask_t){__get_BASEPRI(), __get_PRIMASK(), rtos_port_get_critical_nesting()};
}

static bool matches_mask(hirq_mask_t expected)
{
    const hirq_mask_t actual = read_mask();
    return actual.basepri == expected.basepri && actual.primask == expected.primask &&
        actual.nesting == expected.nesting;
}

static void record_failure(uint32_t code)
{
    uint32_t expected = 0U;
    (void)atomic_compare_exchange_strong(&s_failure, &expected, code);
    atomic_store(&s_stop, true);
}

static uint8_t packet_byte(uint32_t source, uint32_t sequence, uint32_t index)
{
    return (uint8_t)((source * 47U) ^ (sequence * 71U) ^ (index * 13U) ^ 0xA5U);
}

static uint32_t packet_tag(uint32_t source, uint32_t sequence, uint32_t size)
{
    return HIRQ_PACKET_TAG ^ (source * UINT32_C(0x9E3779B9)) ^ sequence ^ (size << 16U);
}

static void submit_packet(uint32_t source)
{
    hirq_source_stats_t *const stats = &s_sources[source];
    const uint32_t sequence = atomic_load(&stats->attempts);
    const uint32_t index = (sequence * 17U + source * 97U) %
        (sizeof(s_sizes) / sizeof(s_sizes[0]));
    const uint32_t size = s_sizes[index];
    const hirq_mask_t initial = read_mask();
    ++stats->attempts;
    hirq_packet_t *const packet = rtos_heap_alloc((uint32_t)sizeof(*packet) + size);
    if (!matches_mask(initial)) {
        record_failure(10U + source);
    }
    if (packet == NULL) {
        ++stats->no_memory;
        return;
    }

    packet->source = source;
    packet->sequence = sequence;
    packet->size = size;
    packet->tag = packet_tag(source, sequence, size);
    for (uint32_t byte = 0U; byte < size; ++byte) {
        packet->bytes[byte] = packet_byte(source, sequence, byte);
    }
    const rtos_status_t status = rtos_queue_send(&s_queue, &packet, RTOS_NO_WAIT, RTOS_FALSE);
    if (status == RTOS_OK) {
        ++stats->accepted;
        return;
    }
    if (status == RTOS_ERR_TIMEOUT) {
        ++stats->queue_full;
    } else {
        record_failure(20U + source);
    }
    rtos_heap_free(packet);
    if (!matches_mask(initial)) {
        record_failure(30U + source);
    }
}

void EXTI4_IRQHandler(void)
{
    if (!atomic_load(&s_timer_active) ||
        __get_IPSR() != (uint32_t)EXTI4_IRQn + 16U) {
        record_failure(40U);
    }
    ++s_nested_entries;
    submit_packet(HIRQ_NESTED_SOURCE);
}

void TIM2_IRQHandler(void)
{
    if ((TIM2->SR & TIM_SR_UIF) == 0U) {
        return;
    }
    TIM2->SR = 0U;
    (void)TIM2->SR;
    atomic_store(&s_timer_active, true);
    const uint32_t entry = ++s_irq_entries;
    submit_packet(HIRQ_TIMER_SOURCE);
    if ((entry % HIRQ_NEST_INTERVAL) == 0U) {
        const uint32_t before = atomic_load(&s_nested_entries);
        NVIC_SetPendingIRQ(EXTI4_IRQn);
        __DSB();
        __ISB();
        if (atomic_load(&s_nested_entries) != before + 1U) {
            record_failure(41U);
        }
    }
    atomic_store(&s_timer_active, false);
}

static bool check_packet(hirq_packet_t *packet)
{
    const uintptr_t address = (uintptr_t)packet;
    if (address < s_arena_begin || address > s_arena_end - sizeof(*packet) ||
        (address & 7U) != 0U) {
        return false;
    }
    if (packet->source >= HIRQ_SOURCES || packet->size < 8U || packet->size > 513U ||
        packet->size > s_arena_end - address - sizeof(*packet)) {
        return false;
    }
    const uint32_t source = packet->source;
    const uint32_t sequence = packet->sequence;
    if (packet->tag != packet_tag(source, sequence, packet->size) ||
        (s_has_sequence[source] && sequence <= s_last_sequence[source])) {
        return false;
    }
    for (uint32_t byte = 0U; byte < packet->size; ++byte) {
        if (packet->bytes[byte] != packet_byte(source, sequence, byte)) {
            return false;
        }
    }
    s_has_sequence[source] = true;
    s_last_sequence[source] = sequence;
    ++s_sources[source].received;
    return true;
}

static void finish_task(uint32_t id)
{
    atomic_store(&s_done[id], true);
    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}

static void probe_nested_heap(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    RTOS_PORT_ENTER_CRITICAL();
    const hirq_mask_t nested = read_mask();
    uint8_t *const payload = rtos_heap_alloc(33U);
    if (!matches_mask(nested)) {
        record_failure(50U);
    }
    if (payload != NULL) {
        payload[0] = 0x3CU;
        payload[32] = 0xC3U;
        if (payload[0] != 0x3CU || payload[32] != 0xC3U) {
            record_failure(51U);
        }
        rtos_heap_free(payload);
        if (!matches_mask(nested)) {
            record_failure(52U);
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
    RTOS_PORT_EXIT_CRITICAL();
    ++s_nested_probes;
}

static void run_producer(void *argument)
{
    const uint32_t source = (uint32_t)(uintptr_t)argument;
    uint32_t bursts = 0U;
    while (!atomic_load(&s_stop)) {
        for (uint32_t index = 0U; index < HIRQ_BURST; ++index) {
            submit_packet(source);
        }
        if ((bursts % 16U) == 0U) {
            probe_nested_heap();
        }
        ++bursts;
        rtos_task_delay(1U + (source & 1U));
    }
    finish_task(source);
}

static void run_consumer(void *argument)
{
    (void)argument;
    uint32_t received = 0U;
    for (;;) {
        hirq_packet_t *packet = NULL;
        const rtos_status_t status = rtos_queue_recv(&s_queue, &packet, 1U);
        if (status == RTOS_OK) {
            const hirq_mask_t initial = read_mask();
            if (!check_packet(packet)) {
                record_failure(60U);
            }
            rtos_heap_free(packet);
            if (!matches_mask(initial)) {
                record_failure(61U);
            }
            ++received;
            if (atomic_load(&s_load_phase) && (received % 8U) == 0U &&
                !atomic_load(&s_drain)) {
                rtos_task_delay(2U);
            }
        } else if (status != RTOS_ERR_TIMEOUT) {
            record_failure(62U);
        }
        if (atomic_load(&s_drain) && rtos_queue_get_count(&s_queue) == 0U) {
            break;
        }
    }
    finish_task(HIRQ_CONSUMER_TASK);
}

static void run_pressure(void *argument)
{
    (void)argument;
    while (!atomic_load(&s_stop) && !atomic_load(&s_load_phase)) {
        rtos_task_delay(1U);
    }
    while (!atomic_load(&s_stop)) {
        const uint32_t size = rtos_heap_get_max_alloc();
        uint8_t *const held = rtos_heap_alloc(size);
        if (held != NULL) {
            (void)memset(held, 0x96, size);
            ++s_pressure_holds;
            rtos_task_delay(2U);
            for (uint32_t byte = 0U; byte < size; ++byte) {
                if (held[byte] != 0x96U) {
                    record_failure(70U);
                    break;
                }
            }
            rtos_heap_free(held);
        } else {
            ++s_pressure_refused;
        }
        rtos_task_delay(1U);
    }
    finish_task(HIRQ_PRESSURE_TASK);
}

static void stop_irq_sources(void)
{
    if (!s_irq_owned) {
        return;
    }
    if ((PRODUCTION_TIMER_APB_ENABLE & PRODUCTION_TIMER_TIM2_ENABLE) != 0U) {
        TIM2->DIER = 0U;
        TIM2->CR1 = 0U;
    }
    NVIC_DisableIRQ(TIM2_IRQn);
    NVIC_DisableIRQ(EXTI4_IRQn);
    NVIC_ClearPendingIRQ(TIM2_IRQn);
    NVIC_ClearPendingIRQ(EXTI4_IRQn);
    __DSB();
    __ISB();
}

static bool start_irq_sources(void)
{
    const uint32_t shift = 8U - __NVIC_PRIO_BITS;
    const uint32_t syscall_priority = RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY >> shift;
    if (!PRODUCTION_TIMER_PROFILE_VALID() || syscall_priority + 1U >= (1U << __NVIC_PRIO_BITS) ||
        NVIC_GetEnableIRQ(TIM2_IRQn) != 0U || NVIC_GetEnableIRQ(EXTI4_IRQn) != 0U ||
        NVIC_GetPendingIRQ(TIM2_IRQn) != 0U || NVIC_GetPendingIRQ(EXTI4_IRQn) != 0U) {
        return false;
    }
    s_timer_clock_was_enabled =
        (PRODUCTION_TIMER_APB_ENABLE & PRODUCTION_TIMER_TIM2_ENABLE) != 0U;
    s_timer_priority = NVIC_GetPriority(TIM2_IRQn);
    s_nested_priority = NVIC_GetPriority(EXTI4_IRQn);
    s_irq_owned = true;
    PRODUCTION_TIMER_APB_ENABLE |= PRODUCTION_TIMER_TIM2_ENABLE;
    (void)PRODUCTION_TIMER_APB_ENABLE;
    stop_irq_sources();
    TIM2->PSC = (PRODUCTION_TIMER_CLOCK_HZ / HIRQ_TIMER_COUNTER_HZ) - 1U;
    TIM2->ARR = HIRQ_TIMER_PERIOD_US - 1U;
    TIM2->EGR = TIM_EGR_UG;
    for (uint32_t attempt = 0U; attempt < 10000U && (TIM2->SR & TIM_SR_UIF) == 0U; ++attempt) {
    }
    if ((TIM2->SR & TIM_SR_UIF) == 0U) {
        return false;
    }
    TIM2->SR = 0U;
    NVIC_SetPriority(TIM2_IRQn, syscall_priority + 1U);
    NVIC_SetPriority(EXTI4_IRQn, syscall_priority);
    NVIC_EnableIRQ(EXTI4_IRQn);
    NVIC_EnableIRQ(TIM2_IRQn);
    TIM2->DIER = TIM_DIER_UIE;
    TIM2->CR1 = TIM_CR1_CEN;
    return true;
}

static bool tasks_done(bool include_consumer, uint32_t created)
{
    for (uint32_t index = 0U; index < created; ++index) {
        if (index == HIRQ_CONSUMER_TASK && !include_consumer) {
            continue;
        }
        if (!atomic_load(&s_done[index])) {
            return false;
        }
    }
    return true;
}

static bool wait_tasks(bool include_consumer, uint32_t created)
{
    const rtos_tick_t start = rtos_sched_get_tick_count();
    while (!tasks_done(include_consumer, created)) {
        if (rtos_sched_get_tick_count() - start >= HIRQ_SHUTDOWN_TICKS) {
            return false;
        }
        rtos_task_delay(1U);
    }
    return true;
}

static bool start_tasks(uint32_t *created)
{
    static const char *const names[HIRQ_TASKS] = {"hprod0", "hprod1", "hconsume", "hpressure"};
    static const rtos_prio_t priorities[HIRQ_TASKS] = {9U, 10U, 6U, 12U};
    static const rtos_task_func_t entries[HIRQ_TASKS] = {
        run_producer, run_producer, run_consumer, run_pressure
    };
    for (uint32_t index = 0U; index < HIRQ_TASKS; ++index) {
        const rtos_status_t status = rtos_task_create_static(&s_tasks[index], s_stacks[index],
            HIRQ_STACK_WORDS, entries[index], (void *)(uintptr_t)index,
            priorities[index], names[index]);
        if (status != RTOS_OK) {
            return false;
        }
        ++*created;
    }
    return true;
}

static uint32_t print_sources(void)
{
    uint32_t accepted = 0U;
    for (uint32_t source = 0U; source < HIRQ_SOURCES; ++source) {
        const hirq_source_stats_t *const stats = &s_sources[source];
        const uint32_t attempts = atomic_load(&stats->attempts);
        const uint32_t sent = atomic_load(&stats->accepted);
        const uint32_t no_memory = atomic_load(&stats->no_memory);
        const uint32_t queue_full = atomic_load(&stats->queue_full);
        const uint32_t received = atomic_load(&stats->received);
        if (sent == 0U || sent != received || attempts != sent + no_memory + queue_full) {
            record_failure(80U + source);
        }
        accepted += sent;
        (void)printf("HBIRQ|SOURCE|id=%" PRIu32 "|attempts=%" PRIu32 "|accepted=%" PRIu32
                     "|no_memory=%" PRIu32 "|queue_full=%" PRIu32 "|received=%" PRIu32 "\r\n",
                     source, attempts, sent, no_memory, queue_full, received);
    }
    return accepted;
}

void f446_heap_irq_stress_run(void *argument)
{
    (void)argument;
    const hirq_mask_t clean = {0U, 0U, 0U};
    const uint32_t capacity = rtos_heap_get_max_alloc();
    uint8_t *const arena = rtos_heap_alloc(capacity);
    if (arena == NULL || capacity < 4096U || !matches_mask(clean)) {
        rtos_heap_free(arena);
        (void)printf("HBIRQ|DONE|status=1|failure=1\r\n");
        return;
    }
    s_arena_begin = (uintptr_t)arena;
    s_arena_end = (uintptr_t)arena + capacity;
    rtos_heap_free(arena);
    const uint32_t baseline_free = rtos_heap_get_free();
    const uint32_t allocations = rtos_heap_get_alloc_count();
    const uint32_t releases = rtos_heap_get_free_count();
    uint32_t created = 0U;
    const bool is_queue_initialized = rtos_queue_init(&s_queue, s_queue_storage,
        (uint32_t)sizeof(s_queue_storage[0]), HIRQ_QUEUE_CAPACITY) == RTOS_OK;
    bool started = is_queue_initialized;
    if (started) {
        started = start_tasks(&created);
    }
    if (started) {
        started = start_irq_sources();
    }
    if (!started) {
        record_failure(2U);
    }
    (void)printf("HBIRQ|START|seconds=%u|capacity=%" PRIu32 "|tasks=%" PRIu32
                 "|basepri=%u|irq_period_us=%u\r\n", HIRQ_SECONDS, capacity, created,
                 (unsigned int)RTOS_CONFIG_USE_BASEPRI, HIRQ_TIMER_PERIOD_US);
    const rtos_tick_t start = rtos_sched_get_tick_count();
    while (started && !atomic_load(&s_stop) &&
           rtos_sched_get_tick_count() - start < HIRQ_SECONDS * RTOS_CONFIG_TICK_RATE_HZ) {
        const rtos_tick_t elapsed = rtos_sched_get_tick_count() - start;
        if (!atomic_load(&s_load_phase) &&
            elapsed >= HIRQ_WARMUP_SECONDS * RTOS_CONFIG_TICK_RATE_HZ) {
            atomic_store(&s_load_phase, true);
            (void)printf("HBIRQ|PHASE|load=1|ticks=%" PRIu32 "\r\n", (uint32_t)elapsed);
        }
        rtos_task_delay(10U);
    }
    stop_irq_sources();
    atomic_store(&s_stop, true);
    if (!wait_tasks(false, created)) {
        record_failure(3U);
    }
    atomic_store(&s_drain, true);
    if (!wait_tasks(true, created)) {
        record_failure(4U);
    }
    for (uint32_t index = 0U; index < created; ++index) {
        if (rtos_task_delete(&s_tasks[index]) != RTOS_OK) {
            record_failure(5U);
        }
    }
    if (is_queue_initialized) {
        if (rtos_queue_get_count(&s_queue) != 0U) {
            record_failure(6U);
        }
        if (rtos_queue_deinit(&s_queue) != RTOS_OK) {
            record_failure(6U);
        }
    }
    if (s_irq_owned) {
        NVIC_SetPriority(TIM2_IRQn, s_timer_priority);
        NVIC_SetPriority(EXTI4_IRQn, s_nested_priority);
        if (!s_timer_clock_was_enabled) {
            PRODUCTION_TIMER_APB_ENABLE &= ~PRODUCTION_TIMER_TIM2_ENABLE;
        }
    }

    const uint32_t alloc_delta = rtos_heap_get_alloc_count() - allocations;
    const uint32_t free_delta = rtos_heap_get_free_count() - releases;
    const uint32_t raw_free = rtos_heap_get_free();
    const uint32_t end_max = rtos_heap_get_max_alloc();
    void *const recovered = rtos_heap_alloc(capacity);
    if (recovered != NULL) {
        rtos_heap_free(recovered);
    }
    const uint32_t final_free = rtos_heap_get_free();
    if (alloc_delta != free_delta || end_max != capacity || recovered == NULL ||
        final_free != baseline_free || !matches_mask(clean)) {
        record_failure(7U);
    }
    if (!atomic_load(&s_load_phase) || atomic_load(&s_irq_entries) < HIRQ_SECONDS * 100U ||
        atomic_load(&s_nested_entries) != atomic_load(&s_irq_entries) / HIRQ_NEST_INTERVAL ||
        atomic_load(&s_pressure_holds) == 0U || atomic_load(&s_nested_probes) == 0U) {
        record_failure(8U);
    }
    const uint32_t accepted = print_sources();
    uint32_t no_memory = 0U;
    uint32_t queue_full = 0U;
    for (uint32_t source = 0U; source < HIRQ_SOURCES; ++source) {
        no_memory += atomic_load(&s_sources[source].no_memory);
        queue_full += atomic_load(&s_sources[source].queue_full);
    }
    if (no_memory == 0U || queue_full == 0U) {
        record_failure(9U);
    }
    (void)printf("HBIRQ|TOTAL|accepted=%" PRIu32 "|no_memory=%" PRIu32
                 "|queue_full=%" PRIu32 "|irq=%" PRIu32 "|nested=%" PRIu32
                 "|pressure_holds=%" PRIu32 "|pressure_refused=%" PRIu32
                 "|probes=%" PRIu32 "|load_phase=%u|alloc_delta=%" PRIu32 "|free_delta=%" PRIu32
                 "|raw_free=%" PRIu32 "|max_alloc=%" PRIu32 "|final_free=%" PRIu32 "\r\n",
                 accepted, no_memory, queue_full, (uint32_t)atomic_load(&s_irq_entries),
                 (uint32_t)atomic_load(&s_nested_entries),
                 (uint32_t)atomic_load(&s_pressure_holds),
                 (uint32_t)atomic_load(&s_pressure_refused),
                 (uint32_t)atomic_load(&s_nested_probes),
                 (unsigned int)atomic_load(&s_load_phase), alloc_delta, free_delta,
                 raw_free, end_max, final_free);
    (void)printf("HBIRQ|DONE|status=%u|failure=%" PRIu32 "\r\n",
                 (unsigned int)(atomic_load(&s_failure) != 0U),
                 (uint32_t)atomic_load(&s_failure));
}
