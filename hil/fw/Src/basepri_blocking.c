/**
 * @file basepri_blocking.c
 * @brief Real blocking-return checks with private objects and no possible giver.
 *
 * The audit task is the only caller of these objects. No IRQ or peer gives the
 * semaphore, consumes the full queue, produces into the empty queue, sets the
 * event, or notifies the task. Every finite wait must therefore expire. All
 * observations occur outside caller-owned critical sections and use the real
 * scheduler, SysTick, PendSV and production API paths, without port test hooks.
 *
 * A compiler barrier and volatile TCB reads preserve the post-call observation;
 * no hardware ISB is inserted between the tested API and that observation.
 * Queue receive timeouts preserve the output sentinel. notify_wait follows the
 * current API's semantics: it writes the stored notify_value, which is zero.
 */
#include "audit_regressions.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx_hal.h"

#include "production_highirq.h"
#include "rtos.h"

#if !defined(FIRMWARE_BASEPRI_BLOCKING) || !defined(FIRMWARE_AUDIT)
#error "basepri_blocking.c requires the dedicated audit-entry blocking variant"
#endif

#if defined(RTOS_HIL_CRITICAL_TEST)
#error "Blocking-return checks must not contain port test hooks"
#endif

#ifndef BASEPRI_BLOCKING_ROUNDS
#define BASEPRI_BLOCKING_ROUNDS (100U)
#endif
#ifndef BASEPRI_BLOCKING_HIGHIRQ
#define BASEPRI_BLOCKING_HIGHIRQ (1U)
#endif

#define BLOCKING_TIMEOUT_TICKS (2U)
#define BLOCKING_ERROR_DETAILS (10U)
#define BLOCKING_SENTINEL (0xC35AA53CU)
#define BLOCKING_QUEUED_WORD (0xA55AA55AU)
#define BLOCKING_SEND_WORD (0x5AA55AA5U)
#define BLOCKING_GUARD_PREFIX (0x12345678U)
#define BLOCKING_GUARD_SUFFIX (0x87654321U)

_Static_assert(BASEPRI_BLOCKING_ROUNDS > 0U, "Blocking test requires at least one round");

typedef enum blocking_kind {
    BLOCKING_SEM = 0,
    BLOCKING_QUEUE_RECV,
    BLOCKING_QUEUE_SEND,
    BLOCKING_EVENT,
    BLOCKING_NOTIFY,
    BLOCKING_KIND_COUNT
} blocking_kind_t;

typedef struct blocking_word {
    uint32_t prefix;
    uint32_t value;
    uint32_t suffix;
} blocking_word_t;

typedef struct blocking_sample {
    int32_t result;
    int32_t wait_result;
    uint32_t elapsed;
    uint32_t output;
    uint32_t state;
    uint32_t timed_out;
    uint32_t basepri;
    uint32_t primask;
    uint32_t nesting;
    uint32_t resource_valid;
} blocking_sample_t;

typedef struct blocking_stats {
    uint32_t attempts;
    uint32_t failures;
    uint32_t unexpected_result;
    uint32_t false_success;
    uint32_t early_return;
    uint32_t wait_result_errors;
    uint32_t resource_errors;
    uint32_t state_errors;
    uint32_t mask_errors;
    uint32_t elapsed_min;
    uint32_t elapsed_max;
} blocking_stats_t;

static const char *const kind_names[BLOCKING_KIND_COUNT] = {
    "sem", "queue_recv", "queue_send", "event", "notify"
};
static rtos_sem_t empty_sem;
static rtos_queue_t empty_queue;
static rtos_queue_t full_queue;
static rtos_event_t empty_event;
static uint32_t empty_storage;
static uint32_t full_storage;
static rtos_tcb_t *test_task;
static uint32_t initialized;
static uint32_t pass_count;
static uint32_t fail_count;
static uint32_t attempts;

static void check_result(const char *name, uint32_t is_valid)
{
    if (is_valid != 0U) {
        pass_count++;
    } else {
        fail_count++;
    }
    (void)printf("BLOCK|CHECK|%s|%s\r\n", name, is_valid != 0U ? "PASS" : "FAIL");
}

static uint32_t word_is_valid(const blocking_word_t *word, uint32_t expected)
{
    return (word->prefix == BLOCKING_GUARD_PREFIX) && (word->value == expected)
        && (word->suffix == BLOCKING_GUARD_SUFFIX);
}

