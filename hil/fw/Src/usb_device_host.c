/**
 * @file usb_device_host.c
 * @brief Exercise composite-device startup and the MSC RAM disk with a host controller stub.
 *
 * CherryUSB device core, class callbacks, and RTOS USB wrappers are compiled from production C.
 * The controller and IPC/task primitives below model ownership without executing an MCU thread.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtos_internal.h"
#include "rtos_mem.h"
#include "rtos_perf.h"
#include "rtos_sched.h"
#include "rtos_usb.h"
#include "usbd_core.h"
#include "usbd_msc.h"

enum {
    SECTOR_SIZE = 512U,
    MEDIA_TEST_BYTES = 2U * SECTOR_SIZE,
    CDC_IN = 0x81U,
    CDC_OUT = 0x02U,
    HID_IN = 0x85U,
    HID_OUT = 0x01U,
    MSC_OUT = 0x05U,
    FULL_SPEED_PACKET_BYTES = 64U,
    USB_ENDPOINT_COUNT = 16U
};

typedef struct host_out_endpoint {
    uint8_t *data;
    uint32_t capacity;
    uint32_t submissions;
} host_out_endpoint_t;

static uint32_t s_worker_attempts;
static uint32_t s_live_workers;
static uint32_t s_controller_attempts;
static uint32_t s_controller_deinits;
static uint32_t s_queue_released_before_worker;
static uint32_t s_failures;
static bool s_reject_controller;
static bool s_complete_writes;
static bool s_reset_before_mutex_acquire;
static uint32_t s_cdc_write_submissions;
static uint32_t s_cdc_zlp_submissions;
static uint32_t s_hid_write_submissions;
static host_out_endpoint_t s_out[USB_ENDPOINT_COUNT];
static rtos_tcb_t *s_msc_worker;
static rtos_queue_t *s_msc_queue;
static rtos_queue_t *s_last_queue;

static void configure_device(void)
{
    uint8_t request[] = { 0U, USB_REQUEST_SET_CONFIGURATION, 1U, 0U, 0U, 0U, 0U, 0U };
    usbd_event_ep0_setup_complete_handler(0U, request);
}

static void check(const char *name, bool is_ok)
{
    if (!is_ok) {
        ++s_failures;
    }
    (void)printf("USBHOST|CHECK|%s|%s\n", name, is_ok ? "PASS" : "FAIL");
}

rtos_status_t rtos_sem_init(rtos_sem_t *sem, uint32_t count, uint32_t max_count)
{
    sem->is_initialized = 1U;
    sem->count = count;
    sem->max_count = max_count;
    return RTOS_OK;
}

rtos_status_t rtos_sem_deinit(rtos_sem_t *sem)
{
    sem->is_initialized = 0U;
    return RTOS_OK;
}

rtos_status_t rtos_sem_give(rtos_sem_t *sem)
{
    if (sem->count == sem->max_count) {
        return RTOS_ERR_OVERFLOW;
    }
    ++sem->count;
    return RTOS_OK;
}

rtos_status_t rtos_sem_take(rtos_sem_t *sem, rtos_tick_t timeout)
{
    (void)timeout;
    if (sem->count == 0U) {
        return RTOS_ERR_TIMEOUT;
    }
    --sem->count;
    return RTOS_OK;
}

rtos_status_t rtos_internal_sem_take_guarded(rtos_sem_t *sem, rtos_tick_t timeout,
                                            const volatile uint32_t *generation,
                                            uint32_t expected_generation)
{
    if (generation != NULL && *generation != expected_generation) {
        return RTOS_ERR_DELETED;
    }
    return rtos_sem_take(sem, timeout);
}

void rtos_internal_sem_cancel_waiters(rtos_sem_t *sem)
{
    /* This host stub never registers blocking waiters. */
    sem->count = 0U;
    sem->wait_bitmap = 0U;
}

rtos_status_t rtos_mutex_init(rtos_mutex_t *mutex)
{
    mutex->is_initialized = 1U;
    return RTOS_OK;
}

