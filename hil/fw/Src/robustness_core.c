/**
 * @file robustness_core.c
 * @brief Board regressions for task storage, blocking masks and scheduler policy.
 */
#include "robustness_core.h"

#include <stdbool.h>

#include "robustness.h"
#include "rtos.h"

#define CORE_PROBE_STACK_WORDS (192U)
#define CORE_MIN_TASK_STACK_WORDS (96U)
#define CORE_HEAP_TAIL_BYTES (256U)
#define CORE_PROBE_LOW_PRIO (19U)
#define CORE_PROBE_HIGH_PRIO (8U)

static rtos_tcb_t probe_task;
static rtos_tcb_t overlap_task;
static _Alignas(8) rtos_stack_t probe_stack[2U * CORE_PROBE_STACK_WORDS];
static rtos_sem_t mask_sem;
static bool has_guard_report;

static void core_wait_task(void *context)
{
    (void)context;
    for (;;) {
        rtos_task_delay(2000U);
    }
}

static void check_storage_overlap(void)
{
    rtos_status_t status = rtos_task_create_static(&probe_task, probe_stack,
                                                  CORE_PROBE_STACK_WORDS, core_wait_task,
                                                  NULL, CORE_PROBE_LOW_PRIO, "core-storage");
    robustness_check("core_static_storage_create", status == RTOS_OK);
    if (status != RTOS_OK) {
        return;
    }

    status = rtos_task_create_static(&overlap_task, &probe_stack[8U], CORE_PROBE_STACK_WORDS,
                                     core_wait_task, NULL, CORE_PROBE_LOW_PRIO, "core-overlap");
    robustness_check("core_static_overlapping_stack_rejected", status == RTOS_ERR_PARAM);
    /* The baseline can safely delete these distinct TCBs without running
     * either lower-priority task on their overlapping storage. */
    robustness_check("core_static_overlap_cleanup",
                     status != RTOS_OK || rtos_task_delete(&overlap_task) == RTOS_OK);

    status = rtos_task_create_static(&overlap_task, &probe_stack[CORE_PROBE_STACK_WORDS],
                                     CORE_PROBE_STACK_WORDS, core_wait_task, NULL,
                                     CORE_PROBE_LOW_PRIO, "core-adjacent");
    robustness_check("core_static_adjacent_stack_accepted", status == RTOS_OK);
    if (status == RTOS_OK) {
        robustness_check("core_static_adjacent_cleanup",
                         rtos_task_delete(&overlap_task) == RTOS_OK);
    }
    robustness_check("core_static_storage_cleanup", rtos_task_delete(&probe_task) == RTOS_OK);
}

static void check_dynamic_memory_pressure(void)
{
    const uint32_t largest = rtos_heap_get_max_alloc();
    if (largest <= CORE_HEAP_TAIL_BYTES) {
        robustness_check("core_dynamic_stack_pressure_setup", false);
        return;
    }

    void *reservation = rtos_heap_alloc(largest - CORE_HEAP_TAIL_BYTES);
    const uint32_t remaining = rtos_heap_get_max_alloc();
    const bool is_limited = (reservation != NULL) && (remaining >= sizeof(rtos_tcb_t)) &&
        (remaining < CORE_MIN_TASK_STACK_WORDS * sizeof(rtos_stack_t));
    robustness_check("core_dynamic_stack_pressure_setup", is_limited);
    if (is_limited) {
        const uint32_t free_bytes = rtos_heap_get_free();
        const uint32_t allocated_count = rtos_heap_get_alloc_count();
        const uint32_t released_count = rtos_heap_get_free_count();
        rtos_tcb_t *current = rtos_task_get_current();
        rtos_tcb_t *created = current;
        const rtos_status_t status = rtos_task_create(&created, CORE_MIN_TASK_STACK_WORDS,
                                                       core_wait_task, NULL, CORE_PROBE_LOW_PRIO,
                                                       "core-heap-pressure");
        robustness_check("core_dynamic_stack_pressure_no_mem", status == RTOS_ERR_NO_MEM);
        robustness_check("core_dynamic_stack_pressure_rollback",
                         created == current && rtos_heap_get_free() == free_bytes &&
                         rtos_heap_get_max_alloc() == remaining &&
                         rtos_heap_get_alloc_count() - allocated_count == 1U &&
                         rtos_heap_get_free_count() - released_count == 1U);
        if (status == RTOS_OK) {
            robustness_check("core_dynamic_unexpected_task_cleanup",
                             rtos_task_delete(created) == RTOS_OK);
        }
    }
    rtos_heap_free(reservation);
}