static void capture_sample(uint32_t started, blocking_sample_t *sample)
{
    volatile const rtos_tcb_t *current = test_task;

    __asm__ __volatile__("" ::: "memory");
    sample->wait_result = (int32_t)current->wait_result;
    sample->state = (uint32_t)current->state;
    sample->timed_out = (uint32_t)current->wait_node.timed_out;
    /* Tick subtraction deliberately uses modulo arithmetic across tick wrap. */
    sample->elapsed = (uint32_t)(rtos_sched_get_tick_count() - started);
    sample->basepri = __get_BASEPRI();
    sample->primask = __get_PRIMASK();
    sample->nesting = rtos_port_crit_nest;
}

static void run_sample(blocking_kind_t kind, uint32_t round, blocking_sample_t *sample)
{
    blocking_word_t output = {BLOCKING_GUARD_PREFIX, BLOCKING_SENTINEL,
                              BLOCKING_GUARD_SUFFIX};
    blocking_word_t input = {BLOCKING_GUARD_PREFIX, BLOCKING_SEND_WORD,
                             BLOCKING_GUARD_SUFFIX};
    uint32_t started = rtos_sched_get_tick_count();

    switch (kind) {
        case BLOCKING_SEM:
            sample->result = (int32_t)rtos_sem_take(&empty_sem, BLOCKING_TIMEOUT_TICKS);
            break;
        case BLOCKING_QUEUE_RECV:
            sample->result = (int32_t)rtos_queue_recv(&empty_queue, &output.value,
                BLOCKING_TIMEOUT_TICKS);
            break;
        case BLOCKING_QUEUE_SEND:
            sample->result = (int32_t)rtos_queue_send(&full_queue, &input.value,
                BLOCKING_TIMEOUT_TICKS, (round & 1U) != 0U ? RTOS_TRUE : RTOS_FALSE);
            break;
        case BLOCKING_EVENT:
            sample->result = (int32_t)rtos_event_wait(&empty_event, 3U,
                (round & 1U) != 0U ? RTOS_EVENT_WAIT_ALL : RTOS_EVENT_WAIT_ANY,
                BLOCKING_TIMEOUT_TICKS);
            break;
        case BLOCKING_NOTIFY:
            sample->result = (int32_t)rtos_task_notify_wait(BLOCKING_TIMEOUT_TICKS,
                &output.value);
            break;
        default:
            sample->result = (int32_t)RTOS_ERR_PARAM;
            break;
    }
    capture_sample(started, sample);
    sample->output = output.value;
    switch (kind) {
        case BLOCKING_SEM:
            sample->resource_valid = (empty_sem.count == 0U)
                && (empty_sem.wait_head == NULL) && (empty_sem.wait_bitmap == 0U);
            break;
        case BLOCKING_QUEUE_RECV:
            sample->resource_valid = (empty_queue.count == 0U)
                && (empty_queue.recv_wait_head == NULL) && (empty_queue.send_wait_head == NULL)
                && word_is_valid(&output, BLOCKING_SENTINEL);
            break;
        case BLOCKING_QUEUE_SEND:
            sample->resource_valid = (full_queue.count == 1U)
                && (full_queue.recv_wait_head == NULL) && (full_queue.send_wait_head == NULL)
                && (full_storage == BLOCKING_QUEUED_WORD)
                && word_is_valid(&input, BLOCKING_SEND_WORD);
            break;
        case BLOCKING_EVENT:
            sample->resource_valid = (empty_event.bits == 0U)
                && (empty_event.wait_head == NULL);
            break;
        case BLOCKING_NOTIFY:
            sample->resource_valid = (test_task->notify_value == 0U)
                && word_is_valid(&output, 0U);
            break;
        default:
            sample->resource_valid = 0U;
            break;
    }
}