rtos_status_t rtos_mutex_deinit(rtos_mutex_t *mutex)
{
    mutex->is_initialized = 0U;
    return RTOS_OK;
}

rtos_status_t rtos_mutex_take(rtos_mutex_t *mutex, rtos_tick_t timeout)
{
    (void)timeout;
    if (s_reset_before_mutex_acquire) {
        s_reset_before_mutex_acquire = false;
        usbd_event_reset_handler(0U);
        configure_device();
    }
    return mutex->is_initialized ? RTOS_OK : RTOS_ERR_DELETED;
}

rtos_status_t rtos_mutex_give(rtos_mutex_t *mutex)
{
    return mutex->is_initialized ? RTOS_OK : RTOS_ERR_DELETED;
}

rtos_status_t rtos_queue_init(rtos_queue_t *queue, void *storage, uint32_t item_size,
                              uint32_t capacity)
{
    queue->is_initialized = 1U;
    queue->storage = storage;
    queue->item_size = item_size;
    queue->capacity = capacity;
    s_last_queue = queue;
    return RTOS_OK;
}

rtos_status_t rtos_queue_deinit(rtos_queue_t *queue)
{
    if (queue == s_msc_queue && s_msc_worker != NULL) {
        ++s_queue_released_before_worker;
    }
    queue->is_initialized = 0U;
    return RTOS_OK;
}

rtos_status_t rtos_queue_send(rtos_queue_t *queue, const void *message, rtos_tick_t timeout,
                              rtos_bool_t is_front)
{
    (void)queue;
    (void)message;
    (void)timeout;
    (void)is_front;
    /* Startup tests do not issue transfers that require an MSC worker. */
    return RTOS_ERR_SCHED;
}

rtos_status_t rtos_queue_recv(rtos_queue_t *queue, void *message, rtos_tick_t timeout)
{
    (void)queue;
    (void)message;
    (void)timeout;
    return RTOS_ERR_SCHED;
}

rtos_status_t rtos_task_create_static(rtos_tcb_t *task, rtos_stack_t *stack, uint32_t stack_size,
                                     rtos_task_func_t entry, void *argument, rtos_prio_t priority,
                                     const char *name)
{
    (void)stack;
    (void)stack_size;
    (void)entry;
    (void)argument;
    (void)priority;
    ++s_live_workers;
    task->state = RTOS_TASK_READY;
    if (strcmp(name, "usbd_msc") == 0) {
        ++s_worker_attempts;
        s_msc_worker = task;
        s_msc_queue = s_last_queue;
    }
    return RTOS_OK;
}

rtos_status_t rtos_task_delete(rtos_tcb_t *task)
{
    if (task == s_msc_worker) {
        s_msc_worker = NULL;
    }
    --s_live_workers;
    task->state = RTOS_TASK_DELETED;
    return RTOS_OK;
}

rtos_tcb_t *rtos_task_get_current(void)
{
    return NULL;
}

/* Unused paths require symbols when the Windows linker keeps unused text. */
rtos_status_t rtos_task_set_priority(rtos_tcb_t *task, rtos_prio_t priority)
{
    (void)task;
    (void)priority;
    return RTOS_ERR_SCHED;
}

void rtos_task_delay(rtos_tick_t ticks)
{
    (void)ticks;
}

void rtos_task_yield(void)
{
}

rtos_tick_t rtos_sched_get_tick_count(void)
{
    return 0U;
}

rtos_status_t rtos_timer_create(rtos_timer_t *timer, const char *name, rtos_timer_cb_t callback,
                               void *argument, rtos_tick_t period, rtos_timer_mode_t mode)
{
    (void)timer;
    (void)name;
    (void)callback;
    (void)argument;
    (void)period;
    (void)mode;
    return RTOS_ERR_SCHED;
}

rtos_status_t rtos_timer_delete(rtos_timer_t *timer)
{
    (void)timer;
    return RTOS_ERR_SCHED;
}

