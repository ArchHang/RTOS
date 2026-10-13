/* Hardware regression checks for USB OSAL and Cortex-M interrupt masking.
 * Run from a task after rtos_init()/rtos_usb_init(). No USB cable is required. */
#include "audit_usb_port.h"
#include "rtos.h"
#include "main.h"
#include "usb_config.h"
#include "usb_osal.h"
#include "usbd_core.h"
#include "usb_scsi.h"
#include "kern_port.h"
#include <stdio.h>

static unsigned s_pass;
static unsigned s_fail;
static volatile uint32_t s_hook_armed;
static volatile uint32_t s_irq_seen;
static volatile uint32_t s_irq_inside_hook;
static volatile uint32_t s_first_task;
static rtos_sem_t s_irq_sem;
static rtos_tcb_t s_high_tcb;
static rtos_tcb_t s_low_tcb;
static rtos_stack_t s_high_stack[256] __attribute__((aligned(8)));
static rtos_stack_t s_low_stack[256] __attribute__((aligned(8)));
static unsigned s_transport_reset;
static volatile unsigned s_budget_hid;
static volatile unsigned s_budget_read;
static volatile uint32_t s_release_delay;
static volatile uint32_t s_usb_thread_exits;

#if defined(RTOS_HIL_AUDIT_PENDSV)
extern int rtos_usb_audit_begin(void);
extern void rtos_usb_audit_event(uint8_t event);
extern void rtos_usb_audit_hid_rx(const uint8_t *data, uint32_t length);
extern void rtos_usb_audit_end(void);
extern bool usbd_msc_audit_decode(uint8_t lun, uint8_t command, uint32_t sector,
                                  uint32_t blocks);
extern bool usbd_dwc2_audit_rx_bounds(uint32_t expected_length, uint32_t packet_length);
extern uint32_t usbd_dwc2_audit_interface_select(uint8_t alt_setting);
extern bool usbd_core_audit_interface_request(uint8_t request, uint16_t index);
extern bool usbd_core_audit_configuration_bounds(void);

/* Audit builds wrap the endpoint transport, but execute the production API,
 * its mutex/sem waits, and the production event/completion callbacks. */
int __wrap_usbd_ep_start_write(uint8_t busid, uint8_t ep, const uint8_t *data,
                               uint32_t length)
{
    (void)busid;
    (void)ep;
    (void)data;
    (void)length;
    if (s_transport_reset != 0U) {
        s_transport_reset = 0U;
        rtos_usb_audit_event(USBD_EVENT_RESET);
    }
    return 0;
}

int __wrap_usbd_ep_start_read(uint8_t busid, uint8_t ep, uint8_t *data, uint32_t length)
{
    (void)busid;
    (void)ep;
    (void)data;
    (void)length;
    return 0;
}
#endif

static void report(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        s_pass++;
        printf("AT|USBPORT|%s|PASS\r\n", name);
    } else {
        s_fail++;
        printf("AT|USBPORT|%s|FAIL|actual=%lu|expected=%lu\r\n", name,
               (unsigned long)actual, (unsigned long)expected);
    }
}

