/*
 * Mutex priority-computation probes. Every wait node belongs to a real,
 * registered task that called mutex_take. The measured helper is read-only;
 * task construction, critical entry/exit, statistics and printing are outside
 * its t0/t1 interval. The same fixture runs with both kernel versions.
 */
#include "perf_probes.h"
#include "rtos.h"
#include "rtos_internal.h"
#include "stm32f4xx.h"
#include <stdio.h>

enum {
    PROBE_WAITERS = 8U,
    PROBE_STACK_WORDS = 256U,
    PROBE_SAMPLES = 1000U,
    PROBE_WAIT_TICKS = 100U,
    PROBE_OWNER_BASE = 20U
};

typedef struct {
    rtos_mutex_t *mutex;
    volatile rtos_status_t take_result;
    volatile rtos_status_t give_result;
    volatile uint32_t done;
} probe_waiter_arg_t;

static rtos_tcb_t s_waiter[PROBE_WAITERS];
static rtos_stack_t s_stack[PROBE_WAITERS][PROBE_STACK_WORDS] __attribute__((aligned(8)));
static probe_waiter_arg_t s_arg[PROBE_WAITERS];
static rtos_bool_t s_created[PROBE_WAITERS];
static rtos_mutex_t s_lock[2];
static rtos_bool_t s_initialized[2];
static uint32_t s_pass, s_fail;

static void probe_check(const char *name, rtos_bool_t condition)
{
    if (condition) s_pass++;
    else s_fail++;
    printf("PC|%s|%s\r\n", name, condition ? "PASS" : "FAIL");
}

static void probe_waiter(void *opaque)
{
    probe_waiter_arg_t *arg = (probe_waiter_arg_t *)opaque;
    arg->take_result = rtos_mutex_take(arg->mutex, RTOS_WAIT_FOREVER);
    if (arg->take_result == RTOS_OK) {
        arg->give_result = rtos_mutex_give(arg->mutex);
    }
    arg->done = 1U;
    /* The controller deletes this static task after checking its outcome. */
    for (;;) rtos_task_delay(1000U);
}

static rtos_bool_t probe_wait_blocked(uint32_t slot, rtos_mutex_t *mutex)
{
    for (uint32_t tick = 0U; tick < PROBE_WAIT_TICKS; tick++) {
        RTOS_PORT_ENTER_CRITICAL();
        rtos_bool_t blocked = s_created[slot] &&
            s_waiter[slot].blocked_on == (void *)mutex &&
            s_waiter[slot].wait_node.list_head == &mutex->wait_head &&
            (s_waiter[slot].state == RTOS_TASK_BLOCKED ||
             s_waiter[slot].state == RTOS_TASK_DELAYED);
        RTOS_PORT_EXIT_CRITICAL();
        if (blocked) return RTOS_TRUE;
        /* Later waiters can have lower priority than the inherited owner. */
        rtos_task_delay(1U);
    }
    return RTOS_FALSE;
}

static rtos_bool_t probe_create_waiter(uint32_t slot, uint32_t lock, rtos_prio_t priority)
{
    if (s_created[slot]) return RTOS_FALSE;
    s_arg[slot].mutex = &s_lock[lock];
    s_arg[slot].take_result = RTOS_ERR_PARAM;
    s_arg[slot].give_result = RTOS_ERR_PARAM;
    s_arg[slot].done = 0U;
    rtos_status_t status = rtos_task_create_static(&s_waiter[slot], s_stack[slot],
        PROBE_STACK_WORDS, probe_waiter, &s_arg[slot], priority, "perf-waiter");
    s_created[slot] = status == RTOS_OK;
    return s_created[slot] && probe_wait_blocked(slot, &s_lock[lock]);
}

static rtos_bool_t probe_init_owned(uint32_t lock, rtos_tcb_t *owner)
{
    if (s_initialized[lock]) return RTOS_FALSE;
    rtos_status_t status = rtos_mutex_init(&s_lock[lock]);
    s_initialized[lock] = status == RTOS_OK;
    if (status != RTOS_OK || rtos_mutex_take(&s_lock[lock], RTOS_NO_WAIT) != RTOS_OK) {
        return RTOS_FALSE;
    }
    return s_lock[lock].owner == owner && s_lock[lock].recursion_count == 1U;
}

/* Caller holds critical. Limit the traversal so corrupt links produce a
 * reported failure instead of making the test runner spin forever. */
static rtos_bool_t probe_sorted(rtos_mutex_t *mutex, uint32_t expected_count)
{
    rtos_wait_node_t *previous = NULL;
    rtos_prio_t previous_priority = 0U;
    uint32_t count = 0U;
    for (rtos_wait_node_t *node = mutex->wait_head; node != NULL; node = node->next) {
        if (++count > PROBE_WAITERS || node->tcb == NULL ||
            node != &node->tcb->wait_node || node->prev != previous ||
            node->wait_obj != (void *)mutex || node->list_head != &mutex->wait_head ||
            node->tcb->blocked_on != (void *)mutex || node->queue_bitmap != NULL ||
            node->queued_priority != node->tcb->priority ||
            (previous != NULL && previous_priority > node->tcb->priority)) {
            return RTOS_FALSE;
        }
        previous_priority = node->tcb->priority;
        previous = node;
    }
    return count == expected_count;
}

