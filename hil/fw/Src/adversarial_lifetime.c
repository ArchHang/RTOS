/**
 * @file adversarial_lifetime.c
 * @brief Real-IRQ object deinitialization between access checks and exclusion.
 *
 * This fixture tests a hypothesis, not an assumed kernel defect. Each private,
 * static object is initialized in thread context. A one-shot access hook pends
 * priority-five TIM7 after the API's first liveness check but before exclusion.
 * The IRQ deinitializes the same object and snapshots its meaningful fields.
 * The resumed API must reject access without changing that snapshot or output.
 * Storage is never freed, so stale-object behavior is observed without a UAF.
 * All waits are finite; no test requires a dead object to be waited on forever.
 */
#include "audit_regressions.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx_hal.h"

#include "rtos.h"

#if !defined(FIRMWARE_ADVERSARIAL_LIFETIME) || !defined(RTOS_HIL_OBJECT_LIFETIME)
#error "adversarial_lifetime.c requires its dedicated HIL lifetime variant"
#endif

#ifndef OBJECT_LIFETIME_ROUNDS
#define OBJECT_LIFETIME_ROUNDS (8U)
#endif

#define OF_CASES_PER_ROUND (10U)
#define OF_SNAPSHOT_FIELDS (10U)
#define OF_OUTPUT_SENTINEL (0xA5C35AA5U)
#define OF_STORAGE_SENTINEL (0x9BDF2468U)
#define OF_MESSAGE (0x13579BDFU)
#define OF_WAIT_TICKS (2U)

typedef enum of_kind {
    OF_KIND_SEM = 0,
    OF_KIND_QUEUE,
    OF_KIND_EVENT,
    OF_KIND_MUTEX
} of_kind_t;

typedef struct of_case {
    const char *name;
    of_kind_t kind;
    uint32_t site;
    uint32_t initial_value;
    uint32_t is_waiting_variant;
} of_case_t;

typedef struct of_snapshot {
    uint32_t values[OF_SNAPSHOT_FIELDS];
} of_snapshot_t;

static const of_case_t case_definitions[OF_CASES_PER_ROUND] = {
    {"sem_give", OF_KIND_SEM, 1U, 0U, 0U},
    {"sem_take", OF_KIND_SEM, 2U, 1U, 0U},
    {"queue_send", OF_KIND_QUEUE, 3U, 0U, 0U},
    {"queue_recv", OF_KIND_QUEUE, 4U, 1U, 0U},
    {"event_set", OF_KIND_EVENT, 5U, 0U, 0U},
    {"event_clear", OF_KIND_EVENT, 6U, 3U, 0U},
    {"event_wait_ready", OF_KIND_EVENT, 7U, 1U, 0U},
    {"mutex_take", OF_KIND_MUTEX, 8U, 0U, 0U},
    {"mutex_give", OF_KIND_MUTEX, 9U, 1U, 0U},
    {"event_wait_empty", OF_KIND_EVENT, 7U, 0U, 1U}
};

static rtos_sem_t semaphore;
static rtos_queue_t queue;
static uint32_t queue_storage;
static rtos_event_t event;
static rtos_mutex_t mutex;
static const of_case_t *active_case;
static const void *armed_object;
static volatile uint32_t armed_site;
static volatile uint32_t hook_count;
static volatile uint32_t irq_count;
static volatile uint32_t irq_inside_hook;
static volatile uint32_t irq_ipsr;
static volatile uint32_t irq_priority;
static volatile uint32_t irq_basepri;
static volatile uint32_t irq_primask;
static volatile int32_t irq_status;
static of_snapshot_t irq_snapshot;
static uint32_t saved_irq_priority;
static uint32_t irq_owned;
static uint32_t pass_count;
static uint32_t fail_count;
static uint32_t executed_cases;

static const void *case_object(of_kind_t kind)
{
    switch (kind) {
        case OF_KIND_SEM:
            return &semaphore;
        case OF_KIND_QUEUE:
            return &queue;
        case OF_KIND_EVENT:
            return &event;
        case OF_KIND_MUTEX:
            return &mutex;
        default:
            return NULL;
    }
}

