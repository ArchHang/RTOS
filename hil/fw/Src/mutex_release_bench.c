/**
 * @file mutex_release_bench.c
 * @brief Measure final mutex release with no waiters and verify real PI handoff.
 */
#include "audit_regressions.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "stm32f4xx.h"

#include "adversarial_registry.h"
#include "rtos.h"

enum {
    MUTEX_RELEASE_SAMPLES = 1000U,
    MUTEX_RELEASE_WORKER_PRIORITY = 6U,
    MUTEX_RELEASE_STACK_WORDS = 256U,
    MUTEX_RELEASE_WAIT_TICKS = 20U
};

typedef struct mutex_release_worker {
    bool is_done;
    bool has_lock;
    rtos_status_t take_status;
    rtos_status_t give_status;
} mutex_release_worker_t;

static rtos_mutex_t measured_mutexes[RTOS_CONFIG_MAX_MUTEXES];
static rtos_mutex_t pi_mutexes[2];
static rtos_tcb_t worker_tcb;
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT) rtos_stack_t worker_stack[MUTEX_RELEASE_STACK_WORDS];
static mutex_release_worker_t worker_result;
static uint32_t failure_count;

static void report_check(const char *name, bool is_valid)
{
    if (!is_valid) {
        ++failure_count;
    }
    (void)printf("MRB|%s|%s\r\n", name, is_valid ? "PASS" : "FAIL");
}

static bool is_mutex_free(const rtos_mutex_t *mutex)
{
    return (mutex->owner == NULL) && (mutex->recursion_count == 0U)
           && (mutex->is_handoff_pending == 0U) && (mutex->wait_head == NULL);
}

static bool check_registry_count(uint32_t expected)
{
    RTOS_PORT_ENTER_CRITICAL();
    bool is_expected = rtos_internal_mutex_registry_count() == expected;
    RTOS_PORT_EXIT_CRITICAL();
    return is_expected;
}

static void measure_release(uint32_t registry_count)
{
    uint32_t initialized = 0U;
    uint32_t samples = 0U;
    uint32_t minimum = UINT32_MAX;
    uint32_t maximum = 0U;
    uint64_t total = 0U;

    while (initialized < registry_count) {
        if (rtos_mutex_init(&measured_mutexes[initialized]) != RTOS_OK) {
            break;
        }
        ++initialized;
    }

    bool is_valid = (initialized == registry_count) && check_registry_count(registry_count);
    if (is_valid) {
        while (samples < MUTEX_RELEASE_SAMPLES) {
            if (rtos_mutex_take(&measured_mutexes[0], RTOS_NO_WAIT) != RTOS_OK) {
                break;
            }
            uint32_t start = DWT->CYCCNT;
            rtos_status_t status = rtos_mutex_give(&measured_mutexes[0]);
            uint32_t elapsed = DWT->CYCCNT - start; /* Modulo 2^32 handles DWT wrap. */
            if ((status != RTOS_OK) || (elapsed == 0U) || !is_mutex_free(&measured_mutexes[0])) {
                break;
            }
            if (elapsed < minimum) {
                minimum = elapsed;
            }
            if (elapsed > maximum) {
                maximum = elapsed;
            }
            total += elapsed;
            ++samples;
        }
    }

    bool is_clean = true;
    while (initialized != 0U) {
        --initialized;
        is_clean = (rtos_mutex_deinit(&measured_mutexes[initialized]) == RTOS_OK) && is_clean;
    }
    is_valid = is_valid && (samples == MUTEX_RELEASE_SAMPLES) && is_clean
               && check_registry_count(0U);
    (void)printf("MRB|GIVE|registered=%lu|N=%lu|avg=%lu|min=%lu|max=%lu|%s\r\n",
                 (unsigned long)registry_count, (unsigned long)samples,
                 (unsigned long)((samples != 0U) ? (total / samples) : 0U),
                 (unsigned long)((samples != 0U) ? minimum : 0U), (unsigned long)maximum,
                 is_valid ? "PASS" : "FAIL");
    if (!is_valid) {
        ++failure_count;
    }
}