static rtos_bool_t probe_priority(rtos_tcb_t *owner, rtos_prio_t expected,
                                  uint32_t count0, uint32_t count1)
{
    RTOS_PORT_ENTER_CRITICAL();
    rtos_bool_t valid = owner->base_priority == PROBE_OWNER_BASE &&
        owner->priority == expected &&
        rtos_internal_mutex_compute_effective_priority(owner) == expected &&
        probe_sorted(&s_lock[0], count0) && probe_sorted(&s_lock[1], count1);
    RTOS_PORT_EXIT_CRITICAL();
    return valid;
}

static void probe_cleanup(const char *name)
{
    rtos_bool_t ok = RTOS_TRUE;
    /* Delete waiters before deinitializing their objects. This also exercises
     * priority-inheritance rollback, and never reuses a live static TCB. */
    for (uint32_t slot = 0U; slot < PROBE_WAITERS; slot++) {
        if (!s_created[slot]) continue;
        rtos_status_t status = rtos_task_delete(&s_waiter[slot]);
        if (status == RTOS_OK) s_created[slot] = RTOS_FALSE;
        else ok = RTOS_FALSE;
    }
    for (uint32_t lock = 0U; lock < 2U; lock++) {
        if (!s_initialized[lock]) continue;
        rtos_status_t status = rtos_mutex_deinit(&s_lock[lock]);
        if (status == RTOS_OK) s_initialized[lock] = RTOS_FALSE;
        else ok = RTOS_FALSE;
    }
    probe_check(name, ok);
}

static void probe_measure(const char *name, rtos_tcb_t *owner, rtos_prio_t expected,
                           rtos_bool_t prepared)
{
    uint32_t minimum = UINT32_MAX, maximum = 0U;
    uint64_t sum = 0U;
    rtos_bool_t exact = RTOS_TRUE;
    if (!prepared) {
        printf("PB|%s|N=0|avg=0|min=0|max=0|status=SKIP\r\n", name);
        probe_check(name, RTOS_FALSE);
        return;
    }
    for (uint32_t sample = 0U; sample < PROBE_SAMPLES; sample++) {
        RTOS_PORT_ENTER_CRITICAL();
        uint32_t t0 = DWT->CYCCNT;
        rtos_prio_t actual = rtos_internal_mutex_compute_effective_priority(owner);
        uint32_t t1 = DWT->CYCCNT;
        RTOS_PORT_EXIT_CRITICAL();
        uint32_t elapsed = t1 - t0;
        if (elapsed < minimum) minimum = elapsed;
        if (elapsed > maximum) maximum = elapsed;
        sum += elapsed;
        if (actual != expected) exact = RTOS_FALSE;
    }
    printf("PB|%s|N=%lu|avg=%lu|min=%lu|max=%lu\r\n", name,
        (unsigned long)PROBE_SAMPLES, (unsigned long)(sum / PROBE_SAMPLES),
        (unsigned long)minimum, (unsigned long)maximum);
    probe_check(name, exact);
}

static void probe_one_lock(rtos_tcb_t *owner, uint32_t count, const char *name,
                            const char *prepare_name, const char *cleanup_name)
{
    /* Arrival order deliberately differs from priority order. Priority 6 is
     * included in every case so owner effective priority is always known. */
    static const rtos_prio_t priorities[PROBE_WAITERS] = { 12U, 6U, 11U, 8U,
                                                          10U, 7U, 13U, 9U };
    rtos_bool_t prepared = probe_init_owned(0U, owner);
    rtos_prio_t expected = PROBE_OWNER_BASE;
    if (prepared) {
        for (uint32_t slot = 0U; slot < count; slot++) {
            rtos_prio_t priority = count == 1U ? 6U : priorities[slot];
            if (priority < expected) expected = priority;
            if (!probe_create_waiter(slot, 0U, priority)) {
                prepared = RTOS_FALSE;
                break;
            }
        }
    }
    RTOS_PORT_ENTER_CRITICAL();
    prepared = prepared && s_lock[0].owner == owner &&
        owner->priority == expected && probe_sorted(&s_lock[0], count) &&
        s_lock[0].wait_head != NULL && s_lock[0].wait_head->tcb->priority == expected;
    RTOS_PORT_EXIT_CRITICAL();
    probe_check(prepare_name, prepared);
    probe_measure(name, owner, expected, prepared);
    probe_cleanup(cleanup_name);
}

static rtos_bool_t probe_wait_done(uint32_t slot)
{
    for (uint32_t tick = 0U; tick < PROBE_WAIT_TICKS; tick++) {
        if (s_arg[slot].done) {
            return s_arg[slot].take_result == RTOS_OK &&
                   s_arg[slot].give_result == RTOS_OK;
        }
        rtos_task_delay(1U);
    }
    return RTOS_FALSE;
}