static void capture_snapshot(of_kind_t kind, of_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    switch (kind) {
        case OF_KIND_SEM:
            snapshot->values[0] = semaphore.is_initialized;
            snapshot->values[1] = semaphore.count;
            snapshot->values[2] = semaphore.max_count;
            snapshot->values[3] = (uint32_t)(uintptr_t)semaphore.wait_head;
            snapshot->values[4] = semaphore.wait_bitmap;
            break;
        case OF_KIND_QUEUE:
            snapshot->values[0] = queue.is_initialized;
            snapshot->values[1] = queue.count;
            snapshot->values[2] = queue.head;
            snapshot->values[3] = queue.tail;
            snapshot->values[4] = queue.capacity;
            snapshot->values[5] = queue.item_size;
            snapshot->values[6] = (uint32_t)(uintptr_t)queue.storage;
            snapshot->values[7] = (uint32_t)(uintptr_t)queue.send_wait_head;
            snapshot->values[8] = (uint32_t)(uintptr_t)queue.recv_wait_head;
            snapshot->values[9] = queue_storage;
            break;
        case OF_KIND_EVENT:
            snapshot->values[0] = event.is_initialized;
            snapshot->values[1] = event.bits;
            snapshot->values[2] = (uint32_t)(uintptr_t)event.wait_head;
            break;
        case OF_KIND_MUTEX:
            snapshot->values[0] = mutex.is_initialized;
            snapshot->values[1] = (uint32_t)(uintptr_t)mutex.owner;
            snapshot->values[2] = mutex.recursion_count;
            snapshot->values[3] = (uint32_t)(uintptr_t)mutex.wait_head;
            break;
        default:
            snapshot->values[0] = UINT32_MAX;
            break;
    }
}

static rtos_status_t prepare_case(const of_case_t *definition)
{
    rtos_status_t status;

    switch (definition->kind) {
        case OF_KIND_SEM:
            return rtos_sem_init(&semaphore, definition->initial_value, 2U);
        case OF_KIND_QUEUE:
            queue_storage = OF_STORAGE_SENTINEL;
            status = rtos_queue_init(&queue, &queue_storage, sizeof(queue_storage), 1U);
            if ((status == RTOS_OK) && (definition->initial_value != 0U)) {
                uint32_t message = OF_MESSAGE;
                status = rtos_queue_send(&queue, &message, RTOS_NO_WAIT, RTOS_FALSE);
            }
            return status;
        case OF_KIND_EVENT:
            status = rtos_event_init(&event);
            if ((status == RTOS_OK) && (definition->initial_value != 0U)) {
                uint32_t bits = rtos_event_set(&event, definition->initial_value);
                if (bits != definition->initial_value) {
                    return RTOS_ERR_PARAM;
                }
            }
            return status;
        case OF_KIND_MUTEX:
            status = rtos_mutex_init(&mutex);
            if ((status == RTOS_OK) && (definition->initial_value != 0U)) {
                status = rtos_mutex_take(&mutex, RTOS_NO_WAIT);
            }
            return status;
        default:
            return RTOS_ERR_PARAM;
    }
}

static int32_t execute_case(const of_case_t *definition, uint32_t *output)
{
    uint32_t message = OF_MESSAGE;

    switch (definition->site) {
        case 1U:
            return (int32_t)rtos_sem_give(&semaphore);
        case 2U:
            return (int32_t)rtos_sem_take(&semaphore, OF_WAIT_TICKS);
        case 3U:
            return (int32_t)rtos_queue_send(&queue, &message, OF_WAIT_TICKS, RTOS_FALSE);
        case 4U:
            return (int32_t)rtos_queue_recv(&queue, output, OF_WAIT_TICKS);
        case 5U:
            return (int32_t)rtos_event_set(&event, 1U);
        case 6U:
            return (int32_t)rtos_event_clear(&event, 1U);
        case 7U:
            return (int32_t)rtos_event_wait(&event, 1U,
                RTOS_EVENT_WAIT_ANY | RTOS_EVENT_CLEAR_ON_EXIT, OF_WAIT_TICKS);
        case 8U:
            return (int32_t)rtos_mutex_take(&mutex, OF_WAIT_TICKS);
        case 9U:
            return (int32_t)rtos_mutex_give(&mutex);
        default:
            return (int32_t)RTOS_ERR_PARAM;
    }
}

