/**
 * @file f446_usb_msc_resource.c
 * @brief Exercise composite USB startup with actual RTOS resources on the F446.
 *
 * The controller entry points are replaced by linker wraps. USB object pools,
 * queue and task lifecycle, and MSC dispatch are production implementations.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtos.h"

#if !RTOS_CONFIG_USE_USB || !RTOS_CONFIG_USB_USE_MSC || !RTOS_CONFIG_USE_PERF_MONITOR
#error "The MSC resource fixture requires USB MSC and the performance registry"
#endif

enum {
    MSC_TEST_BUS = 0U,
    MSC_TEST_HOLDER_PRIORITY = 20U,
    MSC_TEST_WORKER_TICKS = 2U
};

static uint32_t s_controller_starts;
static uint32_t s_controller_stops;
static uint32_t s_passes;
static uint32_t s_failures;
static bool s_reject_controller;
static bool s_startup_passed;
static rtos_perf_task_t s_task_stats[RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE];

int __wrap_usb_dc_init(uint8_t busid);
int __wrap_usb_dc_deinit(uint8_t busid);
void f446_usb_msc_resource_prepare(void);
void f446_usb_msc_resource_run(void *argument);

int __wrap_usb_dc_init(uint8_t busid)
{
    ++s_controller_starts;
    return (busid == MSC_TEST_BUS && !s_reject_controller) ? 0 : -1;
}

int __wrap_usb_dc_deinit(uint8_t busid)
{
    ++s_controller_stops;
    return (busid == MSC_TEST_BUS) ? 0 : -1;
}

static void record_check(const char *name, bool is_passed)
{
    if (is_passed) {
        ++s_passes;
    } else {
        ++s_failures;
    }
    /* Continue collecting evidence if the fixture cannot report a line. */
    (void)printf("UMSC|CHECK|%s|%s\r\n", name, is_passed ? "PASS" : "FAIL");
}

static void wait_as_holder(void *argument)
{
    (void)argument;
    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}

static bool hold_thread_slots(rtos_usb_thread_t **threads)
{
    bool has_all_slots = true;
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_THREAD_COUNT; ++index) {
        threads[index] = rtos_usb_thread_create("hold-msc", 0U, MSC_TEST_HOLDER_PRIORITY,
                                                wait_as_holder, NULL);
        has_all_slots = (threads[index] != NULL) && has_all_slots;
    }
    rtos_usb_thread_t *const extra = rtos_usb_thread_create("extra-msc", 0U,
        MSC_TEST_HOLDER_PRIORITY, wait_as_holder, NULL);
    if (extra != NULL) {
        rtos_usb_thread_delete(extra);
    }
    return has_all_slots && extra == NULL;
}

static void release_thread_slots(rtos_usb_thread_t **threads)
{
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_THREAD_COUNT; ++index) {
        /* A NULL USB thread handle requests self-delete. */
        if (threads[index] != NULL) {
            rtos_usb_thread_delete(threads[index]);
        }
    }
}

static uintptr_t queue_sentinel(uint32_t index)
{
    return (uintptr_t)(UINT32_C(0xA55A0000) + index);
}

static bool hold_queue_slots(rtos_usb_mq_t **queues, bool has_sentinels)
{
    bool has_all_slots = true;
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_MQ_COUNT; ++index) {
        queues[index] = rtos_usb_mq_create(1U);
        if (queues[index] == NULL) {
            has_all_slots = false;
        } else if (has_sentinels) {
            const rtos_status_t send = rtos_usb_mq_send(queues[index], queue_sentinel(index),
                                                       RTOS_NO_WAIT);
            has_all_slots = (send == RTOS_OK) && has_all_slots;
        }
    }
    rtos_usb_mq_t *const extra = rtos_usb_mq_create(1U);
    rtos_usb_mq_delete(extra);
    return has_all_slots && extra == NULL;
}

static void release_queue_slots(rtos_usb_mq_t **queues)
{
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_MQ_COUNT; ++index) {
        rtos_usb_mq_delete(queues[index]);
    }
}

static bool check_queue_slots_available(void)
{
    rtos_usb_mq_t *queues[RTOS_CONFIG_USB_OSAL_MQ_COUNT];
    const bool has_all_slots = hold_queue_slots(queues, false);
    release_queue_slots(queues);
    return has_all_slots;
}

static bool check_sync_slots_available(void)
{
    rtos_usb_sem_t *semaphores[RTOS_CONFIG_USB_OSAL_SEM_COUNT];
    rtos_usb_mutex_t *mutexes[RTOS_CONFIG_USB_OSAL_MUTEX_COUNT];
    bool has_all_slots = true;

    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_SEM_COUNT; ++index) {
        semaphores[index] = rtos_usb_sem_create(0U, 1U);
        has_all_slots = (semaphores[index] != NULL) && has_all_slots;
    }
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_MUTEX_COUNT; ++index) {
        mutexes[index] = rtos_usb_mutex_create();
        has_all_slots = (mutexes[index] != NULL) && has_all_slots;
    }
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_SEM_COUNT; ++index) {
        rtos_usb_sem_delete(semaphores[index]);
    }
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_MUTEX_COUNT; ++index) {
        rtos_usb_mutex_delete(mutexes[index]);
    }
    return has_all_slots;
}

