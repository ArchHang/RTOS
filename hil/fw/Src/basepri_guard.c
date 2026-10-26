/**
 * @file basepri_guard.c
 * @brief Real-IRQ tests for syscall priority contracts and invalid nesting.
 *
 * Every image executes one case. A forbidden call must stop at its own priority
 * assertion before touching shared state. The linker wraps the assertion sink
 * only in this fixture; it prints the expected terminal result and never returns.
 * This tests software API contracts without attaching a USB peripheral.
 */
#include "audit_regressions.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx_hal.h"

#include "rtos.h"
#include "rtos_usb.h"

#ifndef BASEPRI_GUARD_CASE
#define BASEPRI_GUARD_CASE (1U)
#endif
#ifndef BASEPRI_GUARD_PRIORITY
#define BASEPRI_GUARD_PRIORITY (0U)
#endif

#if !RTOS_CONFIG_ASSERT_ENABLED
#error "basepri_guard tests require assertions enabled"
#endif

static rtos_sem_t guard_sem;
static rtos_queue_t guard_queue;
static rtos_event_t guard_event;
static uint32_t queue_storage[2];
static rtos_tcb_t *controller;
static void *heap_to_free;
static rtos_usb_sem_t *usb_sem;
static rtos_perf_system_t system_stats;
static rtos_perf_memory_t memory_stats;
static rtos_perf_task_t task_stats[2];
static volatile uint32_t irq_completed;
static volatile uint32_t operation_ok;
static uint32_t usb_initialized;

static const char *expected_module(void)
{
    static const char *const modules[] = {
        "", "rtos_sem.c", "rtos_queue.c", "rtos_event.c", "rtos_task.c",
        "rtos_heap.c", "rtos_heap.c", "rtos_heap.c", "rtos_perf.c",
        "rtos_perf.c", "rtos_perf.c", "rtos_perf.c", "rtos_perf.c",
        "rtos_usb.c", "rtos_usb.c", "port.c", "port.c"
    };

    return modules[BASEPRI_GUARD_CASE];
}

__attribute__((noreturn)) static void finish(uint32_t is_valid, const char *reason)
{
    (void)printf("BP|GUARD|%s|case=%lu prio=%lu reason=%s\r\n",
        is_valid != 0U ? "PASS" : "FAIL", (unsigned long)BASEPRI_GUARD_CASE,
        (unsigned long)BASEPRI_GUARD_PRIORITY, reason);
    (void)printf("BP|DONE|status=%lu\r\n", is_valid != 0U ? 0UL : 1UL);
    for (;;) {
        /* Expected assertion images deliberately stop; the next run resets. */
        __NOP();
    }
}

/** @brief Test-only assertion sink; never resumes an asserted kernel operation.
 *  @param condition [in] Failed production condition.
 *  @param file [in] Production source file containing the assertion.
 *  @param line [in] Production source line.
 *  @return Never returns; emits an independently checked terminal result. */
void __wrap_rtos_assert_fail(const char *condition, const char *file, int line)
{
    uint32_t is_priority_case = BASEPRI_GUARD_CASE <= 14U;
    uint32_t expected_assert = (BASEPRI_GUARD_PRIORITY < 5U) || !is_priority_case;
    uint32_t condition_matches = condition != NULL;

    if (condition_matches != 0U) {
        condition_matches = is_priority_case != 0U
            ? strstr(condition, "rtos_port_isr_priority") != NULL
            : strstr(condition, "nesting") != NULL;
    }
    uint32_t is_valid = (expected_assert != 0U) && (condition_matches != 0U)
        && (file != NULL) && (strstr(file, expected_module()) != NULL)
        && (__get_IPSR() == (uint32_t)TIM7_IRQn + 16U);

    (void)printf("BP|ASSERT|case=%lu ipsr=%lu file=%s line=%d condition=%s\r\n",
        (unsigned long)BASEPRI_GUARD_CASE, (unsigned long)__get_IPSR(),
        file != NULL ? file : "?", line, condition != NULL ? condition : "?");
    finish(is_valid, "assertion_source_and_condition_checked");
}

static uint32_t call_ipc_api(void)
{
    uint32_t value = 0x12345678U;

    switch (BASEPRI_GUARD_CASE) {
        case 1U:
            return rtos_sem_give(&guard_sem) == RTOS_OK;
        case 2U:
            return rtos_queue_send(&guard_queue, &value, RTOS_NO_WAIT, RTOS_FALSE) == RTOS_OK;
        case 3U:
            return (rtos_event_set(&guard_event, 1U) & 1U) != 0U;
        case 4U:
            return rtos_task_notify(controller, value, RTOS_NOTIFY_VALUE) == RTOS_OK;
        default:
            return 0U;
    }
}

static uint32_t call_memory_api(void)
{
    switch (BASEPRI_GUARD_CASE) {
        case 5U: {
            void *allocation = rtos_heap_alloc(8U);
            if (allocation != NULL) {
                rtos_heap_free(allocation);
                return 1U;
            }
            return 0U;
        }
        case 6U:
            rtos_heap_free(heap_to_free);
            heap_to_free = NULL;
            return 1U;
        case 7U:
            return rtos_heap_get_max_alloc() != 0U;
        default:
            return 0U;
    }
}