/** @brief Inject the legal deinit IRQ once at the requested API boundary.
 *  @param object [in] Object whose unchecked access is about to enter exclusion.
 *  @param site [in] Kernel-assigned access site in the range 1 through 9.
 *  @return No result; observations are emitted after the outer API returns. */
void rtos_hil_object_access_hook(const void *object, uint32_t site)
{
    if ((armed_site == 0U) || (site != armed_site) || (object != armed_object)
        || (__get_IPSR() != 0U)) {
        return;
    }
    armed_site = 0U;
    hook_count++;
    NVIC_SetPendingIRQ(TIM7_IRQn);
    __DSB();
    __ISB();
    irq_inside_hook = irq_count;
}

/** @brief Deinitialize and observe the selected static object in a real IRQ. */
void TIM7_IRQHandler(void)
{
    irq_ipsr = __get_IPSR();
    irq_priority = NVIC_GetPriority(TIM7_IRQn);
    irq_basepri = __get_BASEPRI();
    irq_primask = __get_PRIMASK();
    switch (active_case->kind) {
        case OF_KIND_SEM:
            irq_status = (int32_t)rtos_sem_deinit(&semaphore);
            break;
        case OF_KIND_QUEUE:
            irq_status = (int32_t)rtos_queue_deinit(&queue);
            break;
        case OF_KIND_EVENT:
            irq_status = (int32_t)rtos_event_deinit(&event);
            break;
        case OF_KIND_MUTEX:
            irq_status = (int32_t)rtos_mutex_deinit(&mutex);
            break;
        default:
            irq_status = (int32_t)RTOS_ERR_PARAM;
            break;
    }
    capture_snapshot(active_case->kind, &irq_snapshot);
    irq_count++;
}

static uint32_t acquire_irq(void)
{
    if ((NVIC_GetEnableIRQ(TIM7_IRQn) != 0U) || (NVIC_GetPendingIRQ(TIM7_IRQn) != 0U)
        || (NVIC_GetActive(TIM7_IRQn) != 0U)) {
        return 0U;
    }
    saved_irq_priority = NVIC_GetPriority(TIM7_IRQn);
    NVIC_SetPriority(TIM7_IRQn, 5U);
    NVIC_EnableIRQ(TIM7_IRQn);
    irq_owned = 1U;
    return 1U;
}

static void arm_case(const of_case_t *definition)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    hook_count = 0U;
    irq_count = 0U;
    irq_inside_hook = 0U;
    irq_ipsr = 0U;
    irq_priority = UINT32_MAX;
    irq_basepri = UINT32_MAX;
    irq_primask = UINT32_MAX;
    irq_status = (int32_t)RTOS_ERR_PARAM;
    active_case = definition;
    armed_object = case_object(definition->kind);
    armed_site = definition->site;
    __set_PRIMASK(primask);
    __ISB();
}

static uint32_t mismatch_fields(const of_snapshot_t *after)
{
    uint32_t mismatch = 0U;

    for (uint32_t index = 0U; index < OF_SNAPSHOT_FIELDS; index++) {
        if (after->values[index] != irq_snapshot.values[index]) {
            mismatch |= 1U << index;
        }
    }
    return mismatch;
}