static uint32_t record_sample(blocking_kind_t kind, const blocking_sample_t *sample,
                               blocking_stats_t *stats)
{
    int32_t expected = kind == BLOCKING_EVENT ? 0 : (int32_t)RTOS_ERR_TIMEOUT;
    uint32_t previous_failures = stats->failures;
    uint32_t mask_valid = (sample->basepri == 0U) && (sample->primask == 0U)
        && (sample->nesting == 0U);

    stats->attempts++;
    attempts++;
    stats->unexpected_result += sample->result != expected ? 1U : 0U;
    stats->false_success += ((kind != BLOCKING_EVENT) && (sample->result == RTOS_OK)
        && (sample->wait_result == RTOS_ERR_TIMEOUT)) ? 1U : 0U;
    stats->early_return += sample->elapsed < BLOCKING_TIMEOUT_TICKS ? 1U : 0U;
    stats->wait_result_errors += (sample->wait_result != RTOS_ERR_TIMEOUT)
        || (sample->timed_out == 0U) ? 1U : 0U;
    stats->resource_errors += sample->resource_valid == 0U ? 1U : 0U;
    stats->state_errors += sample->state != RTOS_TASK_RUNNING ? 1U : 0U;
    stats->mask_errors += mask_valid == 0U ? 1U : 0U;
    if (sample->elapsed < stats->elapsed_min) {
        stats->elapsed_min = sample->elapsed;
    }
    if (sample->elapsed > stats->elapsed_max) {
        stats->elapsed_max = sample->elapsed;
    }
    if ((sample->result != expected) || (sample->elapsed < BLOCKING_TIMEOUT_TICKS)
        || (sample->wait_result != RTOS_ERR_TIMEOUT) || (sample->timed_out == 0U)
        || (sample->resource_valid == 0U) || (sample->state != RTOS_TASK_RUNNING)
        || (mask_valid == 0U)) {
        stats->failures++;
        fail_count++;
    } else {
        pass_count++;
    }
    return stats->failures != previous_failures ? 1U : 0U;
}

static void print_failure(blocking_kind_t kind, uint32_t round,
                           const blocking_sample_t *sample)
{
    (void)printf("BLOCK|CASE|FAIL|kind=%s round=%lu ret=%ld expected=%ld elapsed=%lu "
                 "wait_result=%ld timed_out=%lu state=%lu out=0x%08lX "
                 "resource_valid=%lu basepri=0x%02lX primask=%lu nest=%lu\r\n",
                 kind_names[kind], (unsigned long)round, (long)sample->result,
                 kind == BLOCKING_EVENT ? 0L : (long)RTOS_ERR_TIMEOUT,
                 (unsigned long)sample->elapsed, (long)sample->wait_result,
                 (unsigned long)sample->timed_out, (unsigned long)sample->state,
                 (unsigned long)sample->output, (unsigned long)sample->resource_valid,
                 (unsigned long)sample->basepri, (unsigned long)sample->primask,
                 (unsigned long)sample->nesting);
}

static void run_kind(blocking_kind_t kind)
{
    blocking_stats_t stats;
    blocking_sample_t sample;

    memset(&stats, 0, sizeof(stats));
    stats.elapsed_min = UINT32_MAX;
    for (uint32_t round = 0U; round < BASEPRI_BLOCKING_ROUNDS; round++) {
        run_sample(kind, round, &sample);
        if ((record_sample(kind, &sample, &stats) != 0U)
            && (stats.failures <= BLOCKING_ERROR_DETAILS)) {
            print_failure(kind, round, &sample);
        }
    }
    (void)printf("BLOCK|KIND|%s|attempts=%lu fail=%lu unexpected_ret=%lu false_ok=%lu "
                 "early=%lu wait_errors=%lu resource_errors=%lu state_errors=%lu "
                 "mask_errors=%lu elapsed_min=%lu elapsed_max=%lu\r\n",
                 kind_names[kind], (unsigned long)stats.attempts,
                 (unsigned long)stats.failures, (unsigned long)stats.unexpected_result,
                 (unsigned long)stats.false_success, (unsigned long)stats.early_return,
                 (unsigned long)stats.wait_result_errors, (unsigned long)stats.resource_errors,
                 (unsigned long)stats.state_errors, (unsigned long)stats.mask_errors,
                 (unsigned long)stats.elapsed_min, (unsigned long)stats.elapsed_max);
}

static void initialize_objects(void)
{
    uint32_t queued_word = BLOCKING_QUEUED_WORD;

    if (rtos_sem_init(&empty_sem, 0U, 1U) == RTOS_OK) {
        initialized |= 1U << BLOCKING_SEM;
    }
    if (rtos_queue_init(&empty_queue, &empty_storage, sizeof(empty_storage), 1U) == RTOS_OK) {
        initialized |= 1U << BLOCKING_QUEUE_RECV;
    }
    if (rtos_queue_init(&full_queue, &full_storage, sizeof(full_storage), 1U) == RTOS_OK) {
        initialized |= 1U << BLOCKING_QUEUE_SEND;
        check_result("full_queue_preload", rtos_queue_send(&full_queue, &queued_word,
            RTOS_NO_WAIT, RTOS_FALSE) == RTOS_OK);
    }
    if (rtos_event_init(&empty_event) == RTOS_OK) {
        initialized |= 1U << BLOCKING_EVENT;
    }
    if ((test_task->notify_value == 0U) && (test_task->notify_state == 0U)) {
        initialized |= 1U << BLOCKING_NOTIFY;
    }
}

