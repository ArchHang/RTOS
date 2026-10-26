/**
 * @file adversarial_mutex_boundary.c
 * @brief Mutex count boundary, deleted-owner handoff and finite dependency cycle.
 *
 * Only the recursion-count case injects an internal boundary value, while the
 * controller legally owns that mutex. It models UINT32_MAX previous recursive
 * acquisitions. The sample precedes explicit count restoration for cleanup.
 * Owner deletion and finite cyclic waits use public APIs exclusively.
 */
#include "audit_regressions.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx_hal.h"

#include "rtos.h"

#define AMB_STACK_WORDS (256U)
#define AMB_WAIT_TICKS (1500U)
#define AMB_CYCLE_TICKS (1000U)
#define AMB_HIGH_TICKS (100U)

typedef enum amb_mode {
    AMB_MODE_HANDOFF = 0,
    AMB_MODE_CYCLE
} amb_mode_t;

typedef struct amb_result {
    rtos_status_t take;
    rtos_status_t give_dependency;
    rtos_status_t give_own;
    uint32_t owned_on_return;
    uint32_t finished;
} amb_result_t;

static rtos_mutex_t mutexes[2];
static rtos_sem_t ready_sem;
static rtos_sem_t done_sem;
static rtos_sem_t high_done_sem;
static rtos_sem_t gates[2];
static rtos_tcb_t workers[3];
static rtos_stack_t worker_stacks[3][AMB_STACK_WORDS] __attribute__((aligned(8)));
static rtos_tcb_t high_tcb;
static rtos_stack_t high_stack[AMB_STACK_WORDS] __attribute__((aligned(8)));
static amb_result_t results[3];
static rtos_status_t high_result;
static uint32_t acquired_order[2];
static uint32_t acquired_count;
static uint32_t created_mask;
static uint32_t high_created;
static uint32_t mutex_count;
static uint32_t sem_count;
static amb_mode_t mode;
static uint32_t pass_count;
static uint32_t fail_count;

static void check_value(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        pass_count++;
        (void)printf("AMB|PASS|name=%s actual=%lu expected=%lu\r\n", name,
            (unsigned long)actual, (unsigned long)expected);
    } else {
        fail_count++;
        (void)printf("AMB|FAIL|name=%s actual=%lu expected=%lu\r\n", name,
            (unsigned long)actual, (unsigned long)expected);
    }
}