static void run_case(const of_case_t *definition, uint32_t round)
{
    of_snapshot_t after;
    uint32_t output = OF_OUTPUT_SENTINEL;
    rtos_status_t prepared = prepare_case(definition);
    uint32_t started = rtos_sched_get_tick_count();

    arm_case(definition);
    int32_t status = execute_case(definition, &output);
    uint32_t elapsed = (uint32_t)(rtos_sched_get_tick_count() - started);
    armed_site = 0U;
    capture_snapshot(definition->kind, &after);
    uint32_t mismatch = mismatch_fields(&after);
    int32_t expected = definition->kind == OF_KIND_EVENT ? 0 : (int32_t)RTOS_ERR_PARAM;
    uint32_t mask = __get_BASEPRI() | __get_PRIMASK() | rtos_port_get_critical_nesting();
    uint32_t is_valid = (prepared == RTOS_OK) && (status == expected)
        && (output == OF_OUTPUT_SENTINEL) && (mismatch == 0U) && (mask == 0U)
        && (irq_snapshot.values[0] == 0U)
        && (hook_count == 1U) && (irq_count == 1U) && (irq_inside_hook == 1U)
        && (irq_status == RTOS_OK) && (irq_ipsr == (uint32_t)TIM7_IRQn + 16U)
        && (irq_priority == 5U) && (irq_basepri == 0U) && (irq_primask == 0U)
        && ((definition->is_waiting_variant == 0U) || (elapsed < OF_WAIT_TICKS));

    executed_cases++;
    if (is_valid != 0U) {
        pass_count++;
    } else {
        fail_count++;
    }
    (void)printf("OF|CASE|%s|id=%lu round=%lu name=%s site=%lu prep=%ld status=%ld "
        "expected=%ld elapsed=%lu mismatch=0x%lX output=0x%08lX mask=0x%lX "
        "hook=%lu irq=%lu inside=%lu irq_status=%ld dead_init=%lu ipsr=%lu priority=%lu "
        "irq_bp=0x%lX irq_pm=%lu\r\n", is_valid != 0U ? "PASS" : "FAIL",
        (unsigned long)executed_cases, (unsigned long)round, definition->name,
        (unsigned long)definition->site, (long)prepared, (long)status, (long)expected,
        (unsigned long)elapsed, (unsigned long)mismatch, (unsigned long)output,
        (unsigned long)mask, (unsigned long)hook_count, (unsigned long)irq_count,
        (unsigned long)irq_inside_hook, (long)irq_status,
        (unsigned long)irq_snapshot.values[0], (unsigned long)irq_ipsr,
        (unsigned long)irq_priority, (unsigned long)irq_basepri, (unsigned long)irq_primask);
    if (mismatch != 0U) {
        for (uint32_t index = 0U; index < OF_SNAPSHOT_FIELDS; index++) {
            if ((mismatch & (1U << index)) != 0U) {
                (void)printf("OF|DIFF|id=%lu field=%lu snapshot=0x%08lX after=0x%08lX\r\n",
                    (unsigned long)executed_cases, (unsigned long)index,
                    (unsigned long)irq_snapshot.values[index], (unsigned long)after.values[index]);
            }
        }
    }
}

/** @brief Describe test provenance without executing any unsupported ISR use. */
void audit_pre_start(void)
{
    (void)printf("OF|META|rounds=%lu cases_per_round=%lu real_irq=TIM7 priority=5 "
        "static_storage=1 timeout_ticks=%lu hypothesis_requires_measurement=1\r\n",
        (unsigned long)OBJECT_LIFETIME_ROUNDS, (unsigned long)OF_CASES_PER_ROUND,
        (unsigned long)OF_WAIT_TICKS);
}

/** @brief Execute finite object-lifetime access checks and emit OF|DONE.
 *  @param arg [in] Unused task argument; may be NULL.
 *  @return Never returns; waits after emitting the complete result. */
void audit_regressions_task(void *arg)
{
    (void)arg;
    if (acquire_irq() == 0U) {
        fail_count++;
        (void)printf("OF|CHECK|FAIL|irq_ownership\r\n");
    } else {
        for (uint32_t round = 0U; round < OBJECT_LIFETIME_ROUNDS; round++) {
            for (uint32_t index = 0U; index < OF_CASES_PER_ROUND; index++) {
                run_case(&case_definitions[index], round);
            }
        }
    }
    armed_site = 0U;
    if (irq_owned != 0U) {
        NVIC_DisableIRQ(TIM7_IRQn);
        NVIC_ClearPendingIRQ(TIM7_IRQn);
        NVIC_SetPriority(TIM7_IRQn, saved_irq_priority);
    }
    if (executed_cases != OBJECT_LIFETIME_ROUNDS * OF_CASES_PER_ROUND) {
        fail_count++;
    }
    (void)printf("OF|SUMMARY|pass=%lu fail=%lu cases=%lu\r\n", (unsigned long)pass_count,
        (unsigned long)fail_count, (unsigned long)executed_cases);
    (void)printf("OF|DONE|status=%lu\r\n", fail_count == 0U ? 0UL : 1UL);
    for (;;) {
        rtos_task_delay(1000U);
    }
}
