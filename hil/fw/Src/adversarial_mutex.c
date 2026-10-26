/**
 * @file adversarial_mutex.c
 * @brief Legal task-level mutex dependency chains and priority propagation.
 *
 * Every owner holds mutex[i] and waits for mutex[i+1]. The deepest owner waits
 * on a semaphore gate, so the controller can inspect the fully blocked chain
 * without a CPU-hog task or private TCB edits. The high waiter then waits for
 * mutex[0]. Only public APIs construct, cancel and release these dependencies.
 * A complete chain must reflect both inheritance and withdrawal of inheritance.
 */
#include "audit_regressions.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx_hal.h"

#include "rtos.h"

#define AM_MAX_DEPTH (6U)
#define AM_STACK_WORDS (256U)
#define AM_OWNER_BASE (15U)
#define AM_HIGH_PRIORITY (4U)
#define AM_WAIT_TICKS (500U)
#define AM_TIMEOUT_TICKS (100U)

typedef enum am_case {
    AM_CASE_GRANT = 0,
    AM_CASE_TIMEOUT,
    AM_CASE_SUSPEND,
    AM_CASE_DELETE,
    AM_CASE_REPRIORITIZE,
    AM_CASE_COUNT
} am_case_t;

typedef struct am_worker {
    rtos_status_t own_result;
    rtos_status_t dependency_result;
    rtos_status_t give_result;
    uint32_t completed;
} am_worker_t;

static rtos_mutex_t chain_mutex[AM_MAX_DEPTH];
static rtos_tcb_t owners[AM_MAX_DEPTH];
static rtos_stack_t owner_stacks[AM_MAX_DEPTH][AM_STACK_WORDS] __attribute__((aligned(8)));
static am_worker_t owner_results[AM_MAX_DEPTH];
static rtos_tcb_t high_waiter;
static rtos_stack_t high_stack[AM_STACK_WORDS] __attribute__((aligned(8)));
static rtos_sem_t ready_sem;
static rtos_sem_t release_gate;
static rtos_sem_t done_sem;
static rtos_tick_t high_timeout;
static rtos_status_t high_result;
static uint32_t high_completed;
static uint32_t depth;
static uint32_t owners_created;
static uint32_t mutexes_created;
static uint32_t high_created;
static uint32_t sems_created;
static uint32_t pass_count;
static uint32_t fail_count;
static uint32_t case_count;

static void check_value(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        pass_count++;
        (void)printf("AM|PASS|name=%s actual=%lu expected=%lu\r\n", name,
            (unsigned long)actual, (unsigned long)expected);
    } else {
        fail_count++;
        (void)printf("AM|FAIL|name=%s actual=%lu expected=%lu depth=%lu\r\n", name,
            (unsigned long)actual, (unsigned long)expected, (unsigned long)depth);
    }
}

