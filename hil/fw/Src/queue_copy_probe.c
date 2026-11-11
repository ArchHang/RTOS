/**
 * @file queue_copy_probe.c
 * @brief Check queue copies across ring wrap, direct delivery and blocked sender handoff.
 */
#include "audit_regressions.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtos.h"

enum {
    QUEUE_COPY_CAPACITY = 3U,
    QUEUE_COPY_WIDE_SIZE = 5U,
    QUEUE_COPY_GUARD = 0xA5U,
    QUEUE_COPY_FIRST = 0x11U,
    QUEUE_COPY_SECOND = 0x22U,
    QUEUE_COPY_THIRD = 0x33U,
    QUEUE_COPY_FRONT = 0x44U,
    QUEUE_COPY_WAITING = 0x55U,
    QUEUE_COPY_STACK_WORDS = 256U,
    QUEUE_COPY_WORKER_PRIORITY = 6U
};

typedef struct queue_copy_job {
    rtos_status_t status;
    bool is_sender;
    _Alignas(4) uint8_t buffer[QUEUE_COPY_WIDE_SIZE + 2U];
} queue_copy_job_t;

static rtos_queue_t probe_queue;
static _Alignas(4) uint8_t probe_storage[QUEUE_COPY_CAPACITY * QUEUE_COPY_WIDE_SIZE + 2U];
static rtos_tcb_t worker_tcb;
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT) rtos_stack_t worker_stack[QUEUE_COPY_STACK_WORDS];
static queue_copy_job_t worker_job;
static uint32_t pass_count;
static uint32_t fail_count;

static uint8_t probe_value(uint8_t seed, uint32_t index)
{
    return (uint8_t)(seed + index);
}

static void prepare_buffer(uint8_t *buffer, uint32_t size, uint8_t seed)
{
    (void)memset(buffer, QUEUE_COPY_GUARD, QUEUE_COPY_WIDE_SIZE + 2U);
    for (uint32_t index = 0U; index < size; ++index) {
        buffer[index + 1U] = probe_value(seed, index);
    }
}

static bool check_buffer(const uint8_t *buffer, uint32_t size, uint8_t seed)
{
    if ((buffer[0] != QUEUE_COPY_GUARD) || (buffer[size + 1U] != QUEUE_COPY_GUARD)) {
        return false;
    }
    for (uint32_t index = 0U; index < size; ++index) {
        if (buffer[index + 1U] != probe_value(seed, index)) {
            return false;
        }
    }
    return true;
}

static bool send_probe(uint32_t size, uint8_t seed, rtos_bool_t to_front)
{
    _Alignas(4) uint8_t source[QUEUE_COPY_WIDE_SIZE + 2U];
    prepare_buffer(source, size, seed);
    return (rtos_queue_send(&probe_queue, &source[1], RTOS_NO_WAIT, to_front) == RTOS_OK)
           && check_buffer(source, size, seed);
}

static bool receive_probe(uint32_t size, uint8_t seed)
{
    _Alignas(4) uint8_t destination[QUEUE_COPY_WIDE_SIZE + 2U];
    (void)memset(destination, QUEUE_COPY_GUARD, sizeof(destination));
    return (rtos_queue_recv(&probe_queue, &destination[1], RTOS_NO_WAIT) == RTOS_OK)
           && check_buffer(destination, size, seed);
}

static bool initialize_probe(uint32_t size)
{
    (void)memset(probe_storage, QUEUE_COPY_GUARD, sizeof(probe_storage));
    return rtos_queue_init(&probe_queue, &probe_storage[1], size, QUEUE_COPY_CAPACITY) == RTOS_OK;
}

static bool finish_probe(uint32_t size)
{
    const bool is_guarded = (probe_storage[0] == QUEUE_COPY_GUARD)
                            && (probe_storage[1U + QUEUE_COPY_CAPACITY * size]
                                == QUEUE_COPY_GUARD);
    return (rtos_queue_deinit(&probe_queue) == RTOS_OK) && is_guarded;
}