rtos_status_t rtos_timer_start(rtos_timer_t *timer)
{
    (void)timer;
    return RTOS_ERR_SCHED;
}

rtos_status_t rtos_timer_stop(rtos_timer_t *timer)
{
    (void)timer;
    return RTOS_ERR_SCHED;
}

void rtos_perf_register_pool(rtos_mempool_t *pool)
{
    /* USB fixed pools are exercised without the performance registry. */
    (void)pool;
}

int usb_dc_init(uint8_t busid)
{
    (void)busid;
    ++s_controller_attempts;
    return s_reject_controller ? -1 : 0;
}

int usb_dc_deinit(uint8_t busid)
{
    (void)busid;
    ++s_controller_deinits;
    return 0;
}

int usbd_ep_start_read(uint8_t busid, const uint8_t endpoint, uint8_t *data, uint32_t length)
{
    (void)busid;
    host_out_endpoint_t *out = &s_out[endpoint & 0x0FU];
    out->data = data;
    out->capacity = length;
    ++out->submissions;
    return 0;
}

int usbd_ep_start_write(uint8_t busid, const uint8_t endpoint, const uint8_t *data, uint32_t length)
{
    (void)data;
    if (endpoint == CDC_IN) {
        ++s_cdc_write_submissions;
        if (length == 0U) {
            ++s_cdc_zlp_submissions;
        }
    } else if (endpoint == HID_IN) {
        ++s_hid_write_submissions;
    }
    if (s_complete_writes && (endpoint == CDC_IN || endpoint == HID_IN)) {
        usbd_event_ep_in_complete_handler(busid, endpoint, length);
    }
    return 0;
}

int usbd_ep_set_stall(uint8_t busid, const uint8_t endpoint)
{
    (void)busid;
    (void)endpoint;
    return 0;
}

int usbd_ep_open(uint8_t busid, const struct usb_endpoint_descriptor *endpoint)
{
    (void)busid;
    (void)endpoint;
    return 0;
}

int usbd_ep_close(uint8_t busid, const uint8_t endpoint)
{
    (void)busid;
    (void)endpoint;
    return 0;
}

int usbd_ep_clear_stall(uint8_t busid, const uint8_t endpoint)
{
    (void)busid;
    (void)endpoint;
    return 0;
}

int usbd_ep_is_stalled(uint8_t busid, const uint8_t endpoint, uint8_t *stalled)
{
    (void)busid;
    (void)endpoint;
    *stalled = 0U;
    return 0;
}

int usbd_set_address(uint8_t busid, const uint8_t address)
{
    (void)busid;
    (void)address;
    return 0;
}

int usbd_set_remote_wakeup(uint8_t busid)
{
    (void)busid;
    return 0;
}

uint8_t usbd_get_port_speed(uint8_t busid)
{
    (void)busid;
    return USB_SPEED_FULL;
}

void USBD_IRQHandler(uint8_t busid)
{
    (void)busid;
}

static void dormant_worker(void *argument)
{
    (void)argument;
}

static void verify_queue_slots(void)
{
    rtos_usb_mq_t *queues[RTOS_CONFIG_USB_OSAL_MQ_COUNT];
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_MQ_COUNT; ++index) {
        queues[index] = rtos_usb_mq_create(RTOS_CONFIG_USB_MQ_DEPTH);
        check("queue_slot_reusable", queues[index] != NULL);
    }
    check("queue_pool_full", rtos_usb_mq_create(1U) == NULL);
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_MQ_COUNT; ++index) {
        rtos_usb_mq_delete(queues[index]);
    }
}