static void park_task(void)
{
    (void)rtos_task_suspend(NULL);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static void owner_entry(void *arg)
{
    uint32_t index = (uint32_t)(uintptr_t)arg;
    rtos_status_t own = rtos_mutex_take(&chain_mutex[index], RTOS_NO_WAIT);
    rtos_status_t dependency = RTOS_ERR_PARAM;
    rtos_status_t give = RTOS_ERR_PARAM;

    RTOS_PORT_ENTER_CRITICAL();
    owner_results[index].own_result = own;
    RTOS_PORT_EXIT_CRITICAL();
    if (rtos_sem_give(&ready_sem) != RTOS_OK) {
        park_task();
    }
    if (own == RTOS_OK) {
        if (index + 1U == depth) {
            dependency = rtos_sem_take(&release_gate, RTOS_WAIT_FOREVER);
        } else {
            dependency = rtos_mutex_take(&chain_mutex[index + 1U], RTOS_WAIT_FOREVER);
            if (dependency == RTOS_OK) {
                give = rtos_mutex_give(&chain_mutex[index + 1U]);
            }
        }
        if ((index + 1U == depth) || (give == RTOS_OK)) {
            give = rtos_mutex_give(&chain_mutex[index]);
        }
    }
    RTOS_PORT_ENTER_CRITICAL();
    owner_results[index].dependency_result = dependency;
    owner_results[index].give_result = give;
    owner_results[index].completed = 1U;
    RTOS_PORT_EXIT_CRITICAL();
    (void)rtos_sem_give(&done_sem);
    park_task();
}

static void high_entry(void *arg)
{
    (void)arg;
    rtos_status_t status = rtos_mutex_take(&chain_mutex[0], high_timeout);
    if (status == RTOS_OK) {
        rtos_status_t given = rtos_mutex_give(&chain_mutex[0]);
        if (given != RTOS_OK) {
            status = given;
        }
    }
    RTOS_PORT_ENTER_CRITICAL();
    high_result = status;
    high_completed = 1U;
    RTOS_PORT_EXIT_CRITICAL();
    (void)rtos_sem_give(&done_sem);
    park_task();
}

static uint32_t is_blocked_on(rtos_tcb_t *task, void *object)
{
    uint32_t is_blocked;

    RTOS_PORT_ENTER_CRITICAL();
    is_blocked = ((task->state == RTOS_TASK_BLOCKED) || (task->state == RTOS_TASK_DELAYED))
        && (task->blocked_on == object);
    RTOS_PORT_EXIT_CRITICAL();
    return is_blocked;
}

static uint32_t wait_blocked(rtos_tcb_t *task, void *object)
{
    uint32_t started = DWT->CYCCNT;
    rtos_tick_t tick_started = rtos_sched_get_tick_count();

    while ((uint32_t)(DWT->CYCCNT - started) < SystemCoreClock) {
        if (is_blocked_on(task, object) != 0U) {
            return 1U;
        }
        if ((rtos_tick_t)(rtos_sched_get_tick_count() - tick_started) >= AM_WAIT_TICKS) {
            break;
        }
        /* Lower-priority owners must be allowed to reach their real wait. The
         * host capture deadline remains a separate bound if RTOS ticks stop. */
        rtos_task_delay(1U);
    }
    return 0U;
}

static uint32_t wait_high_complete(void)
{
    uint32_t started = DWT->CYCCNT;

    for (uint32_t tick = 0U; tick < AM_WAIT_TICKS; tick++) {
        RTOS_PORT_ENTER_CRITICAL();
        uint32_t completed = high_completed;
        RTOS_PORT_EXIT_CRITICAL();
        if (completed != 0U) {
            return 1U;
        }
        if ((uint32_t)(DWT->CYCCNT - started) >= SystemCoreClock) {
            break;
        }
        rtos_task_delay(1U);
    }
    return 0U;
}

static void check_chain(const char *phase, uint32_t expected)
{
    uint32_t priorities[AM_MAX_DEPTH];
    uint32_t bases[AM_MAX_DEPTH];

    /* Serial output can span a finite wait deadline. Capture the complete
     * dependency state at one instant; never mix samples across print calls. */
    RTOS_PORT_ENTER_CRITICAL();
    for (uint32_t index = 0U; index < depth; index++) {
        priorities[index] = owners[index].priority;
        bases[index] = owners[index].base_priority;
    }
    RTOS_PORT_EXIT_CRITICAL();
    for (uint32_t index = 0U; index < depth; index++) {
        (void)printf("AM|CHAIN|phase=%s index=%lu base=%lu effective=%lu expected=%lu\r\n",
            phase, (unsigned long)index, (unsigned long)bases[index],
            (unsigned long)priorities[index],
            (unsigned long)expected);
        check_value(phase, priorities[index], expected);
    }
}

static uint32_t initialize_case(uint32_t requested_depth)
{
    depth = requested_depth;
    owners_created = mutexes_created = high_created = sems_created = 0U;
    high_completed = 0U;
    high_result = RTOS_ERR_PARAM;
    memset(owner_results, 0, sizeof(owner_results));
    if (rtos_sem_init(&ready_sem, 0U, AM_MAX_DEPTH + 1U) != RTOS_OK) {
        return 0U;
    }
    sems_created = 1U;
    if (rtos_sem_init(&release_gate, 0U, 1U) != RTOS_OK) {
        return 0U;
    }
    sems_created = 2U;
    if (rtos_sem_init(&done_sem, 0U, AM_MAX_DEPTH + 1U) != RTOS_OK) {
        return 0U;
    }
    sems_created = 3U;
    for (uint32_t index = 0U; index < depth; index++) {
        if (rtos_mutex_init(&chain_mutex[index]) != RTOS_OK) {
            return 0U;
        }
        mutexes_created++;
    }
    return 1U;
}

static uint32_t construct_chain(void)
{
    for (uint32_t reverse = depth; reverse > 0U; reverse--) {
        uint32_t index = reverse - 1U;
        rtos_status_t status = rtos_task_create_static(&owners[index], owner_stacks[index],
            AM_STACK_WORDS, owner_entry, (void *)(uintptr_t)index,
            (rtos_prio_t)(AM_OWNER_BASE + index), "am-owner");
        if (status != RTOS_OK) {
            return 0U;
        }
        owners_created++;
        if (rtos_sem_take(&ready_sem, AM_WAIT_TICKS) != RTOS_OK) {
            return 0U;
        }
        void *object = index + 1U == depth ? (void *)&release_gate
            : (void *)&chain_mutex[index + 1U];
        if (wait_blocked(&owners[index], object) == 0U) {
            return 0U;
        }
    }
    return 1U;
}

static void cleanup_case(void)
{
    /* No blocking calls occur here. Suspending scheduling prevents a released
     * owner from running halfway through fixture teardown. */
    rtos_sched_suspend();
    if (high_created != 0U) {
        check_value("cleanup_high", rtos_task_delete(&high_waiter) == RTOS_OK, 1U);
        high_created = 0U;
    }
    for (uint32_t reverse = depth; reverse > 0U; reverse--) {
        uint32_t index = reverse - 1U;
        if (index >= depth - owners_created) {
            check_value("cleanup_owner", rtos_task_delete(&owners[index]) == RTOS_OK, 1U);
        }
    }
    for (uint32_t index = 0U; index < mutexes_created; index++) {
        check_value("cleanup_mutex", rtos_mutex_deinit(&chain_mutex[index]) == RTOS_OK, 1U);
    }
    if (sems_created >= 3U) {
        check_value("cleanup_done", rtos_sem_deinit(&done_sem) == RTOS_OK, 1U);
    }
    if (sems_created >= 2U) {
        check_value("cleanup_gate", rtos_sem_deinit(&release_gate) == RTOS_OK, 1U);
    }
    if (sems_created >= 1U) {
        check_value("cleanup_ready", rtos_sem_deinit(&ready_sem) == RTOS_OK, 1U);
    }
    rtos_sched_resume();
}

static void apply_action(am_case_t kind)
{
    if (kind == AM_CASE_TIMEOUT) {
        check_value("timeout_completed", wait_high_complete(), 1U);
        check_value("timeout_result", high_result == RTOS_ERR_TIMEOUT, 1U);
        check_chain("timeout_withdrawal", AM_OWNER_BASE);
    } else if (kind == AM_CASE_SUSPEND) {
        check_value("suspend_high", rtos_task_suspend(&high_waiter) == RTOS_OK, 1U);
        check_chain("suspend_withdrawal", AM_OWNER_BASE);
        check_value("resume_high", rtos_task_resume(&high_waiter) == RTOS_OK, 1U);
        check_value("resume_completed", wait_high_complete(), 1U);
        check_value("resume_canceled_result", high_result == RTOS_ERR_DELETED, 1U);
    } else if (kind == AM_CASE_DELETE) {
        check_value("delete_high", rtos_task_delete(&high_waiter) == RTOS_OK, 1U);
        high_created = 0U;
        check_chain("delete_withdrawal", AM_OWNER_BASE);
    } else if (kind == AM_CASE_REPRIORITIZE) {
        const rtos_prio_t priorities[] = {6U, 3U, 26U, AM_HIGH_PRIORITY};
        for (uint32_t index = 0U; index < sizeof(priorities) / sizeof(priorities[0]); index++) {
            check_value("change_high_priority",
                rtos_task_set_priority(&high_waiter, priorities[index]) == RTOS_OK, 1U);
            uint32_t effective = priorities[index] < AM_OWNER_BASE
                ? priorities[index] : AM_OWNER_BASE;
            check_chain("changed_waiter", effective);
        }
    }
}

static void release_and_verify(am_case_t kind)
{
    uint32_t expected_done = depth + (kind == AM_CASE_DELETE ? 0U : 1U);

    check_value("release_gate", rtos_sem_give(&release_gate) == RTOS_OK, 1U);
    for (uint32_t index = 0U; index < expected_done; index++) {
        check_value("completion_token", rtos_sem_take(&done_sem, AM_WAIT_TICKS) == RTOS_OK, 1U);
    }
    for (uint32_t index = 0U; index < depth; index++) {
        check_value("owner_completed", owner_results[index].completed, 1U);
        check_value("owner_own_result", owner_results[index].own_result == RTOS_OK, 1U);
        check_value("owner_dependency_result", owner_results[index].dependency_result == RTOS_OK, 1U);
        check_value("owner_give_result", owner_results[index].give_result == RTOS_OK, 1U);
        check_value("owner_base_restored", owners[index].priority, AM_OWNER_BASE + index);
        check_value("mutex_unowned", chain_mutex[index].owner == NULL, 1U);
    }
    if ((kind == AM_CASE_GRANT) || (kind == AM_CASE_REPRIORITIZE)) {
        check_value("high_granted", high_result == RTOS_OK, 1U);
    }
    check_value("final_basepri", __get_BASEPRI(), 0U);
    check_value("final_primask", __get_PRIMASK(), 0U);
    check_value("final_critical_count", rtos_port_get_critical_nesting(), 0U);
}

static void run_case(am_case_t kind, uint32_t requested_depth)
{
    case_count++;
    (void)printf("AM|CASE|id=%lu kind=%lu depth=%lu\r\n", (unsigned long)case_count,
        (unsigned long)kind, (unsigned long)requested_depth);
    uint32_t initialized = initialize_case(requested_depth);
    check_value("case_initialized", initialized, 1U);
    uint32_t constructed = initialized != 0U ? construct_chain() : 0U;
    check_value("owners_really_blocked", constructed, 1U);
    if (constructed != 0U) {
        check_chain("initial_chain", AM_OWNER_BASE);
        high_timeout = kind == AM_CASE_TIMEOUT ? AM_TIMEOUT_TICKS : RTOS_WAIT_FOREVER;
        rtos_status_t status = rtos_task_create_static(&high_waiter, high_stack,
            AM_STACK_WORDS, high_entry, NULL, AM_HIGH_PRIORITY, "am-high");
        high_created = status == RTOS_OK ? 1U : 0U;
        check_value("high_created", high_created, 1U);
        if (high_created != 0U) {
            check_value("high_really_blocked", wait_blocked(&high_waiter, &chain_mutex[0]), 1U);
            check_chain("high_inheritance", AM_HIGH_PRIORITY);
            apply_action(kind);
            release_and_verify(kind);
        }
    }
    cleanup_case();
}

void audit_pre_start(void)
{
    (void)printf("AM|META|public_api_only=1 private_tcb_writes=0 "
        "depths=2,3,6 owners_base=15..20 high=4\r\n");
}

void audit_regressions_task(void *arg)
{
    static const uint32_t depths[] = {2U, 3U, 6U};

    (void)arg;
    for (uint32_t kind = 0U; kind < (uint32_t)AM_CASE_COUNT; kind++) {
        for (uint32_t index = 0U; index < sizeof(depths) / sizeof(depths[0]); index++) {
            run_case((am_case_t)kind, depths[index]);
        }
    }
    (void)printf("AM|SUMMARY|pass=%lu fail=%lu cases=%lu\r\n",
        (unsigned long)pass_count, (unsigned long)fail_count, (unsigned long)case_count);
    (void)printf("AM|DONE|status=%lu\r\n", fail_count == 0U ? 0UL : 1UL);
    for (;;) {
        rtos_task_delay(1000U);
    }
}
