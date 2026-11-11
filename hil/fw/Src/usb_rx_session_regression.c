/** @file usb_rx_session_regression.c
 * @brief Exercise USB receive session changes with RTOS readers and a real NVIC IRQ.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hil_target.h"
#include "rtos.h"
#include "usbd_core.h"

#if !RTOS_CONFIG_USE_USB || !RTOS_CONFIG_USB_USE_HID || !defined(RTOS_HIL_OBJECT_LIFETIME)
#error "The USB RX regression requires CDC/HID and the wait-registration hook"
#endif

enum {
    RX_TEST_BUS = 0U,
    RX_TEST_HID_OUT = 1U,
    RX_TEST_CDC_OUT = 2U,
    RX_TEST_READERS = 4U,
    RX_TEST_STACK_WORDS = 192U,
    RX_TEST_WORKER_PRIORITY = 8U,
    RX_TEST_WAIT_TICKS = 16U,
    RX_TEST_CDC_DATA = 0xA6U,
    RX_TEST_HID_DATA = 0x6BU
};

typedef enum rx_test_irq {
    RX_IRQ_NONE,
    RX_IRQ_CONFIGURE,
    RX_IRQ_RESET,
    RX_IRQ_RECONFIGURE,
    RX_IRQ_RECONFIGURE_WITH_DATA,
    RX_IRQ_DISCONNECT
} rx_test_irq_t;

typedef struct rx_test_reader {
    rtos_tcb_t task;
    bool is_hid;
    int32_t result;
    uint8_t data;
} rx_test_reader_t;

static usbd_event_handler_t s_usb_event;
static uint8_t *s_cdc_out_buffer;
static uint8_t *s_hid_out_buffer;
static _Atomic(rx_test_irq_t) s_irq_action;
static _Atomic uint32_t s_irq_count;
static atomic_bool s_inject_on_take;
static rx_test_reader_t s_readers[RX_TEST_READERS];
static _Alignas(8) rtos_stack_t s_stacks[RX_TEST_READERS][RX_TEST_STACK_WORDS];
static rtos_sem_t s_completion;
static bool s_usb_initialized;
static uint32_t s_failures;

int __wrap_usbd_initialize(uint8_t busid, uintptr_t reg_base, usbd_event_handler_t event_handler);
int __wrap_usbd_ep_start_read(uint8_t busid, uint8_t ep, uint8_t *data, uint32_t data_len);
void rtos_hil_object_access_hook(const void *object, uint32_t site);
void EXTI2_IRQHandler(void);
void usb_rx_session_prepare(void);
void usb_rx_session_run(void *argument);

int __wrap_usbd_initialize(uint8_t busid, uintptr_t reg_base, usbd_event_handler_t event_handler)
{
    (void)reg_base;
    if (busid != RX_TEST_BUS || event_handler == NULL) {
        return -1;
    }
    s_usb_event = event_handler;
    return 0;
}

int __wrap_usbd_ep_start_read(uint8_t busid, uint8_t ep, uint8_t *data, uint32_t data_len)
{
    if (busid != RX_TEST_BUS || data == NULL || data_len == 0U) {
        return -1;
    }
    if (ep == RX_TEST_CDC_OUT) {
        s_cdc_out_buffer = data;
    } else if (ep == RX_TEST_HID_OUT) {
        s_hid_out_buffer = data;
    }
    return 0;
}

static void request_irq(rx_test_irq_t action)
{
    atomic_store_explicit(&s_irq_action, action, memory_order_relaxed);
    NVIC_SetPendingIRQ(EXTI2_IRQn);
    __DSB();
    __ISB();
}

void EXTI2_IRQHandler(void)
{
    const rx_test_irq_t action = atomic_load_explicit(&s_irq_action, memory_order_relaxed);
    if (s_usb_event == NULL || action == RX_IRQ_NONE) {
        return;
    }
    if (action == RX_IRQ_DISCONNECT) {
        s_usb_event(RX_TEST_BUS, USBD_EVENT_DISCONNECTED);
    } else if (action != RX_IRQ_CONFIGURE) {
        s_usb_event(RX_TEST_BUS, USBD_EVENT_RESET);
    }
    if (action != RX_IRQ_RESET && action != RX_IRQ_DISCONNECT) {
        s_usb_event(RX_TEST_BUS, USBD_EVENT_CONFIGURED);
    }
    if (action == RX_IRQ_RECONFIGURE_WITH_DATA &&
        s_cdc_out_buffer != NULL && s_hid_out_buffer != NULL) {
        *s_cdc_out_buffer = RX_TEST_CDC_DATA;
        *s_hid_out_buffer = RX_TEST_HID_DATA;
        usbd_event_ep_out_complete_handler(RX_TEST_BUS, RX_TEST_CDC_OUT, 1U);
        usbd_event_ep_out_complete_handler(RX_TEST_BUS, RX_TEST_HID_OUT, 1U);
    }
    atomic_fetch_add_explicit(&s_irq_count, 1U, memory_order_relaxed);
}

void rtos_hil_object_access_hook(const void *object, uint32_t site)
{
    (void)object;
    /* Site 2 is sem_take, after the USB connection check and before wait registration. */
    if (site == 2U && rtos_task_get_current() == &s_readers[0].task &&
        atomic_exchange_explicit(&s_inject_on_take, false, memory_order_relaxed)) {
        request_irq(RX_IRQ_RECONFIGURE);
    }
}

static void read_as_worker(void *argument)
{
    rx_test_reader_t *reader = argument;
    if (reader->is_hid) {
        reader->result = rtos_usb_hid_read(RX_TEST_BUS, &reader->data, 1U, UINT32_MAX);
    } else {
        reader->result = rtos_usb_cdc_read(RX_TEST_BUS, &reader->data, 1U, UINT32_MAX);
    }
    if (rtos_sem_give(&s_completion) != RTOS_OK) {
        rtos_assert_fail("USB RX fixture completion signal", __FILE__, __LINE__);
    }
    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}