static void verify_sync_slots(void)
{
    rtos_usb_sem_t *semaphores[RTOS_CONFIG_USB_OSAL_SEM_COUNT];
    rtos_usb_mutex_t *mutexes[RTOS_CONFIG_USB_OSAL_MUTEX_COUNT];
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_SEM_COUNT; ++index) {
        semaphores[index] = rtos_usb_sem_create(0U, 1U);
        check("semaphore_slot_reusable", semaphores[index] != NULL);
    }
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_MUTEX_COUNT; ++index) {
        mutexes[index] = rtos_usb_mutex_create();
        check("mutex_slot_reusable", mutexes[index] != NULL);
    }
    check("semaphore_pool_full", rtos_usb_sem_create(0U, 1U) == NULL);
    check("mutex_pool_full", rtos_usb_mutex_create() == NULL);
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_SEM_COUNT; ++index) {
        rtos_usb_sem_delete(semaphores[index]);
    }
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_MUTEX_COUNT; ++index) {
        rtos_usb_mutex_delete(mutexes[index]);
    }
}

static void verify_retry(void)
{
    const uint32_t previous_controller_attempts = s_controller_attempts;
    const uint32_t previous_worker_attempts = s_worker_attempts;
    rtos_status_t status = rtos_usb_cdc_init(0U, 0U);
    check("retry_succeeds", status == RTOS_OK);
    check("retry_creates_one_worker",
          s_live_workers == 1U && s_worker_attempts == previous_worker_attempts + 1U);
    check("retry_starts_controller", s_controller_attempts == previous_controller_attempts + 1U);
    if (status == RTOS_OK) {
        check("duplicate_init_rejected", rtos_usb_cdc_init(0U, 0U) == RTOS_ERR_ALREADY);
    }
}

static void test_queue_exhaustion(void)
{
    rtos_usb_mq_t *queues[RTOS_CONFIG_USB_OSAL_MQ_COUNT];
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_MQ_COUNT; ++index) {
        queues[index] = rtos_usb_mq_create(1U);
        check("held_queue", queues[index] != NULL);
    }
    check("queue_full_returns_no_memory", rtos_usb_cdc_init(0U, 0U) == RTOS_ERR_NO_MEM);
    check("queue_full_never_creates_worker", s_worker_attempts == 0U && s_live_workers == 0U);
    check("queue_full_leaves_controller_untouched", s_controller_attempts == 0U);
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_MQ_COUNT; ++index) {
        rtos_usb_mq_delete(queues[index]);
    }
    verify_sync_slots();
    verify_retry();
}

static void test_thread_exhaustion(void)
{
    rtos_usb_thread_t *threads[RTOS_CONFIG_USB_OSAL_THREAD_COUNT];
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_THREAD_COUNT; ++index) {
        threads[index] = rtos_usb_thread_create("hold", 0U, 20U, dormant_worker, NULL);
        check("held_thread", threads[index] != NULL);
    }
    check("thread_full_returns_no_memory", rtos_usb_cdc_init(0U, 0U) == RTOS_ERR_NO_MEM);
    check("thread_full_preserves_existing_workers",
          s_live_workers == RTOS_CONFIG_USB_OSAL_THREAD_COUNT);
    check("thread_full_leaves_controller_untouched", s_controller_attempts == 0U);
    verify_queue_slots();
    verify_sync_slots();
    for (uint32_t index = 0U; index < RTOS_CONFIG_USB_OSAL_THREAD_COUNT; ++index) {
        if (threads[index] != NULL) {
            rtos_usb_thread_delete(threads[index]);
        }
    }
    verify_retry();
}

static void test_controller_failure(void)
{
    s_reject_controller = true;
    check("controller_failure_propagates", rtos_usb_cdc_init(0U, 0U) == RTOS_ERR_PARAM);
    check("controller_failure_deinitializes", s_controller_attempts == 1U &&
          s_controller_deinits == 1U);
    check("worker_stops_before_queue_release", s_queue_released_before_worker == 0U);
    check("controller_failure_releases_worker", s_live_workers == 0U);
    verify_queue_slots();
    verify_sync_slots();
    s_reject_controller = false;
    verify_retry();
}