static void run_worker(void *argument)
{
    queue_copy_job_t *job = (queue_copy_job_t *)argument;
    rtos_status_t status;
    if (job->is_sender) {
        status = rtos_queue_send(&probe_queue, &job->buffer[1], RTOS_WAIT_FOREVER, RTOS_TRUE);
    } else {
        status = rtos_queue_recv(&probe_queue, &job->buffer[1], RTOS_WAIT_FOREVER);
    }
    RTOS_PORT_ENTER_CRITICAL();
    job->status = status;
    RTOS_PORT_EXIT_CRITICAL();
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static bool start_worker(uint32_t size, bool is_sender)
{
    worker_job.is_sender = is_sender;
    worker_job.status = RTOS_ERR_PARAM;
    prepare_buffer(worker_job.buffer, size, QUEUE_COPY_WAITING);
    return rtos_task_create_static(&worker_tcb, worker_stack, QUEUE_COPY_STACK_WORDS,
                                   run_worker, &worker_job, QUEUE_COPY_WORKER_PRIORITY,
                                   "queue-copy") == RTOS_OK;
}

static bool worker_waits_on(rtos_wait_node_t **head)
{
    RTOS_PORT_ENTER_CRITICAL();
    const bool is_waiting = (worker_tcb.blocked_on == &probe_queue)
                            && (worker_tcb.wait_node.list_head == head);
    RTOS_PORT_EXIT_CRITICAL();
    return is_waiting;
}

static bool check_worker_result(uint32_t size, uint8_t expected)
{
    RTOS_PORT_ENTER_CRITICAL();
    const bool has_result = (worker_job.status == RTOS_OK)
                            && check_buffer(worker_job.buffer, size, expected);
    RTOS_PORT_EXIT_CRITICAL();
    return has_result;
}

static bool check_ring_wrap(uint32_t size)
{
    if (!initialize_probe(size)) {
        return false;
    }
    const bool is_valid = send_probe(size, QUEUE_COPY_FRONT, RTOS_TRUE)
                          && send_probe(size, QUEUE_COPY_FIRST, RTOS_FALSE)
                          && send_probe(size, QUEUE_COPY_SECOND, RTOS_FALSE)
                          && receive_probe(size, QUEUE_COPY_FRONT)
                          && receive_probe(size, QUEUE_COPY_FIRST)
                          && send_probe(size, QUEUE_COPY_THIRD, RTOS_FALSE)
                          && send_probe(size, QUEUE_COPY_FRONT, RTOS_TRUE)
                          && receive_probe(size, QUEUE_COPY_FRONT)
                          && receive_probe(size, QUEUE_COPY_SECOND)
                          && receive_probe(size, QUEUE_COPY_THIRD)
                          && (rtos_queue_get_count(&probe_queue) == 0U);
    return finish_probe(size) && is_valid;
}

static bool check_direct_receiver(uint32_t size)
{
    if (!initialize_probe(size)) {
        return false;
    }
    const bool is_created = start_worker(size, false);
    const bool is_valid = is_created && worker_waits_on(&probe_queue.recv_wait_head)
                          && send_probe(size, QUEUE_COPY_FIRST, RTOS_FALSE)
                          && check_worker_result(size, QUEUE_COPY_FIRST)
                          && (rtos_queue_get_count(&probe_queue) == 0U);
    const bool is_deleted = !is_created || (rtos_task_delete(&worker_tcb) == RTOS_OK);
    return finish_probe(size) && is_deleted && is_valid;
}

static bool check_reserved_sender(uint32_t size)
{
    if (!initialize_probe(size)) {
        return false;
    }
    const bool is_full = send_probe(size, QUEUE_COPY_FIRST, RTOS_FALSE)
                         && send_probe(size, QUEUE_COPY_SECOND, RTOS_FALSE)
                         && send_probe(size, QUEUE_COPY_THIRD, RTOS_FALSE);
    const bool is_created = is_full && start_worker(size, true);
    const bool is_valid = is_created && worker_waits_on(&probe_queue.send_wait_head)
                          && receive_probe(size, QUEUE_COPY_FIRST)
                          && check_worker_result(size, QUEUE_COPY_WAITING)
                          && (rtos_queue_get_count(&probe_queue) == QUEUE_COPY_CAPACITY)
                          && receive_probe(size, QUEUE_COPY_WAITING)
                          && receive_probe(size, QUEUE_COPY_SECOND)
                          && receive_probe(size, QUEUE_COPY_THIRD);
    const bool is_deleted = !is_created || (rtos_task_delete(&worker_tcb) == RTOS_OK);
    return finish_probe(size) && is_deleted && is_valid;
}

static void report_probe(const char *name, uint32_t size, bool is_valid)
{
    if (is_valid) {
        ++pass_count;
    } else {
        ++fail_count;
    }
    (void)printf("QCOPY|%s|%s|size=%lu\r\n", name, is_valid ? "PASS" : "FAIL",
                 (unsigned long)size);
}

void audit_pre_start(void)
{
}

void audit_regressions_task(void *argument)
{
    (void)argument;
    static const uint32_t item_sizes[] = {1U, QUEUE_COPY_WIDE_SIZE};
    for (uint32_t index = 0U; index < (sizeof(item_sizes) / sizeof(item_sizes[0])); ++index) {
        const uint32_t size = item_sizes[index];
        report_probe("ring_wrap", size, check_ring_wrap(size));
        report_probe("direct_receive", size, check_direct_receiver(size));
        report_probe("reserved_sender", size, check_reserved_sender(size));
    }
    (void)printf("QCOPY|DONE|pass=%lu|fail=%lu\r\n",
                 (unsigned long)pass_count, (unsigned long)fail_count);
    for (;;) {
        rtos_task_delay(1000U);
    }
}