static void park_task(void)
{
    (void)rtos_task_suspend(NULL);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static void publish_result(uint32_t index, const amb_result_t *result)
{
    RTOS_PORT_ENTER_CRITICAL();
    results[index] = *result;
    results[index].finished = 1U;
    RTOS_PORT_EXIT_CRITICAL();
    (void)rtos_sem_give(&done_sem);
}

static void handoff_entry(uint32_t index)
{
    amb_result_t result = {RTOS_ERR_PARAM, RTOS_ERR_PARAM, RTOS_ERR_PARAM, 0U, 0U};

    result.take = rtos_mutex_take(&mutexes[0], RTOS_WAIT_FOREVER);
    if (result.take == RTOS_OK) {
        RTOS_PORT_ENTER_CRITICAL();
        result.owned_on_return = mutexes[0].owner == &workers[index];
        if (index != 0U && acquired_count < 2U) {
            acquired_order[acquired_count++] = index;
        }
        RTOS_PORT_EXIT_CRITICAL();
        if (index <= 1U) {
            (void)rtos_sem_give(&ready_sem);
            (void)rtos_sem_take(&gates[index == 0U ? 1U : 0U], RTOS_WAIT_FOREVER);
        }
        result.give_own = rtos_mutex_give(&mutexes[0]);
    }
    publish_result(index, &result);
}

static void cycle_entry(uint32_t index)
{
    uint32_t dependency = index ^ 1U;
    amb_result_t result = {RTOS_ERR_PARAM, RTOS_ERR_PARAM, RTOS_ERR_PARAM, 0U, 0U};
    rtos_status_t own = rtos_mutex_take(&mutexes[index], RTOS_NO_WAIT);

    (void)rtos_sem_give(&ready_sem);
    if (own == RTOS_OK) {
        (void)rtos_sem_take(&gates[index], RTOS_WAIT_FOREVER);
        result.take = rtos_mutex_take(&mutexes[dependency], AMB_CYCLE_TICKS);
        if (result.take == RTOS_OK) {
            RTOS_PORT_ENTER_CRITICAL();
            result.owned_on_return = mutexes[dependency].owner == &workers[index];
            RTOS_PORT_EXIT_CRITICAL();
            result.give_dependency = rtos_mutex_give(&mutexes[dependency]);
        }
        result.give_own = rtos_mutex_give(&mutexes[index]);
    }
    publish_result(index, &result);
}

static void worker_entry(void *arg)
{
    uint32_t index = (uint32_t)(uintptr_t)arg;
    if (mode == AMB_MODE_HANDOFF) {
        handoff_entry(index);
    } else {
        cycle_entry(index);
    }
    park_task();
}

static void high_entry(void *arg)
{
    (void)arg;
    rtos_status_t status = rtos_mutex_take(&mutexes[0], AMB_HIGH_TICKS);
    if (status == RTOS_OK) {
        (void)rtos_mutex_give(&mutexes[0]);
    }
    RTOS_PORT_ENTER_CRITICAL();
    high_result = status;
    RTOS_PORT_EXIT_CRITICAL();
    (void)rtos_sem_give(&high_done_sem);
    park_task();
}

static uint32_t wait_blocked(rtos_tcb_t *task, void *object)
{
    uint32_t started = DWT->CYCCNT;

    for (uint32_t tick = 0U; tick < AMB_WAIT_TICKS; tick++) {
        RTOS_PORT_ENTER_CRITICAL();
        uint32_t blocked = ((task->state == RTOS_TASK_BLOCKED)
            || (task->state == RTOS_TASK_DELAYED)) && task->blocked_on == object;
        RTOS_PORT_EXIT_CRITICAL();
        if (blocked != 0U) {
            return 1U;
        }
        if ((uint32_t)(DWT->CYCCNT - started) >= SystemCoreClock * 2U) {
            break;
        }
        rtos_task_delay(1U);
    }
    return 0U;
}

static uint32_t create_worker(uint32_t index, rtos_prio_t priority)
{
    rtos_status_t status = rtos_task_create_static(&workers[index], worker_stacks[index],
        AMB_STACK_WORDS, worker_entry, (void *)(uintptr_t)index, priority, "amb-worker");
    if (status != RTOS_OK) {
        return 0U;
    }
    created_mask |= 1U << index;
    return 1U;
}

static uint32_t initialize_case(uint32_t requested_mutexes)
{
    created_mask = high_created = mutex_count = sem_count = acquired_count = 0U;
    memset(results, 0, sizeof(results));
    memset(acquired_order, 0, sizeof(acquired_order));
    rtos_sem_t *semaphores[] = {&ready_sem, &done_sem, &high_done_sem, &gates[0], &gates[1]};

    for (uint32_t index = 0U; index < sizeof(semaphores) / sizeof(semaphores[0]); index++) {
        if (rtos_sem_init(semaphores[index], 0U, 4U) != RTOS_OK) {
            return 0U;
        }
        sem_count++;
    }
    for (uint32_t index = 0U; index < requested_mutexes; index++) {
        if (rtos_mutex_init(&mutexes[index]) != RTOS_OK) {
            return 0U;
        }
        mutex_count++;
    }
    return 1U;
}

static void cleanup_case(void)
{
    rtos_sched_suspend();
    if (high_created != 0U) {
        check_value("cleanup_high", rtos_task_delete(&high_tcb) == RTOS_OK, 1U);
    }
    for (uint32_t index = 0U; index < 3U; index++) {
        if ((created_mask & (1U << index)) != 0U) {
            check_value("cleanup_worker", rtos_task_delete(&workers[index]) == RTOS_OK, 1U);
        }
    }
    for (uint32_t index = 0U; index < mutex_count; index++) {
        check_value("cleanup_mutex", rtos_mutex_deinit(&mutexes[index]) == RTOS_OK, 1U);
    }
    rtos_sem_t *semaphores[] = {&ready_sem, &done_sem, &high_done_sem, &gates[0], &gates[1]};
    for (uint32_t index = 0U; index < sem_count; index++) {
        check_value("cleanup_sem", rtos_sem_deinit(semaphores[index]) == RTOS_OK, 1U);
    }
    rtos_sched_resume();
}

static void recursion_boundary(void)
{
    uint32_t initialized = initialize_case(1U);

    check_value("recursion_init", initialized, 1U);
    if (initialized == 0U) {
        cleanup_case();
        return;
    }
    check_value("unowned_give_rejected", rtos_mutex_give(&mutexes[0]) == RTOS_ERR_PARAM, 1U);
    rtos_status_t taken = rtos_mutex_take(&mutexes[0], RTOS_NO_WAIT);
    check_value("recursion_owner_take", taken == RTOS_OK, 1U);
    if (taken != RTOS_OK) {
        cleanup_case();
        return;
    }
    rtos_tcb_t *self = rtos_task_get_current();
    RTOS_PORT_ENTER_CRITICAL();
    mutexes[0].recursion_count = UINT32_MAX;
    RTOS_PORT_EXIT_CRITICAL();
    rtos_status_t status = rtos_mutex_take(&mutexes[0], RTOS_NO_WAIT);
    RTOS_PORT_ENTER_CRITICAL();
    uint32_t sampled_count = mutexes[0].recursion_count;
    uint32_t owner_retained = mutexes[0].owner == self;
    /* Evidence is sampled first. Restore one actual acquisition for cleanup. */
    mutexes[0].recursion_count = 1U;
    RTOS_PORT_EXIT_CRITICAL();
    (void)printf("AMB|BOUNDARY|status=%ld count=0x%08lX owner_retained=%lu "
        "injection=UINT32_MAX_previous_acquisitions repair=fixture_only\r\n",
        (long)status, (unsigned long)sampled_count, (unsigned long)owner_retained);
    check_value("recursive_overflow_status", status == RTOS_ERR_OVERFLOW, 1U);
    check_value("recursive_overflow_count", sampled_count, UINT32_MAX);
    check_value("recursive_overflow_owner", owner_retained, 1U);
    check_value("recursive_final_give", rtos_mutex_give(&mutexes[0]) == RTOS_OK, 1U);
    cleanup_case();
}

static void owner_delete_handoff(void)
{
    mode = AMB_MODE_HANDOFF;
    uint32_t initialized = initialize_case(1U);
    check_value("handoff_init", initialized, 1U);
    if (initialized == 0U) {
        cleanup_case();
        return;
    }
    uint32_t constructed = create_worker(0U, 20U)
        && (rtos_sem_take(&ready_sem, AMB_WAIT_TICKS) == RTOS_OK)
        && wait_blocked(&workers[0], &gates[1]) && create_worker(1U, 4U)
        && wait_blocked(&workers[1], &mutexes[0]) && create_worker(2U, 6U)
        && wait_blocked(&workers[2], &mutexes[0]);
    check_value("handoff_waiters_really_blocked", constructed, 1U);
    if (constructed != 0U) {
        rtos_status_t deleted = rtos_task_delete(&workers[0]);
        check_value("delete_holding_owner", deleted == RTOS_OK, 1U);
        if (deleted == RTOS_OK) {
            created_mask &= ~1U;
        }
        check_value("first_actually_acquired",
            rtos_sem_take(&ready_sem, AMB_WAIT_TICKS) == RTOS_OK, 1U);
        check_value("new_owner_paused", wait_blocked(&workers[1], &gates[0]), 1U);
        RTOS_PORT_ENTER_CRITICAL();
        uint32_t transferred = mutexes[0].owner == &workers[1];
        uint32_t remaining = mutexes[0].wait_head == &workers[2].wait_node;
        RTOS_PORT_EXIT_CRITICAL();
        check_value("deleted_owner_transferred", transferred, 1U);
        check_value("remaining_waiter_retained", remaining, 1U);
        check_value("lower_new_owner_base",
            rtos_task_set_priority(&workers[1], 22U) == RTOS_OK, 1U);
        check_value("remaining_waiter_inheritance", rtos_task_get_priority(&workers[1]), 6U);
        check_value("release_new_owner", rtos_sem_give(&gates[0]) == RTOS_OK, 1U);
        for (uint32_t index = 0U; index < 2U; index++) {
            check_value("handoff_completed",
                rtos_sem_take(&done_sem, AMB_WAIT_TICKS) == RTOS_OK, 1U);
        }
        /* The second waiter can preempt the first owner's give before that
         * first owner publishes its result. Consume both completions first. */
        for (uint32_t index = 0U; index < 2U; index++) {
            check_value("handoff_finished", results[index + 1U].finished, 1U);
            check_value("handoff_take_success", results[index + 1U].take == RTOS_OK, 1U);
            check_value("handoff_true_owner", results[index + 1U].owned_on_return, 1U);
            check_value("handoff_give_success", results[index + 1U].give_own == RTOS_OK, 1U);
        }
        check_value("first_acquisition_order", acquired_order[0], 1U);
        check_value("second_acquisition_order", acquired_order[1], 2U);
        check_value("new_owner_base_restored", rtos_task_get_priority(&workers[1]), 22U);
        check_value("handoff_mutex_unowned", mutexes[0].owner == NULL, 1U);
    }
    cleanup_case();
}

static void capture_cycle_priority(const char *name, uint32_t expected)
{
    uint32_t priorities[2];

    RTOS_PORT_ENTER_CRITICAL();
    priorities[0] = workers[0].priority;
    priorities[1] = workers[1].priority;
    RTOS_PORT_EXIT_CRITICAL();
    check_value(name, priorities[0], expected);
    check_value(name, priorities[1], expected);
}

static void finite_cycle(void)
{
    mode = AMB_MODE_CYCLE;
    uint32_t initialized = initialize_case(2U);
    check_value("cycle_init", initialized, 1U);
    if (initialized == 0U) {
        cleanup_case();
        return;
    }
    uint32_t constructed = create_worker(0U, 15U)
        && (rtos_sem_take(&ready_sem, AMB_WAIT_TICKS) == RTOS_OK)
        && wait_blocked(&workers[0], &gates[0]) && create_worker(1U, 16U)
        && (rtos_sem_take(&ready_sem, AMB_WAIT_TICKS) == RTOS_OK)
        && wait_blocked(&workers[1], &gates[1]);
    check_value("cycle_owners_ready", constructed, 1U);
    if (constructed != 0U) {
        check_value("start_cycle_first", rtos_sem_give(&gates[0]) == RTOS_OK, 1U);
        check_value("cycle_first_blocked", wait_blocked(&workers[0], &mutexes[1]), 1U);
        check_value("start_cycle_second", rtos_sem_give(&gates[1]) == RTOS_OK, 1U);
        check_value("cycle_second_blocked", wait_blocked(&workers[1], &mutexes[0]), 1U);
        rtos_status_t created = rtos_task_create_static(&high_tcb, high_stack, AMB_STACK_WORDS,
            high_entry, NULL, 4U, "amb-cycle-high");
        high_created = created == RTOS_OK ? 1U : 0U;
        check_value("cycle_high_created", high_created, 1U);
        if (high_created != 0U) {
            check_value("cycle_high_blocked", wait_blocked(&high_tcb, &mutexes[0]), 1U);
            capture_cycle_priority("cycle_inherits_high", 4U);
            check_value("cycle_high_times_out",
                rtos_sem_take(&high_done_sem, AMB_WAIT_TICKS) == RTOS_OK, 1U);
            check_value("cycle_high_result", high_result == RTOS_ERR_TIMEOUT, 1U);
            capture_cycle_priority("cycle_withdraws_high", 15U);
        }
        for (uint32_t index = 0U; index < 2U; index++) {
            check_value("finite_cycle_completes",
                rtos_sem_take(&done_sem, AMB_WAIT_TICKS) == RTOS_OK, 1U);
        }
        uint32_t timeouts = 0U;
        for (uint32_t index = 0U; index < 2U; index++) {
            timeouts += results[index].take == RTOS_ERR_TIMEOUT ? 1U : 0U;
            check_value("cycle_finished", results[index].finished, 1U);
            check_value("cycle_dependency_status",
                results[index].take == RTOS_OK || results[index].take == RTOS_ERR_TIMEOUT, 1U);
            if (results[index].take == RTOS_OK) {
                check_value("cycle_true_dependency_owner", results[index].owned_on_return, 1U);
                check_value("cycle_dependency_releases",
                    results[index].give_dependency == RTOS_OK, 1U);
            }
            check_value("cycle_owner_releases", results[index].give_own == RTOS_OK, 1U);
            check_value("cycle_base_restored",
                rtos_task_get_priority(&workers[index]), 15U + index);
            check_value("cycle_mutex_unowned", mutexes[index].owner == NULL, 1U);
        }
        check_value("cycle_broken_by_real_timeout", timeouts != 0U, 1U);
    }
    cleanup_case();
}

void audit_pre_start(void)
{
    (void)printf("AMB|META|recursion_boundary_injection=1 owner_handoff_public_api=1 "
        "finite_cycle_public_api=1\r\n");
}

void audit_regressions_task(void *arg)
{
    (void)arg;
    recursion_boundary();
    owner_delete_handoff();
    finite_cycle();
    check_value("final_basepri", __get_BASEPRI(), 0U);
    check_value("final_primask", __get_PRIMASK(), 0U);
    check_value("final_critical_count", rtos_port_get_critical_nesting(), 0U);
    (void)printf("AMB|SUMMARY|pass=%lu fail=%lu\r\n",
        (unsigned long)pass_count, (unsigned long)fail_count);
    (void)printf("AMB|DONE|status=%lu\r\n", fail_count == 0U ? 0UL : 1UL);
    for (;;) {
        rtos_task_delay(1000U);
    }
}