static void test_media(void)
{
    uint8_t input[MEDIA_TEST_BYTES];
    uint8_t output[MEDIA_TEST_BYTES];
    uint32_t blocks = 0U;
    uint32_t block_size = 0U;
    uint32_t reads = 0U;
    uint32_t writes = 0U;
    usbd_msc_get_cap(0U, 0U, &blocks, &block_size);
    check("media_geometry", block_size == SECTOR_SIZE &&
          blocks == RTOS_CONFIG_USB_MSC_RAM_DISK_SIZE / SECTOR_SIZE);
    if (blocks < 2U) {
        return;
    }
    for (uint32_t index = 0U; index < MEDIA_TEST_BYTES; ++index) {
        input[index] = (uint8_t)((index * 13U) ^ (index / SECTOR_SIZE));
    }
    memset(output, 0xA5, sizeof(output));
#if RTOS_CONFIG_USB_MSC_READONLY
    check("readonly_rejects_valid_write", usbd_msc_sector_write(0U, 0U, blocks - 2U,
          input, sizeof(input)) != 0);
    check("readonly_still_serves_reads", usbd_msc_sector_read(0U, 0U, blocks - 2U,
          output, sizeof(output)) == 0);
    memset(input, 0, sizeof(input));
    check("readonly_preserves_media", memcmp(input, output, sizeof(output)) == 0);
#else
    check("last_two_sectors_write", usbd_msc_sector_write(0U, 0U, blocks - 2U,
          input, sizeof(input)) == 0);
    check("last_two_sectors_round_trip", usbd_msc_sector_read(0U, 0U, blocks - 2U,
          output, sizeof(output)) == 0 && memcmp(input, output, sizeof(output)) == 0);
#endif
    check("reject_read_past_end_without_output", usbd_msc_sector_read(0U, 0U, blocks - 1U,
          output, sizeof(output)) != 0 && memcmp(input, output, sizeof(output)) == 0);
    check("reject_write_past_end", usbd_msc_sector_write(0U, 0U, blocks - 1U,
          input, sizeof(input)) != 0);
    check("reject_non_sector_length", usbd_msc_sector_read(0U, 0U, 0U, output, 1U) != 0);
    check("reject_null_sector_buffer", usbd_msc_sector_write(0U, 0U, 0U, NULL, SECTOR_SIZE) != 0);
    rtos_usb_msc_get_stats(0U, &reads, &writes);
    check("only_valid_sectors_counted", reads == 2U &&
          writes == (RTOS_CONFIG_USB_MSC_READONLY ? 0U : 2U));
}

static bool receive_packet(uint8_t endpoint, const uint8_t *data, uint32_t length)
{
    const host_out_endpoint_t *out = &s_out[endpoint];
    if (out->data == NULL || length > out->capacity) {
        return false;
    }
    memcpy(out->data, data, length);
    usbd_event_ep_out_complete_handler(0U, endpoint, length);
    return true;
}