static void mask_checks(void)
{
    uint32_t original_basepri = __get_BASEPRI();
    uint32_t original_primask = __get_PRIMASK();
    uint32_t entry, exit, inner, outer;

    /* Prevent UART output while a deliberately strong mask is installed. */
    __set_BASEPRI(0x20U);
    RTOS_PORT_ENTER_CRITICAL();
    entry = __get_BASEPRI();
    RTOS_PORT_EXIT_CRITICAL();
    exit = __get_BASEPRI();
    __set_BASEPRI(original_basepri);
#if RTOS_CONFIG_USE_BASEPRI
    report("basepri_preserve_stronger_enter", entry, 0x20U);
    report("basepri_preserve_stronger_exit", exit, 0x20U);
#else
    report("basepri_preserve_stronger_enter", entry, 0x20U);
    report("basepri_preserve_stronger_exit", exit, 0x20U);
#endif

    __set_BASEPRI(0x80U);
    RTOS_PORT_ENTER_CRITICAL();
    entry = __get_BASEPRI();
    RTOS_PORT_EXIT_CRITICAL();
    exit = __get_BASEPRI();
    __set_BASEPRI(original_basepri);
#if RTOS_CONFIG_USE_BASEPRI
    report("basepri_strengthen_weaker_enter", entry,
           RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY);
#else
    report("basepri_strengthen_weaker_enter", entry, 0x80U);
#endif
    report("basepri_restore_weaker_exit", exit, 0x80U);

    __disable_irq();
    RTOS_PORT_ENTER_CRITICAL();
    entry = __get_PRIMASK();
    RTOS_PORT_EXIT_CRITICAL();
    exit = __get_PRIMASK();
    __set_PRIMASK(original_primask);
    report("primask_preserve_disabled_enter", entry, 1U);
    report("primask_preserve_disabled_exit", exit, 1U);

    RTOS_PORT_ENTER_CRITICAL();
    RTOS_PORT_ENTER_CRITICAL();
    RTOS_PORT_EXIT_CRITICAL();
#if RTOS_CONFIG_USE_BASEPRI
    inner = __get_BASEPRI();
#else
    inner = __get_PRIMASK();
#endif
    RTOS_PORT_EXIT_CRITICAL();
#if RTOS_CONFIG_USE_BASEPRI
    outer = __get_BASEPRI();
    report("nested_inner_remains_masked", inner,
           RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY);
    report("nested_outer_restores", outer, original_basepri);
#else
    outer = __get_PRIMASK();
    report("nested_inner_remains_masked", inner, 1U);
    report("nested_outer_restores", outer, original_primask);
#endif
    report("critical_nesting_balanced", rtos_port_get_critical_nesting(), 0U);
}

static void no_op_timer(void *arg)
{
    (void)arg;
}