static void wait_for_mutex(void *argument)
{
    (void)argument;
    rtos_status_t take_status = rtos_mutex_take(&pi_mutexes[1], RTOS_WAIT_FOREVER);
    bool has_lock = (take_status == RTOS_OK) && (pi_mutexes[1].owner == &worker_tcb)
                    && (pi_mutexes[1].is_handoff_pending == 0U);
    rtos_status_t give_status = has_lock ? rtos_mutex_give(&pi_mutexes[1]) : RTOS_ERR_PARAM;
    RTOS_PORT_ENTER_CRITICAL();
    worker_result = (mutex_release_worker_t){true, has_lock, take_status, give_status};
    RTOS_PORT_EXIT_CRITICAL();
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static bool check_waiter(const rtos_tcb_t *owner)
{
    return (pi_mutexes[1].wait_head == &worker_tcb.wait_node)
           && (worker_tcb.blocked_on == &pi_mutexes[1]) && (pi_mutexes[1].owner == owner)
           && (owner->priority == MUTEX_RELEASE_WORKER_PRIORITY);
}

static bool wait_until_inherited(const rtos_tcb_t *owner)
{
    for (uint32_t tick = 0U; tick < MUTEX_RELEASE_WAIT_TICKS; ++tick) {
        RTOS_PORT_ENTER_CRITICAL();
        bool is_inherited = check_waiter(owner);
        RTOS_PORT_EXIT_CRITICAL();
        if (is_inherited) {
            return true;
        }
        rtos_task_delay(1U);
    }
    return false;
}

static bool check_handoff(rtos_tcb_t *owner)
{
    RTOS_PORT_ENTER_CRITICAL();
    bool is_transferred = (rtos_mutex_give(&pi_mutexes[1]) == RTOS_OK)
                          && (pi_mutexes[1].owner == &worker_tcb)
                          && (pi_mutexes[1].recursion_count == 1U)
                          && (pi_mutexes[1].is_handoff_pending != 0U)
                          && (owner->priority == owner->base_priority);
    RTOS_PORT_EXIT_CRITICAL();

    for (uint32_t tick = 0U; is_transferred && (tick < MUTEX_RELEASE_WAIT_TICKS); ++tick) {
        RTOS_PORT_ENTER_CRITICAL();
        bool is_done = worker_result.is_done;
        bool is_valid = worker_result.has_lock && (worker_result.take_status == RTOS_OK)
                        && (worker_result.give_status == RTOS_OK) && is_mutex_free(&pi_mutexes[1]);
        RTOS_PORT_EXIT_CRITICAL();
        if (is_done) {
            return is_valid;
        }
        rtos_task_delay(1U);
    }
    return false;
}

static void check_release_with_inheritance(void)
{
    rtos_tcb_t *owner = rtos_task_get_current();
    bool is_first_initialized = rtos_mutex_init(&pi_mutexes[0]) == RTOS_OK;
    bool is_second_initialized = rtos_mutex_init(&pi_mutexes[1]) == RTOS_OK;
    bool is_prepared = (owner != NULL) && (owner->base_priority > MUTEX_RELEASE_WORKER_PRIORITY)
                       && is_first_initialized && is_second_initialized
                       && (rtos_mutex_take(&pi_mutexes[0], RTOS_NO_WAIT) == RTOS_OK)
                       && (rtos_mutex_take(&pi_mutexes[0], RTOS_NO_WAIT) == RTOS_OK)
                       && (rtos_mutex_take(&pi_mutexes[1], RTOS_NO_WAIT) == RTOS_OK);
    bool is_worker_created = is_prepared
        && (rtos_task_create_static(&worker_tcb, worker_stack, MUTEX_RELEASE_STACK_WORDS,
                                   wait_for_mutex, NULL, MUTEX_RELEASE_WORKER_PRIORITY,
                                   "mrb-waiter") == RTOS_OK);
    bool is_inherited = is_worker_created && wait_until_inherited(owner);
    report_check("WAITING_ON_OTHER_LOCK", is_inherited);

    bool is_recursive = is_inherited && (rtos_mutex_give(&pi_mutexes[0]) == RTOS_OK)
                        && (pi_mutexes[0].owner == owner)
                        && (pi_mutexes[0].recursion_count == 1U);
    report_check("RECURSION_PRESERVED", is_recursive);

    RTOS_PORT_ENTER_CRITICAL();
    bool is_preserved = is_recursive && (pi_mutexes[0].wait_head == NULL)
                        && (rtos_mutex_give(&pi_mutexes[0]) == RTOS_OK)
                        && is_mutex_free(&pi_mutexes[0]) && check_waiter(owner);
    RTOS_PORT_EXIT_CRITICAL();
    report_check("OTHER_INHERITANCE_PRESERVED", is_preserved);
    report_check("CONTENDED_HANDOFF", is_preserved && check_handoff(owner));

    bool is_clean = !is_worker_created || (rtos_task_delete(&worker_tcb) == RTOS_OK);
    if (is_first_initialized) {
        is_clean = (rtos_mutex_deinit(&pi_mutexes[0]) == RTOS_OK) && is_clean;
    }
    if (is_second_initialized) {
        is_clean = (rtos_mutex_deinit(&pi_mutexes[1]) == RTOS_OK) && is_clean;
    }
    report_check("CLEANUP", is_clean && check_registry_count(0U));
}

void audit_pre_start(void)
{
}

void audit_regressions_task(void *argument)
{
    (void)argument;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    measure_release(1U);
    measure_release(RTOS_CONFIG_MAX_MUTEXES);
    check_release_with_inheritance();
    (void)printf("MRB|DONE|pass=%lu|fail=%lu\r\n", (unsigned long)(7U - failure_count),
                 (unsigned long)failure_count);
    for (;;) {
        rtos_task_delay(1000U);
    }
}