void robustness_core_record_stack_overflow(rtos_tcb_t *task)
{
    if (task == &probe_task) {
        RTOS_PORT_ENTER_CRITICAL();
        has_guard_report = true;
        RTOS_PORT_EXIT_CRITICAL();
    }
}

static void check_physical_stack_guard(void)
{
#if RTOS_CONFIG_CHECK_FOR_STACK_OVERFLOW == 2
    rtos_status_t status = rtos_task_create_static(&probe_task, probe_stack, CORE_PROBE_STACK_WORDS,
                                                   core_wait_task, NULL, CORE_PROBE_LOW_PRIO,
                                                   "core-stack-guard");
    robustness_check("core_stack_guard_task_created", status == RTOS_OK);
    if (status != RTOS_OK) {
        return;
    }

    rtos_task_delay(10U);
    RTOS_PORT_ENTER_CRITICAL();
    const rtos_stack_t original = probe_stack[0];
    const bool is_delayed = (probe_task.state == RTOS_TASK_DELAYED);
    has_guard_report = false;
    probe_stack[0] = original ^ 1U;
    RTOS_PORT_EXIT_CRITICAL();
    robustness_check("core_stack_guard_probe_is_delayed", is_delayed);

    rtos_task_delay(750U);
    RTOS_PORT_ENTER_CRITICAL();
    const bool has_report = has_guard_report;
    probe_stack[0] = original;
    RTOS_PORT_EXIT_CRITICAL();
    robustness_check("core_physical_stack_guard_reported", has_report);
    robustness_check("core_stack_guard_task_cleanup", rtos_task_delete(&probe_task) == RTOS_OK);
#endif
}

static void check_masked_wait(void)
{
    rtos_status_t status = rtos_sem_init(&mask_sem, 0U, 1U);
    robustness_check("core_masked_wait_sem_created", status == RTOS_OK);
    if (status != RTOS_OK) {
        return;
    }

    RTOS_PORT_ENTER_CRITICAL();
    status = rtos_sem_take(&mask_sem, 2U);
    RTOS_PORT_EXIT_CRITICAL();
    robustness_check("core_nested_wait_rejected", status == RTOS_ERR_SCHED);

    /* The controller starts unmasked. PRIMASK blocks PendSV even when the
     * RTOS critical sections normally use BASEPRI. */
    RTOS_PORT_DISABLE_INTERRUPTS();
    status = rtos_sem_take(&mask_sem, 2U);
    RTOS_PORT_ENABLE_INTERRUPTS();
    __asm volatile("isb" ::: "memory");
    robustness_check("core_primask_wait_rejected", status == RTOS_ERR_SCHED);

    uint32_t old_basepri;
    const uint32_t mask = RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY;
    __asm volatile("mrs %[old], basepri\n"
                   "msr basepri, %[mask]\n"
                   "isb" : [old] "=&r"(old_basepri) : [mask] "r"(mask) : "memory");
    status = rtos_sem_take(&mask_sem, 2U);
    __asm volatile("msr basepri, %[old]\n"
                   "isb" :: [old] "r"(old_basepri) : "memory");
    robustness_check("core_basepri_wait_rejected", status == RTOS_ERR_SCHED);

    __asm volatile("cpsid f\n"
                   "isb" ::: "memory");
    status = rtos_sem_take(&mask_sem, 2U);
    __asm volatile("cpsie f\n"
                   "isb" ::: "memory");
    robustness_check("core_faultmask_wait_rejected", status == RTOS_ERR_SCHED);

    RTOS_PORT_ENTER_CRITICAL();
    const rtos_status_t give_status = rtos_sem_give(&mask_sem);
    const rtos_status_t take_status = rtos_sem_take(&mask_sem, 2U);
    RTOS_PORT_EXIT_CRITICAL();
    robustness_check("core_masked_available_resource_accepted",
                     give_status == RTOS_OK && take_status == RTOS_OK);
    robustness_check("core_unmasked_finite_wait_times_out",
                     rtos_sem_take(&mask_sem, 2U) == RTOS_ERR_TIMEOUT);
    robustness_check("core_masked_wait_sem_cleanup", rtos_sem_deinit(&mask_sem) == RTOS_OK);
}