static void dormant_usb_thread(void *arg)
{
    (void)arg;
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static void osal_checks(void)
{
    const uint32_t ms_values[] = { 1U, 4000000U, 4294968U };
    const char *names[] = { "timer_ms_one", "timer_ms_before_overflow", "timer_ms_overflow" };
    for (unsigned i = 0U; i < sizeof(ms_values) / sizeof(ms_values[0]); ++i) {
        uint32_t expected = (uint32_t)(((uint64_t)ms_values[i] *
                                      RTOS_CONFIG_TICK_RATE_HZ + 999U) / 1000U);
        struct usb_osal_timer *timer = usb_osal_timer_create("audit_ms", ms_values[i],
                                                            no_op_timer, NULL, false);
        uint32_t actual = timer ? ((rtos_usb_timer_t *)timer->timer)->timer.period : 0U;
        report(names[i], actual, expected);
        if (timer != NULL) {
            usb_osal_timer_delete(timer);
        }
    }
#if RTOS_CONFIG_USB_USE_MSC
    report("msc_stack_byte_configuration", CONFIG_USBDEV_MSC_STACKSIZE,
           RTOS_CONFIG_USB_THREAD_STACK_SIZE * sizeof(rtos_stack_t));
    usb_osal_thread_t thread = usb_osal_thread_create("audit_msc", CONFIG_USBDEV_MSC_STACKSIZE,
                                                     RTOS_CONFIG_MAX_PRIORITIES - 1U,
                                                     dormant_usb_thread, NULL);
    uint32_t actual = thread ? ((rtos_usb_thread_t *)thread)->tcb.stack_size : 0U;
    report("msc_actual_stack_words", actual, RTOS_CONFIG_USB_THREAD_STACK_SIZE);
    if (thread != NULL) {
        usb_osal_thread_delete(thread);
    }
#endif
}

static void injected_irq(void)
{
    s_irq_seen = 1U;
    rtos_sem_give(&s_irq_sem);
}

static void high_waiter(void *arg)
{
    (void)arg;
    rtos_sem_take(&s_irq_sem, RTOS_WAIT_FOREVER);
    if (s_first_task == 0U) {
        s_first_task = 1U;
    }
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static void low_candidate(void *arg)
{
    (void)arg;
    if (s_first_task == 0U) {
        s_first_task = 2U;
    }
    for (;;) {
        rtos_task_delay(1000U);
    }
}

/* Inject a syscall-domain IRQ precisely after PendSV snapshots next_tcb.
 * Before the fix it preempts and its newer choice is overwritten by NULL.
 * With the fix the IRQ runs after PSP restoration and chooses the high task. */
void audit_usb_port_pendsv_hook(void)
{
    if (s_hook_armed != 0U) {
        s_hook_armed = 0U;
        TIM6->EGR = TIM_EGR_UG;
        __DSB();
        __ISB();
        for (volatile unsigned i = 0U; i < 50U; ++i) {
            __NOP();
        }
        s_irq_inside_hook = s_irq_seen;
    }
}

static void pendsv_irq_checks(void)
{
#if defined(RTOS_HIL_AUDIT_PENDSV)
    s_hook_armed = s_irq_seen = s_irq_inside_hook = s_first_task = 0U;
    rtos_sem_init(&s_irq_sem, 0U, 1U);
    rtos_status_t high = rtos_task_create_static(&s_high_tcb, s_high_stack, 256U,
                                                high_waiter, NULL, 4U, "audit_irq_hi");
    rtos_status_t low = rtos_task_create_static(&s_low_tcb, s_low_stack, 256U,
                                               low_candidate, NULL, 12U, "audit_irq_lo");
    if ((high != RTOS_OK) || (low != RTOS_OK)) {
        report("pendsv_irq_setup", 0U, 1U);
    } else {
        kern_isr_set_handler(injected_irq);
        kern_timer_isr_start(60000U, 6U);
        TIM6->CR1 = 0U;
        TIM6->SR = 0U;
        HAL_NVIC_ClearPendingIRQ(TIM6_DAC_IRQn);
        s_hook_armed = 1U;
        rtos_task_delay(3U);
        kern_timer_isr_stop();
        kern_isr_set_handler(NULL);
        report("pendsv_legal_irq_executed", s_irq_seen, 1U);
        report("pendsv_legal_irq_deferred", s_irq_inside_hook, 0U);
        report("pendsv_highest_ready_runs_first", s_first_task, 1U);
    }
    if (high == RTOS_OK) {
        rtos_task_delete(&s_high_tcb);
    }
    if (low == RTOS_OK) {
        rtos_task_delete(&s_low_tcb);
    }
    rtos_sem_deinit(&s_irq_sem);
#endif
}

static void simulated_usb_checks(void)
{
#if defined(RTOS_HIL_AUDIT_PENDSV)
    uint8_t report_data[64] = { 0xA5U };
    uint8_t read_data[64];
    int setup = rtos_usb_audit_begin();
    report("simulated_usb_setup", setup == 0, 1U);
    if (setup == 0) {
        s_transport_reset = 1U;
        report("cdc_reset_does_not_report_sent",
               rtos_usb_cdc_write(0U, report_data, 1U, 5U), 0U);
#if RTOS_CONFIG_USB_USE_HID
        rtos_usb_audit_event(USBD_EVENT_CONFIGURED);
        s_transport_reset = 1U;
        report("hid_reset_does_not_report_sent",
               rtos_usb_hid_write(0U, report_data, sizeof(report_data), 5U), 0U);
        rtos_usb_audit_event(USBD_EVENT_CONFIGURED);
        rtos_usb_audit_hid_rx(report_data, sizeof(report_data));
        rtos_usb_audit_event(USBD_EVENT_RESET);
        report("hid_reset_discards_stale_rx",
               rtos_usb_hid_read(0U, read_data, sizeof(read_data), 0U), 0U);
#endif
    }
    rtos_usb_audit_end();
#if RTOS_CONFIG_USB_USE_MSC
    report("msc_valid_lun_read_capacity",
           usbd_msc_audit_decode(0U, SCSI_CMD_READCAPACITY10, 0U, 0U), 1U);
    report("msc_invalid_lun_rejected",
           usbd_msc_audit_decode(1U, SCSI_CMD_READCAPACITY10, 0U, 0U), 0U);
    report("msc_sector_add_overflow_rejected",
           usbd_msc_audit_decode(0U, SCSI_CMD_READ10, 0xFFFFFFFFU, 2U), 0U);
#endif
    report("dwc2_valid_rx_preserves_guard", usbd_dwc2_audit_rx_bounds(31U, 31U), 1U);
    report("dwc2_oversize_rx_preserves_guard", usbd_dwc2_audit_rx_bounds(31U, 64U), 1U);
    report("core_get_valid_interface", usbd_core_audit_interface_request(USB_REQUEST_GET_INTERFACE, 0U), 1U);
    report("core_get_invalid_interface_rejected", usbd_core_audit_interface_request(USB_REQUEST_GET_INTERFACE, 16U), 0U);
    report("core_set_invalid_interface_rejected", usbd_core_audit_interface_request(USB_REQUEST_SET_INTERFACE, 16U), 0U);
    report("core_high_index_interface_rejected", usbd_core_audit_interface_request(USB_REQUEST_GET_INTERFACE, 0x100U), 0U);
    report("core_select_alt_zero_keeps_endpoint", usbd_dwc2_audit_interface_select(0U), 3U);
    report("core_invalid_alt_rejected_keeps_endpoint", usbd_dwc2_audit_interface_select(1U), 2U);
    report("core_config_scan_stays_in_total_length", usbd_core_audit_configuration_bounds(), 0U);
#endif
}

#if defined(RTOS_HIL_AUDIT_PENDSV)
static void blocking_writer(void *arg)
{
    (void)arg;
    uint8_t byte = 0x55U;
    if (s_budget_hid != 0U) {
        rtos_usb_hid_write(0U, &byte, 1U, RTOS_WAIT_FOREVER);
    } else {
        rtos_usb_cdc_write(0U, &byte, 1U, RTOS_WAIT_FOREVER);
    }
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static void budget_event_source(void *arg)
{
    (void)arg;
    if (s_budget_read != 0U) {
        /* A host repeatedly resets/reconfigures while this reader waits. */
        for (unsigned i = 0U; i < 5U; ++i) {
            rtos_task_delay(2U);
            rtos_usb_audit_event(USBD_EVENT_RESET);
            rtos_usb_audit_event(USBD_EVENT_CONFIGURED);
        }
    } else {
        rtos_task_delay(s_release_delay);
        rtos_usb_audit_event(USBD_EVENT_RESET);
    }
    for (;;) {
        rtos_task_delay(1000U);
    }
}
#endif

static void usb_budget_checks(void)
{
#if defined(RTOS_HIL_AUDIT_PENDSV)
    static rtos_tcb_t event_tcb;
    static rtos_stack_t event_stack[256] __attribute__((aligned(8)));
    const char *lock_names[2][2] = {
        { "cdc_mutex_no_wait", "cdc_mutex_finite_budget" },
        { "hid_mutex_no_wait", "hid_mutex_finite_budget" }
    };
    uint8_t byte = 0U;
    for (unsigned hid = 0U; hid < 2U; ++hid) {
        for (unsigned finite = 0U; finite < 2U; ++finite) {
            s_budget_hid = hid;
            s_budget_read = 0U;
            s_release_delay = 15U;
            rtos_usb_audit_begin();
            rtos_status_t evt = rtos_task_create_static(&event_tcb, event_stack, 256U,
                                                        budget_event_source, NULL, 5U, "usb_release");
            rtos_status_t owner = rtos_task_create_static(&s_high_tcb, s_high_stack, 256U,
                                                          blocking_writer, NULL, 4U, "usb_owner");
            if ((evt == RTOS_OK) && (owner == RTOS_OK)) {
                uint32_t before = rtos_sched_get_tick_count();
                if (hid != 0U) {
                    rtos_usb_hid_write(0U, &byte, 1U, finite ? 5U : 0U);
                } else {
                    rtos_usb_cdc_write(0U, &byte, 1U, finite ? 5U : 0U);
                }
                uint32_t elapsed = rtos_sched_get_tick_count() - before;
                report(lock_names[hid][finite], elapsed <= (finite ? 6U : 0U), 1U);
                rtos_task_delay(16U);
            } else {
                report(lock_names[hid][finite], 0U, 1U);
            }
            if (evt == RTOS_OK) rtos_task_delete(&event_tcb);
            if (owner == RTOS_OK) rtos_task_delete(&s_high_tcb);
            rtos_usb_audit_end();
        }
        s_budget_hid = hid;
        s_budget_read = 1U;
        rtos_usb_audit_begin();
        rtos_status_t evt = rtos_task_create_static(&event_tcb, event_stack, 256U,
                                                    budget_event_source, NULL, 4U, "usb_resets");
        uint32_t before = rtos_sched_get_tick_count();
        if (hid != 0U) {
            rtos_usb_hid_read(0U, &byte, 1U, 5U);
        } else {
            rtos_usb_cdc_read(0U, &byte, 1U, 5U);
        }
        uint32_t elapsed = rtos_sched_get_tick_count() - before;
        report(hid ? "hid_read_repeated_event_budget" : "cdc_read_repeated_event_budget",
               (evt == RTOS_OK) && (elapsed <= 6U), 1U);
        if (evt == RTOS_OK) rtos_task_delete(&event_tcb);
        rtos_usb_audit_end();
    }
#endif
}

static void usb_self_delete(void *arg)
{
    s_usb_thread_exits++;
    if (arg != NULL) {
        usb_osal_thread_delete(NULL);
    }
    /* NULL arg returns naturally through rtos_task_exit_handler(). */
}

static void usb_thread_lifecycle_checks(void)
{
    for (unsigned explicit_delete = 0U; explicit_delete < 2U; ++explicit_delete) {
        usb_osal_thread_t handles[RTOS_CONFIG_USB_OSAL_THREAD_COUNT + 1U] = { 0 };
        s_usb_thread_exits = 0U;
        unsigned created = 0U;
        for (unsigned i = 0U; i < RTOS_CONFIG_USB_OSAL_THREAD_COUNT + 1U; ++i) {
            handles[i] = usb_osal_thread_create("usb_self_exit", 1024U, 4U,
                                                usb_self_delete,
                                                explicit_delete ? (void *)1U : NULL);
            if (handles[i] != NULL) {
                created++;
            }
        }
        report(explicit_delete ? "osal_self_delete_pool_reused" : "osal_return_pool_reused",
               created, RTOS_CONFIG_USB_OSAL_THREAD_COUNT + 1U);
        report(explicit_delete ? "osal_self_delete_completed" : "osal_return_completed",
               s_usb_thread_exits, RTOS_CONFIG_USB_OSAL_THREAD_COUNT + 1U);
        /* Reclaim leaked baseline slots so the following fixture can run. */
        for (unsigned i = 0U; i < RTOS_CONFIG_USB_OSAL_THREAD_COUNT + 1U; ++i) {
            if (handles[i] != NULL) {
                usb_osal_thread_delete(handles[i]);
            }
        }
    }
}

static struct usb_osal_timer *s_delete_timer;
static usb_osal_thread_t s_delete_thread;
static volatile uint32_t s_delete_irq_count;
static rtos_timer_t s_delete_filler;
static volatile uint32_t s_fill_successes;
static volatile rtos_status_t s_fill_status;

static void osal_delete_in_irq(void)
{
    s_delete_irq_count++;
    /* TIMER cannot run inside this IRQ. The first command may be delivered
     * directly to its blocked queue receiver, so fill until NO_WAIT reports
     * the queue actually full instead of assuming 16 sends imply fullness. */
    for (unsigned i = 0U; i < 32U; ++i) {
        s_fill_status = rtos_timer_stop(&s_delete_filler);
        if (s_fill_status != RTOS_OK) break;
        s_fill_successes++;
    }
    if (s_delete_timer != NULL) {
        usb_osal_timer_delete(s_delete_timer);
    }
    if (s_delete_thread != NULL) {
        usb_osal_thread_delete(s_delete_thread);
    }
    kern_timer_isr_stop();
}

static void osal_delete_thread(void *arg)
{
    (void)arg;
    for (;;) rtos_task_delay(1000U);
}

static void usb_failed_delete_checks(void)
{
    rtos_task_delay(5U); /* Let previous timer-delete commands drain. */
    s_delete_timer = usb_osal_timer_create("usb_full_delete", 1000U, no_op_timer, NULL, false);
    s_delete_thread = usb_osal_thread_create("usb_irq_delete", 1024U, 12U,
                                              osal_delete_thread, NULL);
    if ((s_delete_timer == NULL) || (s_delete_thread == NULL) ||
        (rtos_timer_create(&s_delete_filler, "usb_cmd_fill", no_op_timer, NULL, 1000U,
                           RTOS_TIMER_ONE_SHOT) != RTOS_OK)) {
        report("osal_failed_delete_fixture", 0U, 1U);
        if (s_delete_timer != NULL) usb_osal_timer_delete(s_delete_timer);
        if (s_delete_thread != NULL) usb_osal_thread_delete(s_delete_thread);
        return;
    }
    rtos_usb_timer_t *inner = (rtos_usb_timer_t *)s_delete_timer->timer;
    rtos_usb_thread_t *thread = (rtos_usb_thread_t *)s_delete_thread;
    s_delete_irq_count = 0U;
    s_fill_successes = 0U;
    s_fill_status = RTOS_OK;
    kern_isr_set_handler(osal_delete_in_irq);
    RTOS_PORT_ENTER_CRITICAL();
    (void)kern_timer_isr_start(500U, 6U);
    TIM6->EGR = TIM_EGR_UG;
    RTOS_PORT_EXIT_CRITICAL();
    while (s_delete_irq_count == 0U) __asm volatile("nop");
    kern_timer_isr_stop();
    kern_isr_set_handler(NULL);
    report("osal_delete_irq_executed", s_delete_irq_count, 1U);
    printf("AT|USBPORT|DETAIL|timer_queue_fill_successes=%u|status=%d\r\n",
           (unsigned)s_fill_successes, (int)s_fill_status);
    report("osal_timer_queue_full_confirmed", s_fill_status == RTOS_ERR_TIMEOUT, 1U);
    report("osal_timer_queue_fill_count_valid", s_fill_successes >= 16U && s_fill_successes <= 17U, 1U);
    report("osal_timer_failed_delete_retains_slot", inner->in_use, 1U);
    {
        /* The CherryUSB outer timer handle is a live 64-byte-pool allocation
         * until the command is accepted; checking capacity avoids reading
         * through a possibly freed outer handle in the old implementation. */
        void *blocks[RTOS_CONFIG_USB_HEAP_SIZE / 128U] = { 0 };
        unsigned count = 0U;
        while (count < RTOS_CONFIG_USB_HEAP_SIZE / 128U) {
            blocks[count] = usb_osal_malloc(64U);
            if (blocks[count] == NULL) break;
            count++;
        }
        report("osal_timer_failed_delete_retains_outer", count,
               RTOS_CONFIG_USB_HEAP_SIZE / 128U - 1U);
        for (unsigned i = 0U; i < count; ++i) usb_osal_free(blocks[i]);
    }
    report("osal_thread_isr_delete_retains_slot", thread->in_use, 1U);
    report("osal_thread_isr_delete_keeps_task", thread->tcb.state != RTOS_TASK_DELETED, 1U);
    usb_osal_thread_delete(s_delete_thread);
    /* Original OSAL already freed the outer handle on the failed delete. */
    if (inner->in_use != 0U) {
        usb_osal_timer_delete(s_delete_timer);
    } else {
        (void)rtos_timer_delete(&inner->timer);
    }
    (void)rtos_timer_delete(&s_delete_filler);
    s_delete_timer = NULL;
    s_delete_thread = NULL;
    rtos_task_delay(5U);
}

static void usb_active_timer_reuse_checks(void)
{
    struct usb_osal_timer *old = usb_osal_timer_create("usb_active_old", 1000U,
                                                       no_op_timer, NULL, false);
    if (old == NULL) {
        report("osal_active_timer_fixture", 0U, 1U);
        return;
    }
    rtos_usb_timer_t *inner = (rtos_usb_timer_t *)old->timer;
    usb_osal_timer_start(old);
    rtos_task_delay(2U); /* TIMER processes START and leaves the timer active. */
    report("osal_active_timer_fixture", inner->timer.is_active, 1U);
    usb_osal_timer_delete(old);
    /* This runner has higher priority than TIMER: accepted DELETE is still
     * queued. Keep only the inner pool pointer, as the outer handle is freed. */
    report("osal_active_delete_accepted", inner->in_use, 0U);
    report("osal_active_delete_pending", inner->timer.is_active, 1U);
    struct usb_osal_timer *replacement = usb_osal_timer_create("usb_active_new", 1000U,
                                                               no_op_timer, NULL, false);
    report("osal_create_after_active_delete", replacement != NULL, 1U);
    rtos_task_delay(3U); /* Finish the old DELETE before reclaiming new timer. */
    if (replacement != NULL) usb_osal_timer_delete(replacement);
    rtos_task_delay(3U);
}

void audit_usb_port_run(void)
{
    s_pass = s_fail = 0U;
    rtos_usb_init();
    mask_checks();
    osal_checks();
    pendsv_irq_checks();
    simulated_usb_checks();
    usb_budget_checks();
    usb_thread_lifecycle_checks();
    usb_failed_delete_checks();
    usb_active_timer_reuse_checks();
    printf("AT|USBPORT|SUMMARY|pass=%u|fail=%u\r\n", s_pass, s_fail);
}