static void probe_multilock(rtos_tcb_t *owner)
{
    rtos_bool_t prepared = probe_init_owned(0U, owner) && probe_init_owned(1U, owner);
    if (prepared) prepared = probe_create_waiter(0U, 0U, 12U); /* A */
    if (prepared) prepared = probe_create_waiter(1U, 0U, 8U);  /* B */
    if (prepared) prepared = probe_create_waiter(2U, 0U, 10U); /* C */
    if (prepared) prepared = probe_create_waiter(3U, 1U, 6U);  /* D */
    probe_check("multilock_sorted_initial", prepared && probe_priority(owner, 6U, 3U, 1U));
    if (!prepared) {
        probe_cleanup("multilock_cleanup");
        return;
    }

    rtos_status_t status = rtos_task_set_priority(&s_waiter[1], 14U);
    probe_check("multilock_lower_waiter", status == RTOS_OK &&
                probe_priority(owner, 6U, 3U, 1U));
    status = rtos_task_set_priority(&s_waiter[3], 16U);
    probe_check("multilock_other_head_selected", status == RTOS_OK &&
                probe_priority(owner, 10U, 3U, 1U));
    status = rtos_task_set_priority(&s_waiter[0], 10U);
    probe_check("multilock_raise_waiter", status == RTOS_OK &&
                probe_priority(owner, 10U, 3U, 1U));
    status = rtos_task_set_priority(&s_waiter[2], 10U); /* effective no-op */
    RTOS_PORT_ENTER_CRITICAL();
    rtos_bool_t fifo = s_lock[0].wait_head == &s_waiter[2].wait_node &&
        s_waiter[2].wait_node.next == &s_waiter[0].wait_node;
    RTOS_PORT_EXIT_CRITICAL();
    probe_check("multilock_same_priority_fifo", status == RTOS_OK && fifo &&
                probe_priority(owner, 10U, 3U, 1U));

    status = rtos_task_suspend(&s_waiter[2]);
    probe_check("multilock_suspend_shared_top", status == RTOS_OK &&
                s_waiter[2].state == RTOS_TASK_SUSPENDED &&
                probe_priority(owner, 10U, 2U, 1U));
    status = rtos_task_delete(&s_waiter[0]);
    if (status == RTOS_OK) s_created[0] = RTOS_FALSE;
    probe_check("multilock_delete_top_rollback", status == RTOS_OK &&
                probe_priority(owner, 14U, 1U, 1U));

    status = rtos_mutex_give(&s_lock[0]);
    rtos_bool_t handed_off = status == RTOS_OK && probe_wait_done(1U);
    probe_check("multilock_release_keeps_other_boost", handed_off &&
                probe_priority(owner, 16U, 0U, 1U));
    status = rtos_task_suspend(&s_waiter[3]);
    probe_check("multilock_suspend_top_restores_base", status == RTOS_OK &&
                probe_priority(owner, PROBE_OWNER_BASE, 0U, 0U));
    status = rtos_mutex_give(&s_lock[1]);
    probe_check("multilock_final_base", status == RTOS_OK &&
                s_lock[0].owner == NULL && s_lock[1].owner == NULL &&
                probe_priority(owner, PROBE_OWNER_BASE, 0U, 0U));
    probe_cleanup("multilock_cleanup");
}

void perf_probes_run(void)
{
    s_pass = 0U;
    s_fail = 0U;
    rtos_tcb_t *owner = rtos_task_get_current();
    if (owner == NULL || RTOS_PORT_IN_ISR()) {
        probe_check("runner_context", RTOS_FALSE);
        printf("PC|SUMMARY|pass=%lu fail=%lu\r\n", (unsigned long)s_pass,
               (unsigned long)s_fail);
        return;
    }
    rtos_prio_t original_base = owner->base_priority;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    rtos_status_t status = rtos_task_set_priority(owner, PROBE_OWNER_BASE);
    probe_check("owner_base_set", status == RTOS_OK && owner->base_priority == PROBE_OWNER_BASE);
    if (status == RTOS_OK) {
        probe_one_lock(owner, 1U, "mutex_compute_w01", "mutex_w01_prepared", "mutex_w01_cleanup");
        probe_one_lock(owner, 4U, "mutex_compute_w04", "mutex_w04_prepared", "mutex_w04_cleanup");
        probe_one_lock(owner, 8U, "mutex_compute_w08", "mutex_w08_prepared", "mutex_w08_cleanup");
        probe_multilock(owner);
    }
    status = rtos_task_set_priority(owner, original_base);
    probe_check("owner_base_restored", status == RTOS_OK && owner->base_priority == original_base);
    printf("PC|SUMMARY|pass=%lu fail=%lu\r\n", (unsigned long)s_pass,
           (unsigned long)s_fail);
}