static void cleanup_objects(void)
{
    if ((initialized & (1U << BLOCKING_SEM)) != 0U) {
        check_result("sem_cleanup", rtos_sem_deinit(&empty_sem) == RTOS_OK);
    }
    if ((initialized & (1U << BLOCKING_QUEUE_RECV)) != 0U) {
        check_result("empty_queue_cleanup", rtos_queue_deinit(&empty_queue) == RTOS_OK);
    }
    if ((initialized & (1U << BLOCKING_QUEUE_SEND)) != 0U) {
        check_result("full_queue_cleanup", rtos_queue_deinit(&full_queue) == RTOS_OK);
    }
    if ((initialized & (1U << BLOCKING_EVENT)) != 0U) {
        check_result("event_cleanup", rtos_event_deinit(&empty_event) == RTOS_OK);
    }
}

static void stop_highirq(void)
{
    production_highirq_snapshot_t irq;
    rtos_status_t status = production_highirq_stop();

    check_result("highirq_stop", status == RTOS_OK);
    status = production_highirq_snapshot(&irq);
    check_result("highirq_snapshot", status == RTOS_OK);
    if (status == RTOS_OK) {
        (void)printf("BLOCK|HIGHIRQ|entries=%lu natural=%lu nonzero_samples=%lu errors=%lu "
                     "restored=%lu mismatch=0x%08lX pending_before=%lu pending_stop=%lu "
                     "pending_restored=%lu sr=0x%lX reload_polls=%lu/%lu "
                     "reload_timeouts=%lu\r\n", (unsigned long)irq.entries,
                     (unsigned long)irq.natural_updates,
                     (unsigned long)irq.nonzero_basepri_samples, (unsigned long)irq.errors,
                     (unsigned long)irq.restored, (unsigned long)irq.restore_mismatch,
                     (unsigned long)irq.stop_pending_before, (unsigned long)irq.stop_pending_after,
                     (unsigned long)irq.restore_pending_after,
                     (unsigned long)irq.restore_sr_actual,
                     (unsigned long)irq.configure_reload_polls,
                     (unsigned long)irq.restore_reload_polls,
                     (unsigned long)irq.reload_timeouts);
        check_result("highirq_real_load", (irq.entries != 0U) && (irq.natural_updates != 0U));
    }
}

/** @brief Report the dedicated fixture configuration before scheduler start. */
void audit_pre_start(void)
{
    (void)printf("BLOCK|META|rounds_per_kind=%lu timeout_ticks=%lu highirq=%lu "
                 "givers=0 setters=0 notifiers=0 port_hooks=0 "
                 "notify_timeout_output=stored_zero\r\n",
                 (unsigned long)BASEPRI_BLOCKING_ROUNDS,
                 (unsigned long)BLOCKING_TIMEOUT_TICKS, (unsigned long)BASEPRI_BLOCKING_HIGHIRQ);
}

/**
 * @brief Run five classes of real timeout returns and report their observations.
 * @param arg [in] Unused audit-task argument; may be NULL.
 * @return Does not return; waits after reporting BLOCK|DONE.
 */
void audit_regressions_task(void *arg)
{
    uint32_t highirq_started = 0U;

    (void)arg;
    test_task = rtos_task_get_current();
    check_result("task_context", (test_task != NULL) && (__get_IPSR() == 0U)
        && (__get_BASEPRI() == 0U) && (__get_PRIMASK() == 0U)
        && (rtos_port_crit_nest == 0U));
    if (test_task != NULL) {
        initialize_objects();
#if BASEPRI_BLOCKING_HIGHIRQ
        highirq_started = production_highirq_start() == RTOS_OK ? 1U : 0U;
        check_result("highirq_start", highirq_started);
#endif
        for (uint32_t kind = 0U; kind < BLOCKING_KIND_COUNT; kind++) {
            check_result(kind_names[kind], (initialized & (1U << kind)) != 0U);
            if ((initialized & (1U << kind)) != 0U) {
                run_kind((blocking_kind_t)kind);
            }
        }
        if (highirq_started != 0U) {
            stop_highirq();
        }
        cleanup_objects();
    }
    check_result("all_attempts_completed", attempts == BASEPRI_BLOCKING_ROUNDS * 5U);
    (void)printf("BLOCK|SUMMARY|pass=%lu fail=%lu attempts=%lu\r\n",
                 (unsigned long)pass_count, (unsigned long)fail_count, (unsigned long)attempts);
    (void)printf("BLOCK|DONE|status=%lu\r\n", fail_count == 0U ? 0UL : 1UL);
    for (;;) {
        rtos_task_delay(1000U);
    }
}