static uint32_t call_monitor_api(void)
{
    uint32_t actual_count = 0U;

    switch (BASEPRI_GUARD_CASE) {
        case 8U:
            return rtos_perf_get_system(&system_stats) == RTOS_OK;
        case 9U:
            return rtos_perf_get_memory(&memory_stats) == RTOS_OK;
        case 10U:
            return rtos_perf_get_task(controller, &task_stats[0]) == RTOS_OK;
        case 11U:
            return (rtos_perf_get_all_tasks(task_stats, 2U, &actual_count) == RTOS_OK)
                && (actual_count != 0U);
        case 12U:
            rtos_perf_reset();
            return 1U;
        default:
            return 0U;
    }
}

static uint32_t call_other_api(void)
{
    switch (BASEPRI_GUARD_CASE) {
        case 13U:
            return rtos_usb_cdc_available(0U) == 0U;
        case 14U:
            rtos_usb_sem_reset(usb_sem);
            return rtos_sem_get_count(&usb_sem->sem) == 0U;
        case 15U:
            RTOS_PORT_EXIT_CRITICAL();
            return 0U;
        case 16U:
            rtos_port_crit_nest = UINT32_MAX;
            RTOS_PORT_ENTER_CRITICAL();
            return 0U;
        default:
            return 0U;
    }
}

/** @brief Execute exactly one API from the real CMSIS TIM7 interrupt vector. */
void TIM7_IRQHandler(void)
{
    if (BASEPRI_GUARD_CASE <= 4U) {
        operation_ok = call_ipc_api();
    } else if (BASEPRI_GUARD_CASE <= 7U) {
        operation_ok = call_memory_api();
    } else if (BASEPRI_GUARD_CASE <= 12U) {
        operation_ok = call_monitor_api();
    } else {
        operation_ok = call_other_api();
    }
    irq_completed = 1U;
}

void audit_pre_start(void)
{
    _Static_assert(BASEPRI_GUARD_CASE >= 1U && BASEPRI_GUARD_CASE <= 16U,
        "unknown priority-contract case");
    _Static_assert(BASEPRI_GUARD_PRIORITY <= 15U, "invalid IRQ priority");
    usb_initialized = rtos_usb_init() == RTOS_OK;
}

static uint32_t prepare_fixture(void)
{
    if ((rtos_sem_init(&guard_sem, 0U, 1U) != RTOS_OK)
        || (rtos_queue_init(&guard_queue, queue_storage, sizeof(uint32_t), 2U) != RTOS_OK)
        || (rtos_event_init(&guard_event) != RTOS_OK)) {
        return 0U;
    }
    usb_sem = rtos_usb_sem_create(1U, 1U);
    heap_to_free = rtos_heap_alloc(8U);
    controller = rtos_task_get_current();
    return (usb_initialized != 0U) && (usb_sem != NULL)
        && (heap_to_free != NULL) && (controller != NULL)
        && (NVIC_GetEnableIRQ(TIM7_IRQn) == 0U)
        && (NVIC_GetPendingIRQ(TIM7_IRQn) == 0U);
}

void audit_regressions_task(void *arg)
{
    (void)arg;
    if (prepare_fixture() == 0U) {
        finish(0U, "fixture_initialization");
    }
    uint32_t saved_priority = NVIC_GetPriority(TIM7_IRQn);
    uint32_t started = DWT->CYCCNT;

    NVIC_SetPriority(TIM7_IRQn, BASEPRI_GUARD_PRIORITY);
    NVIC_EnableIRQ(TIM7_IRQn);
    (void)printf("BP|GUARD_ARM|case=%lu prio=%lu\r\n",
        (unsigned long)BASEPRI_GUARD_CASE, (unsigned long)BASEPRI_GUARD_PRIORITY);
    NVIC_SetPendingIRQ(TIM7_IRQn);
    __DSB();
    __ISB();
    while ((irq_completed == 0U) && ((uint32_t)(DWT->CYCCNT - started) < SystemCoreClock)) {
        /* The DWT bound also works if an unexpected mask stops SysTick. */
        __NOP();
    }
    NVIC_DisableIRQ(TIM7_IRQn);
    NVIC_ClearPendingIRQ(TIM7_IRQn);
    NVIC_SetPriority(TIM7_IRQn, saved_priority);
    uint32_t is_valid = (BASEPRI_GUARD_PRIORITY >= 5U) && (BASEPRI_GUARD_CASE <= 14U)
        && (irq_completed == 1U) && (operation_ok == 1U)
        && (__get_BASEPRI() == 0U) && (__get_PRIMASK() == 0U)
        && (rtos_port_get_critical_nesting() == 0U);

    finish(is_valid, "legal_call_returned_or_missing_expected_assertion");
}
