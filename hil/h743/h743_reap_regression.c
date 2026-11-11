/** @file h743_reap_regression.c
 * @brief Keep Idle off the CPU while higher-priority dynamic tasks exit.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "rtos.h"
#include "rtos_heap.h"

enum {
    H7_REAP_ITERATIONS = 256U,
    H7_REAP_CHILD_PRIORITY = 5U,
    H7_REAP_STACK_WORDS = 160U
};

static uint32_t g_completed;
static uint32_t g_passed;
static uint32_t g_failed;

void h743_reap_regression_run(void *argument);

static void child_run(void *argument)
{
    (void)argument;
    ++g_completed;
    /* The port's task-return trampoline exercises real deferred self-deletion. */
}

static void check_result(const char *name, bool is_passed)
{
    if (is_passed) {
        ++g_passed;
    } else {
        ++g_failed;
    }
    (void)printf("H7REAP|CHECK|%s|%s\r\n", name, is_passed ? "PASS" : "FAIL");
}

static uint32_t live_allocations(void)
{
    return rtos_heap_get_alloc_count() - rtos_heap_get_free_count();
}

static void check_exhaustion(uint32_t expected_live, uint32_t expected_max, bool leave_tcb_room)
{
    const uint32_t reserve = leave_tcb_room ? (uint32_t)sizeof(rtos_tcb_t) + 32U : 0U;
    void *const allocation = rtos_heap_alloc(expected_max - reserve);
    rtos_tcb_t *out = rtos_kernel.idle_tcb;
    rtos_status_t status = RTOS_ERR_PARAM;
    bool is_target_space = false;
    if (allocation != NULL) {
        const uint32_t available = rtos_heap_get_max_alloc();
        is_target_space = leave_tcb_room
            ? available >= sizeof(rtos_tcb_t) && available < H7_REAP_STACK_WORDS * sizeof(rtos_stack_t)
            : available == 0U;
        status = rtos_task_create(&out, H7_REAP_STACK_WORDS, child_run, NULL,
                                  H7_REAP_CHILD_PRIORITY, "reap-full");
        rtos_heap_free(allocation);
    }
    check_result(leave_tcb_room ? "stack_exhaustion_preserves_output" : "tcb_exhaustion_preserves_output",
                 is_target_space &&
                 status == RTOS_ERR_NO_MEM && out == rtos_kernel.idle_tcb);
    check_result(leave_tcb_room ? "stack_exhaustion_rolls_back_tcb" : "tcb_exhaustion_preserves_heap",
                 live_allocations() == expected_live &&
                 rtos_heap_get_max_alloc() == expected_max);
}

void h743_reap_regression_run(void *argument)
{
    (void)argument;
    const uint32_t initial_free = rtos_heap_get_free();
    const uint32_t initial_max = rtos_heap_get_max_alloc();
    const uint32_t initial_live = live_allocations();
    const uint32_t initial_idle_switches = rtos_kernel.idle_tcb->switch_count;
    rtos_status_t status = RTOS_OK;
    uint32_t created = 0U;

    for (; created < H7_REAP_ITERATIONS; ++created) {
        rtos_tcb_t *out = NULL;
        status = rtos_task_create(&out, H7_REAP_STACK_WORDS, child_run, NULL,
                                  H7_REAP_CHILD_PRIORITY, "reap-child");
        if (status != RTOS_OK) {
            break;
        }
    }
    const uint32_t idle_switches = rtos_kernel.idle_tcb->switch_count - initial_idle_switches;
    (void)printf("H7REAP|LOAD|created=%lu|completed=%lu|status=%ld|idle_switches=%lu"
                 "|initial_free=%lu|free_before_idle=%lu\r\n",
                 (unsigned long)created, (unsigned long)g_completed, (long)status,
                 (unsigned long)idle_switches, (unsigned long)initial_free,
                 (unsigned long)rtos_heap_get_free());
    check_result("idle_never_dispatched_during_churn", idle_switches == 0U);
    check_result("all_created_tasks_exited", created == g_completed);
    check_result("churn_survives_without_idle", created == H7_REAP_ITERATIONS);

    rtos_task_delay(4U);
    check_result("idle_recovers_entire_heap", live_allocations() == initial_live &&
                 rtos_heap_get_max_alloc() == initial_max &&
                 rtos_kernel.idle_tcb->switch_count != initial_idle_switches);
    check_exhaustion(initial_live, initial_max, false);
    check_exhaustion(initial_live, initial_max, true);
    (void)printf("H7REAP|DONE|pass=%lu|fail=%lu\r\n", (unsigned long)g_passed,
                 (unsigned long)g_failed);
    for (;;) {
        rtos_task_delay(1000U);
    }
}
