/**
 * @file usb_osal_priority_regression.c
 * @brief Exercise a temporary USB OSAL yield while a real mutex waiter donates priority.
 */
#include "audit_regressions.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "rtos.h"
#include "rtos_usb.h"

enum {
    UPR_WAITER_PRIORITY = 4U,
    UPR_STACK_WORDS = 192U,
    UPR_WAIT_TICKS = 100U
};

static rtos_mutex_t g_mutex;
static rtos_tcb_t g_waiter;
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT) rtos_stack_t g_waiter_stack[UPR_STACK_WORDS];
static rtos_status_t g_acquire_status = RTOS_ERR_SCHED;
static rtos_status_t g_release_status = RTOS_ERR_SCHED;
static uint32_t g_passed;
static uint32_t g_failed;

static void record_result(const char *name, bool passed, uint32_t observed, uint32_t expected)
{
    if (passed) {
        ++g_passed;
    } else {
        ++g_failed;
    }
    (void)printf("UPR|%s|%s|actual=%lu|expected=%lu\r\n", name, passed ? "PASS" : "FAIL",
                 (unsigned long)observed, (unsigned long)expected);
}

static void run_waiter(void *argument)
{
    (void)argument;
    g_acquire_status = rtos_mutex_take(&g_mutex, UPR_WAIT_TICKS);
    if (g_acquire_status == RTOS_OK) {
        g_release_status = rtos_mutex_give(&g_mutex);
    }
    for (;;) {
        rtos_task_delay(1000U);
    }
}

void audit_pre_start(void)
{
}

static void check_priority(rtos_tcb_t *owner, rtos_prio_t base)
{
    const bool is_donated = (g_mutex.wait_head != NULL) &&
        (g_mutex.wait_head->tcb == &g_waiter) && (owner->base_priority == base) &&
        (rtos_task_get_priority(owner) == UPR_WAITER_PRIORITY);
    record_result("donation_active", is_donated, rtos_task_get_priority(owner),
                  UPR_WAITER_PRIORITY);
    rtos_usb_thread_schedule_other();
    record_result("base_after_yield", owner->base_priority == base, owner->base_priority, base);
    record_result("effective_while_locked", rtos_task_get_priority(owner) == UPR_WAITER_PRIORITY,
                  rtos_task_get_priority(owner), UPR_WAITER_PRIORITY);
    const rtos_status_t status = rtos_mutex_give(&g_mutex);
    record_result("owner_release", status == RTOS_OK, (uint32_t)status, (uint32_t)RTOS_OK);
    rtos_task_delay(2U);
    record_result("effective_after_release", rtos_task_get_priority(owner) == base,
                  rtos_task_get_priority(owner), base);
    record_result("waiter_completion",
                  (g_acquire_status == RTOS_OK) && (g_release_status == RTOS_OK),
                  (uint32_t)g_acquire_status, (uint32_t)RTOS_OK);
}

void audit_regressions_task(void *argument)
{
    (void)argument;
    rtos_tcb_t *const owner = rtos_task_get_current();
    const rtos_prio_t base = owner->base_priority;
    const bool initialized = rtos_mutex_init(&g_mutex) == RTOS_OK;
    const bool acquired = initialized && (rtos_mutex_take(&g_mutex, RTOS_NO_WAIT) == RTOS_OK);
    const bool created = acquired &&
        (rtos_task_create_static(&g_waiter, g_waiter_stack, UPR_STACK_WORDS, run_waiter,
                                 NULL, UPR_WAITER_PRIORITY, "usb-pi-waiter") == RTOS_OK);

    record_result("setup", created, (uint32_t)created, 1U);
    if (created) {
        check_priority(owner, base);
        const rtos_status_t status = rtos_task_delete(&g_waiter);
        record_result("waiter_cleanup", status == RTOS_OK, (uint32_t)status, (uint32_t)RTOS_OK);
    } else if (acquired) {
        (void)rtos_mutex_give(&g_mutex);
    }
    if (initialized) {
        const rtos_status_t status = rtos_mutex_deinit(&g_mutex);
        record_result("mutex_cleanup", status == RTOS_OK, (uint32_t)status, (uint32_t)RTOS_OK);
    }
    (void)printf("UPR|DONE|pass=%lu|fail=%lu\r\n", (unsigned long)g_passed,
                 (unsigned long)g_failed);
    for (;;) {
        rtos_task_delay(1000U);
    }
}