static void test_transport(void)
{
    const uint8_t cdc_payload[FULL_SPEED_PACKET_BYTES] = { 7U, 8U, 9U };
    const uint8_t hid_payload[] = { 1U, 2U, 3U, 4U };
    const uint8_t discarded_payload[] = { 5U, 6U };
    uint8_t received[FULL_SPEED_PACKET_BYTES];

    const rtos_status_t status = rtos_usb_cdc_init(0U, 0U);
    check("transport_startup", status == RTOS_OK);
    if (status != RTOS_OK) {
        return;
    }
    check("no_config_no_connection", !rtos_usb_cdc_is_connected(0U));
    usbd_event_reset_handler(0U);
    configure_device();
    check("configured_by_real_device_core", rtos_usb_cdc_is_connected(0U));
    check("composite_arms_three_endpoints", s_out[CDC_OUT].submissions == 1U &&
          s_out[HID_OUT].submissions == 1U && s_out[MSC_OUT].submissions == 1U);

    check("cdc_out_accepts_and_rearms", receive_packet(CDC_OUT, cdc_payload, sizeof(cdc_payload)) &&
          s_out[CDC_OUT].submissions == 2U);
    check("cdc_reports_available_bytes", rtos_usb_cdc_available(0U) == sizeof(cdc_payload));
    check("cdc_returns_exact_bytes", rtos_usb_cdc_read(0U, received, sizeof(received), 0U) ==
          (int)sizeof(cdc_payload) && memcmp(received, cdc_payload, sizeof(cdc_payload)) == 0);
    check("cdc_rx_empty", rtos_usb_cdc_available(0U) == 0U);
    check("hid_rearms_even_when_report_dropped", receive_packet(HID_OUT, hid_payload,
          sizeof(hid_payload)) && receive_packet(HID_OUT, discarded_payload,
          sizeof(discarded_payload)) && s_out[HID_OUT].submissions == 3U);
    check("hid_delivers_first_report_prefix", rtos_usb_hid_read(0U, received, 2U, 0U) == 2 &&
          memcmp(received, hid_payload, 2U) == 0);
    check("hid_report_remainder_discarded",
          rtos_usb_hid_read(0U, received, sizeof(received), 0U) == 0);

    s_complete_writes = true;
    check("cdc_exact_packet_sends_zlp", rtos_usb_cdc_write(0U, cdc_payload,
          sizeof(cdc_payload), 0U) == (int)sizeof(cdc_payload) &&
          s_cdc_write_submissions == 2U && s_cdc_zlp_submissions == 1U);
    check("hid_write_completion", rtos_usb_hid_write(0U, hid_payload,
          sizeof(hid_payload), 0U) == (int)sizeof(hid_payload));
    check("cdc_pending_before_reset", receive_packet(CDC_OUT, cdc_payload, sizeof(cdc_payload)));
    usbd_event_reset_handler(0U);
    check("reset_clears_connection_and_rx", !rtos_usb_cdc_is_connected(0U) &&
          rtos_usb_cdc_available(0U) == 0U);
    check("disconnected_rejects_write", rtos_usb_cdc_write(0U, cdc_payload,
          sizeof(cdc_payload), 0U) == 0 && s_cdc_write_submissions == 2U);

    configure_device();
    uint32_t cdc_writes = s_cdc_write_submissions;
    s_reset_before_mutex_acquire = true;
    check("cdc_stale_writer_cannot_use_new_session", rtos_usb_cdc_write(0U, cdc_payload,
          1U, UINT32_MAX) == 0 && !s_reset_before_mutex_acquire &&
          s_cdc_write_submissions == cdc_writes);
    cdc_writes = s_cdc_write_submissions;
    check("cdc_new_writer_uses_new_session", rtos_usb_cdc_write(0U, cdc_payload,
          1U, 0U) == 1 && s_cdc_write_submissions == cdc_writes + 1U);

    uint32_t hid_writes = s_hid_write_submissions;
    s_reset_before_mutex_acquire = true;
    check("hid_stale_writer_cannot_use_new_session", rtos_usb_hid_write(0U, hid_payload,
          sizeof(hid_payload), UINT32_MAX) == 0 && !s_reset_before_mutex_acquire &&
          s_hid_write_submissions == hid_writes);
    hid_writes = s_hid_write_submissions;
    check("hid_new_writer_uses_new_session", rtos_usb_hid_write(0U, hid_payload,
          sizeof(hid_payload), 0U) == (int)sizeof(hid_payload) &&
          s_hid_write_submissions == hid_writes + 1U);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        return EXIT_FAILURE;
    }
    check("osal_init", rtos_usb_init() == RTOS_OK);
    check("osal_duplicate_init", rtos_usb_init() == RTOS_ERR_ALREADY);
    if (strcmp(argv[1], "queue-full") == 0) {
        test_queue_exhaustion();
    } else if (strcmp(argv[1], "thread-full") == 0) {
        test_thread_exhaustion();
    } else if (strcmp(argv[1], "controller-fail") == 0) {
        test_controller_failure();
    } else if (strcmp(argv[1], "media") == 0) {
        test_media();
    } else if (strcmp(argv[1], "transport") == 0) {
        test_transport();
    } else {
        return EXIT_FAILURE;
    }
    (void)printf("USBHOST|DONE|case=%s|fail=%lu\n", argv[1], (unsigned long)s_failures);
    return s_failures == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