static void check_phase(rx_test_irq_t action)
{
    const bool is_wait_registration_race = action == RX_IRQ_RECONFIGURE;
    const uint32_t readers = is_wait_registration_race ? 1U : RX_TEST_READERS;
    const uint32_t initial_irqs = atomic_load_explicit(&s_irq_count, memory_order_relaxed);
    request_irq(RX_IRQ_CONFIGURE);
    atomic_store_explicit(&s_inject_on_take, is_wait_registration_race, memory_order_relaxed);

    uint32_t created = 0U;
    for (uint32_t index = 0U; index < readers; ++index) {
        rx_test_reader_t *reader = &s_readers[index];
        reader->is_hid = index >= RX_TEST_READERS / 2U;
        reader->result = -1;
        reader->data = 0U;
        if (rtos_task_create_static(&reader->task, s_stacks[index], RX_TEST_STACK_WORDS,
                                    read_as_worker, reader, RX_TEST_WORKER_PRIORITY,
                                    "usb-reader") != RTOS_OK) {
            break;
        }
        ++created;
    }
    rtos_task_delay(2U);

    uint32_t blocked = 0U;
    if (!is_wait_registration_race) {
        RTOS_PORT_ENTER_CRITICAL();
        for (uint32_t index = 0U; index < created; ++index) {
            const rtos_tcb_t *task = &s_readers[index].task;
            blocked += (uint32_t)(task->state == RTOS_TASK_BLOCKED && task->blocked_on != NULL);
        }
        RTOS_PORT_EXIT_CRITICAL();
        request_irq(action);
    }

    uint32_t completed = 0U;
    for (uint32_t index = 0U; index < created; ++index) {
        if (rtos_sem_take(&s_completion, RX_TEST_WAIT_TICKS) != RTOS_OK) {
            break;
        }
        ++completed;
    }
    uint32_t cancelled = 0U;
    uint32_t stolen = 0U;
    bool cleanup_ok = true;
    for (uint32_t index = 0U; index < created; ++index) {
        cancelled += (uint32_t)(s_readers[index].result == 0);
        stolen += (uint32_t)(s_readers[index].result > 0);
        cleanup_ok = (rtos_task_delete(&s_readers[index].task) == RTOS_OK) && cleanup_ok;
    }
    uint32_t fresh = 0U;
    if (action == RX_IRQ_RECONFIGURE_WITH_DATA) {
        uint8_t byte = 0U;
        fresh += (uint32_t)(rtos_usb_cdc_read(RX_TEST_BUS, &byte, 1U, 0U) == 1 &&
                            byte == RX_TEST_CDC_DATA);
        fresh += (uint32_t)(rtos_usb_hid_read(RX_TEST_BUS, &byte, 1U, 0U) == 1 &&
                            byte == RX_TEST_HID_DATA);
    }
    const uint32_t irqs = atomic_load_explicit(&s_irq_count, memory_order_relaxed) - initial_irqs;
    const uint32_t expected_fresh = (action == RX_IRQ_RECONFIGURE_WITH_DATA) ? 2U : 0U;
    const bool passed = created == readers && irqs == 2U && cleanup_ok &&
        (is_wait_registration_race || blocked == readers) && completed == readers &&
        cancelled == readers && stolen == 0U && fresh == expected_fresh;
    s_failures += (uint32_t)!passed;
    (void)printf("URX|CASE|action=%lu|created=%lu|blocked=%lu|done=%lu|cancelled=%lu|"
                 "stolen=%lu|fresh=%lu|irq=%lu|pass=%u\r\n", (unsigned long)action,
                 (unsigned long)created, (unsigned long)blocked, (unsigned long)completed,
                 (unsigned long)cancelled, (unsigned long)stolen, (unsigned long)fresh,
                 (unsigned long)irqs, (unsigned int)passed);
}

void usb_rx_session_prepare(void)
{
    s_usb_initialized = rtos_usb_init() == RTOS_OK && rtos_usb_cdc_init(RX_TEST_BUS, 0U) == RTOS_OK;
}

void usb_rx_session_run(void *argument)
{
    (void)argument;
    if (!s_usb_initialized || rtos_sem_init(&s_completion, 0U, RX_TEST_READERS) != RTOS_OK ||
        NVIC_GetEnableIRQ(EXTI2_IRQn) != 0U || NVIC_GetPendingIRQ(EXTI2_IRQn) != 0U) {
        (void)printf("URX|DONE|status=1|failures=1|setup=0\r\n");
        return;
    }

    const uint32_t saved_priority = NVIC_GetPriority(EXTI2_IRQn);
    const uint32_t priority = RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY >> (8U - __NVIC_PRIO_BITS);
    NVIC_SetPriority(EXTI2_IRQn, priority);
    NVIC_EnableIRQ(EXTI2_IRQn);
    check_phase(RX_IRQ_RESET);
    check_phase(RX_IRQ_RECONFIGURE_WITH_DATA);
    check_phase(RX_IRQ_DISCONNECT);
    check_phase(RX_IRQ_RECONFIGURE);
    NVIC_DisableIRQ(EXTI2_IRQn);
    NVIC_ClearPendingIRQ(EXTI2_IRQn);
    NVIC_SetPriority(EXTI2_IRQn, saved_priority);
    s_failures += (uint32_t)(rtos_sem_deinit(&s_completion) != RTOS_OK);
    (void)printf("URX|DONE|status=%u|failures=%lu|setup=1\r\n",
                 (unsigned int)(s_failures != 0U), (unsigned long)s_failures);
    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}