static bool check_queue_exhaustion(void)
{
    rtos_usb_mq_t *queues[RTOS_CONFIG_USB_OSAL_MQ_COUNT];
    const bool is_full = hold_queue_slots(queues, true);
    record_check("queue_slots_full", is_full);
    if (!is_full) {
        release_queue_slots(queues);
        return false;
    }

    const uint32_t task_count = rtos_perf_task_registry_count();
    const rtos_status_t status = rtos_usb_cdc_init(MSC_TEST_BUS, 0U);
    record_check("queue_full_returns_no_memory", status == RTOS_ERR_NO_MEM);
    record_check("queue_full_controller_untouched",
                 s_controller_starts == 0U && s_controller_stops == 0U);
    record_check("queue_full_creates_no_worker", rtos_perf_task_registry_count() == task_count);
    bool has_all_sentinels = true;
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_MQ_COUNT; ++index) {
        uintptr_t value = 0U;
        const rtos_status_t receive = rtos_usb_mq_recv(queues[index], &value, RTOS_NO_WAIT);
        has_all_sentinels = (receive == RTOS_OK && value == queue_sentinel(index)) &&
                            has_all_sentinels;
    }
    record_check("queue_contents_unchanged", has_all_sentinels);
    release_queue_slots(queues);
    record_check("queue_failure_releases_sync", check_sync_slots_available());
    return status == RTOS_ERR_NO_MEM;
}

static bool check_thread_exhaustion(void)
{
    rtos_usb_thread_t *threads[RTOS_CONFIG_USB_OSAL_THREAD_COUNT];
    const uint32_t task_count = rtos_perf_task_registry_count();
    const bool is_full = hold_thread_slots(threads);
    record_check("thread_slots_full", is_full);
    if (!is_full) {
        release_thread_slots(threads);
        return false;
    }

    const rtos_status_t status = rtos_usb_cdc_init(MSC_TEST_BUS, 0U);
    record_check("thread_full_returns_no_memory", status == RTOS_ERR_NO_MEM);
    record_check("thread_full_controller_untouched",
                 s_controller_starts == 0U && s_controller_stops == 0U);
    record_check("thread_full_preserves_workers",
                 rtos_perf_task_registry_count() == task_count + RTOS_CONFIG_USB_OSAL_THREAD_COUNT);
    record_check("thread_failure_releases_queue", check_queue_slots_available());
    record_check("thread_failure_releases_sync", check_sync_slots_available());
    release_thread_slots(threads);
    return status == RTOS_ERR_NO_MEM;
}

static bool check_controller_failure(void)
{
    const uint32_t task_count = rtos_perf_task_registry_count();
    const uint32_t starts = s_controller_starts;
    const uint32_t stops = s_controller_stops;
    s_reject_controller = true;
    const rtos_status_t status = rtos_usb_cdc_init(MSC_TEST_BUS, 0U);
    s_reject_controller = false;
    record_check("controller_failure_status", status == RTOS_ERR_PARAM);
    record_check("controller_failure_calls",
                 s_controller_starts == starts + 1U && s_controller_stops == stops + 1U);
    record_check("controller_failure_removes_worker",
                 rtos_perf_task_registry_count() == task_count);
    record_check("controller_failure_releases_queue", check_queue_slots_available());
    record_check("controller_failure_releases_sync", check_sync_slots_available());

    rtos_usb_thread_t *threads[RTOS_CONFIG_USB_OSAL_THREAD_COUNT];
    record_check("controller_failure_releases_threads", hold_thread_slots(threads));
    release_thread_slots(threads);
    return status == RTOS_ERR_PARAM;
}

void f446_usb_msc_resource_prepare(void)
{
    const bool is_initialized = rtos_usb_init() == RTOS_OK;
    record_check("osal_initialized", is_initialized);
    if (!is_initialized || !check_queue_exhaustion() || !check_thread_exhaustion() ||
        !check_controller_failure()) {
        return;
    }

    const uint32_t starts = s_controller_starts;
    const uint32_t stops = s_controller_stops;
    s_startup_passed = rtos_usb_cdc_init(MSC_TEST_BUS, 0U) == RTOS_OK;
    record_check("retry_succeeds", s_startup_passed);
    record_check("retry_starts_controller_once",
                 s_controller_starts == starts + 1U && s_controller_stops == stops);
    if (s_startup_passed) {
        const rtos_status_t status = rtos_usb_cdc_init(MSC_TEST_BUS, 0U);
        record_check("duplicate_init_rejected",
                     status == RTOS_ERR_ALREADY && s_controller_starts == starts + 1U);
    }
}

static void check_runtime_worker(void)
{
    const rtos_tick_t before = rtos_sched_get_tick_count();
    rtos_task_delay(MSC_TEST_WORKER_TICKS);
    const rtos_tick_t elapsed = rtos_sched_get_tick_count() - before;
    uint32_t task_count = 0U;
    bool is_worker_waiting = false;
    if (rtos_perf_get_all_tasks(s_task_stats, RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE, &task_count) ==
        RTOS_OK) {
        for (uint32_t index = 0U; index < task_count; ++index) {
            const rtos_perf_task_t *const task = &s_task_stats[index];
            if (strcmp(task->name, "usbd_msc") == 0) {
                is_worker_waiting = task->state == RTOS_TASK_BLOCKED && task->switch_count > 0U;
                break;
            }
        }
    }
    record_check("worker_blocks_on_real_queue", is_worker_waiting);
    record_check("scheduler_ticks_advance", elapsed >= MSC_TEST_WORKER_TICKS);
}

void f446_usb_msc_resource_run(void *argument)
{
    (void)argument;
    if (s_startup_passed) {
        check_runtime_worker();
    }
    (void)printf("UMSC|DONE|pass=%lu|fail=%lu\r\n", (unsigned long)s_passes,
                 (unsigned long)s_failures);
    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}