#if !RTOS_CONFIG_USE_PREEMPTION
static uint32_t probe_execution_count;

static void core_count_task(void *context)
{
    (void)context;
    RTOS_PORT_ENTER_CRITICAL();
    ++probe_execution_count;
    RTOS_PORT_EXIT_CRITICAL();
}

static uint32_t read_probe_count(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    const uint32_t count = probe_execution_count;
    RTOS_PORT_EXIT_CRITICAL();
    return count;
}

static void check_cooperative_policy(void)
{
    const rtos_tick_t started = rtos_sched_get_tick_count();
    rtos_status_t status = rtos_task_create_static(&probe_task, probe_stack, CORE_PROBE_STACK_WORDS,
                                                   core_count_task, NULL, CORE_PROBE_HIGH_PRIO,
                                                   "core-tick");
    robustness_check("core_cooperative_higher_ready", status == RTOS_OK);
    if (status != RTOS_OK) {
        return;
    }

    while ((rtos_sched_get_tick_count() - started) < 4U) {
        /* Wait across several real ticks without giving up the CPU. */
    }
    RTOS_PORT_ENTER_CRITICAL();
    const bool has_worker_run = probe_execution_count != 0U;
    const rtos_status_t cleanup = probe_task.state == RTOS_TASK_DELETED ?
        RTOS_OK : rtos_task_delete(&probe_task);
    RTOS_PORT_EXIT_CRITICAL();
    robustness_check("core_cooperative_tick_does_not_preempt",
                     !has_worker_run && cleanup == RTOS_OK);

    /* Keep both PendSV and SysTick deferred while capturing the yield request.
     * A baseline tick after unmasking cannot make this assertion pass. */
    RTOS_PORT_ENTER_CRITICAL();
    const uint32_t count_before_yield = probe_execution_count;
    status = rtos_task_create_static(&probe_task, probe_stack, CORE_PROBE_STACK_WORDS,
                                     core_count_task, NULL, CORE_PROBE_HIGH_PRIO, "core-yield");
    const bool is_ready_before_yield = status == RTOS_OK && probe_task.state == RTOS_TASK_READY &&
        rtos_kernel.next_tcb != &probe_task && probe_execution_count == count_before_yield;
    if (is_ready_before_yield) {
        rtos_task_yield();
    }
    const bool has_yield_request = rtos_kernel.next_tcb == &probe_task;
    RTOS_PORT_EXIT_CRITICAL();
    robustness_check("core_cooperative_yield_picks_higher_ready",
                     is_ready_before_yield && has_yield_request &&
                     read_probe_count() == count_before_yield + 1U);

    const rtos_tick_t before_delay = rtos_sched_get_tick_count();
    rtos_task_delay(2U);
    robustness_check("core_cooperative_idle_dispatches_wake",
                     (rtos_sched_get_tick_count() - before_delay) >= 2U);
}
#endif

void robustness_core_run(void)
{
    check_storage_overlap();
    check_dynamic_memory_pressure();
    check_physical_stack_guard();
    check_masked_wait();
#if !RTOS_CONFIG_USE_PREEMPTION
    check_cooperative_policy();
#endif
}
