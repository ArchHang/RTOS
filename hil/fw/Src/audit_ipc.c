/**
 * IPC audit regressions. Helpers really block on the target objects. The
 * duplicate-init checks restore the saved object before releasing the outer
 * critical section, so a failing baseline cannot leave orphan wait nodes.
 */
#include "audit_ipc.h"
#include "rtos.h"
#include "rtos_internal.h"
#include <stdio.h>
#include <string.h>

extern uint32_t rtos_internal_mutex_registry_count(void);

enum { IPC_SEM, IPC_QUEUE_RECV, IPC_QUEUE_SEND, IPC_EVENT, IPC_MUTEX };
enum { IPC_HELPER_PRIO = 6U, IPC_STACK_WORDS = 256U };

typedef struct {
    uint32_t operation;
    uint32_t id;
    rtos_tick_t timeout;
    volatile uint32_t done;
    volatile uint32_t result;
    uint32_t item;
} ipc_helper_arg_t;

static rtos_tcb_t s_helper[2];
static rtos_stack_t s_stack[2][IPC_STACK_WORDS] __attribute__((aligned(8)));
static ipc_helper_arg_t s_arg[2];
static rtos_bool_t s_created[2];
static rtos_sem_t s_sem;
static rtos_queue_t s_queue;
static uint32_t s_storage[2];
static rtos_event_t s_event;
static rtos_mutex_t s_mutex;
static volatile uint32_t s_order[2];
static volatile uint32_t s_order_count;
static uint32_t s_pass;
static uint32_t s_fail;

static void ipc_report(const char *name, rtos_bool_t ok, uint32_t actual, uint32_t expected)
{
    if (ok) {
        s_pass++;
    } else {
        s_fail++;
    }
    printf("AT|IPC|%s|%s|actual=%lu expect=%lu\r\n", name, ok ? "PASS" : "FAIL",
           (unsigned long)actual, (unsigned long)expected);
}

static void ipc_helper(void *opaque)
{
    ipc_helper_arg_t *arg = (ipc_helper_arg_t *)opaque;
    rtos_status_t status = RTOS_ERR_PARAM;
    switch (arg->operation) {
    case IPC_SEM:
        status = rtos_sem_take(&s_sem, arg->timeout);
        break;
    case IPC_QUEUE_RECV:
        status = rtos_queue_recv(&s_queue, &arg->item, arg->timeout);
        break;
    case IPC_QUEUE_SEND:
        status = rtos_queue_send(&s_queue, &arg->item, arg->timeout, RTOS_FALSE);
        break;
    case IPC_EVENT:
        arg->result = rtos_event_wait(&s_event, 1U, RTOS_EVENT_WAIT_ANY |
                                     RTOS_EVENT_CLEAR_ON_EXIT, arg->timeout);
        break;
    case IPC_MUTEX:
        status = rtos_mutex_take(&s_mutex, arg->timeout);
        if (status == RTOS_OK) {
            (void)rtos_mutex_give(&s_mutex);
        }
        break;
    default:
        break;
    }
    if (arg->operation != IPC_EVENT) {
        arg->result = (uint32_t)status;
    }
    RTOS_PORT_ENTER_CRITICAL();
    if (s_order_count < 2U) {
        s_order[s_order_count++] = arg->id;
    }
    arg->done = 1U;
    RTOS_PORT_EXIT_CRITICAL();
    /* Keep the helper alive until the controller explicitly deletes it. */
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static rtos_bool_t ipc_create(uint32_t slot, uint32_t operation, rtos_tick_t timeout)
{
    memset(&s_arg[slot], 0, sizeof(s_arg[slot]));
    s_arg[slot].operation = operation;
    s_arg[slot].id = slot + 1U;
    s_arg[slot].timeout = timeout;
    s_arg[slot].item = 0xABCD0000U + slot;
    rtos_status_t status = rtos_task_create_static(&s_helper[slot], s_stack[slot],
                                                 IPC_STACK_WORDS, ipc_helper,
                                                 &s_arg[slot], IPC_HELPER_PRIO, "ipc-audit");
    s_created[slot] = (status == RTOS_OK);
    return s_created[slot];
}

static rtos_bool_t ipc_wait_blocked(uint32_t slot, void *object)
{
    for (uint32_t tick = 0U; tick < 30U; tick++) {
        if (s_created[slot] && s_helper[slot].blocked_on == object &&
            s_helper[slot].wait_node.list_head != NULL) {
            return RTOS_TRUE;
        }
        rtos_task_delay(1U);
    }
    return RTOS_FALSE;
}

static rtos_bool_t ipc_wait_done(uint32_t slot)
{
    for (uint32_t tick = 0U; tick < 30U; tick++) {
        if (s_arg[slot].done != 0U) {
            return RTOS_TRUE;
        }
        rtos_task_delay(1U);
    }
    return RTOS_FALSE;
}

static void ipc_delete_helpers(void)
{
    for (uint32_t slot = 0U; slot < 2U; slot++) {
        if (s_created[slot]) {
            (void)rtos_task_delete(&s_helper[slot]);
            s_created[slot] = RTOS_FALSE;
        }
    }
    s_order_count = 0U;
    s_order[0] = 0U;
    s_order[1] = 0U;
}

/* Remove every duplicate baseline registration of this static test mutex. */
static void ipc_mutex_cleanup(uint32_t previous_count)
{
    for (uint32_t i = 0U; i < RTOS_CONFIG_MAX_MUTEXES; i++) {
        if (rtos_internal_mutex_registry_count() <= previous_count) {
            break;
        }
        (void)rtos_mutex_deinit(&s_mutex);
    }
}

static void ipc_test_event_timeout_lifetime(void)
{
    rtos_tcb_t *self = rtos_task_get_current();
    rtos_prio_t old_base = self->base_priority;
    (void)rtos_event_init(&s_event);
    (void)rtos_event_set(&s_event, 2U); /* bit 0 remains unsatisfied */
    /* Controller is above the waiter after its timeout. It may legally deinit
     * the event after the waiter has left both lists but before it resumes. */
    (void)rtos_task_set_priority(self, 4U);
    rtos_bool_t created = ipc_create(0U, IPC_EVENT, 4U);
    rtos_bool_t blocked = ipc_wait_blocked(0U, &s_event);
    rtos_tick_t started = rtos_sched_get_tick_count();
    if (created && blocked) {
        while ((rtos_tick_t)(rtos_sched_get_tick_count() - started) < 6U) {
            /* SysTick runs; the lower-priority waiter cannot resume yet. */
            __asm volatile("nop");
        }
    }
    rtos_bool_t timed_out = created && blocked &&
        s_helper[0].blocked_on == NULL && s_arg[0].done == 0U &&
        s_helper[0].wait_result == RTOS_ERR_TIMEOUT;
    (void)rtos_event_deinit(&s_event);
    /* Static storage is safe to overwrite; this simulates the lifetime ending
     * without a genuine freed-memory access or undefined host test behavior. */
    s_event.bits = 0x00654321U;
    rtos_bool_t completed = ipc_wait_done(0U);
    ipc_report("event_timeout_object_reused", timed_out && completed && s_arg[0].result == 2U,
               s_arg[0].result, 2U);
    ipc_delete_helpers();
    (void)rtos_task_set_priority(self, old_base);
}

static void ipc_test_active_reinit(uint32_t operation, const char *name)
{
    uint32_t mutex_count = rtos_internal_mutex_registry_count();
    rtos_status_t initialized = RTOS_ERR_PARAM;
    void *object = NULL;
    switch (operation) {
    case IPC_SEM:
        initialized = rtos_sem_init(&s_sem, 0U, 1U);
        object = &s_sem;
        break;
    case IPC_QUEUE_RECV:
    case IPC_QUEUE_SEND:
        initialized = rtos_queue_init(&s_queue, s_storage, sizeof(uint32_t), 2U);
        object = &s_queue;
        if (operation == IPC_QUEUE_SEND && initialized == RTOS_OK) {
            uint32_t value = 1U;
            (void)rtos_queue_send(&s_queue, &value, RTOS_NO_WAIT, RTOS_FALSE);
            (void)rtos_queue_send(&s_queue, &value, RTOS_NO_WAIT, RTOS_FALSE);
        }
        break;
    case IPC_EVENT:
        initialized = rtos_event_init(&s_event);
        object = &s_event;
        break;
    case IPC_MUTEX:
        initialized = rtos_mutex_init(&s_mutex);
        object = &s_mutex;
        if (initialized == RTOS_OK) {
            initialized = rtos_mutex_take(&s_mutex, RTOS_NO_WAIT);
        }
        break;
    default:
        break;
    }
    rtos_bool_t created = initialized == RTOS_OK && ipc_create(0U, operation, RTOS_WAIT_FOREVER);
    rtos_bool_t blocked = created && ipc_wait_blocked(0U, object);
    rtos_status_t duplicate = RTOS_ERR_PARAM;
    rtos_bool_t preserved = RTOS_FALSE;
    if (blocked) {
        RTOS_PORT_ENTER_CRITICAL();
        switch (operation) {
        case IPC_SEM: {
            rtos_sem_t before = s_sem;
            duplicate = rtos_sem_init(&s_sem, 0U, 1U);
            preserved = s_sem.wait_head == before.wait_head &&
                        s_sem.wait_bitmap == before.wait_bitmap;
            s_sem = before;
            break;
        }
        case IPC_QUEUE_RECV:
        case IPC_QUEUE_SEND: {
            rtos_queue_t before = s_queue;
            duplicate = rtos_queue_init(&s_queue, s_storage, sizeof(uint32_t), 2U);
            preserved = s_queue.recv_wait_head == before.recv_wait_head &&
                        s_queue.send_wait_head == before.send_wait_head &&
                        s_queue.count == before.count;
            s_queue = before;
            break;
        }
        case IPC_EVENT: {
            rtos_event_t before = s_event;
            duplicate = rtos_event_init(&s_event);
            preserved = s_event.wait_head == before.wait_head;
            s_event = before;
            break;
        }
        case IPC_MUTEX: {
            rtos_mutex_t before = s_mutex;
            duplicate = rtos_mutex_init(&s_mutex);
            preserved = s_mutex.wait_head == before.wait_head &&
                        s_mutex.owner == before.owner &&
                        s_mutex.recursion_count == before.recursion_count;
            s_mutex = before;
            break;
        }
        default:
            break;
        }
        RTOS_PORT_EXIT_CRITICAL();
    }
    ipc_report(name, blocked && preserved && duplicate == RTOS_ERR_ALREADY,
               (uint32_t)duplicate, (uint32_t)RTOS_ERR_ALREADY);
    ipc_delete_helpers();
    switch (operation) {
    case IPC_SEM: (void)rtos_sem_deinit(&s_sem); break;
    case IPC_QUEUE_RECV:
    case IPC_QUEUE_SEND: (void)rtos_queue_deinit(&s_queue); break;
    case IPC_EVENT: (void)rtos_event_deinit(&s_event); break;
    case IPC_MUTEX: ipc_mutex_cleanup(mutex_count); break;
    default: break;
    }
}

static void ipc_test_mutex_registration(void)
{
    uint32_t initial = rtos_internal_mutex_registry_count();
    rtos_status_t first = rtos_mutex_init(&s_mutex);
    rtos_status_t duplicate = rtos_mutex_init(&s_mutex);
    uint32_t count = rtos_internal_mutex_registry_count();
    ipc_report("mutex_duplicate_registration", first == RTOS_OK &&
                 duplicate == RTOS_OK && count == initial + 1U, count, initial + 1U);
    ipc_mutex_cleanup(initial);
    ipc_report("mutex_registration_cleanup", rtos_internal_mutex_registry_count() == initial,
               rtos_internal_mutex_registry_count(), initial);
}

static void ipc_test_fifo(uint32_t operation, const char *name)
{
    uint32_t mutex_count = rtos_internal_mutex_registry_count();
    rtos_status_t initialized = RTOS_ERR_PARAM;
    void *object = NULL;
    switch (operation) {
    case IPC_SEM: initialized = rtos_sem_init(&s_sem, 0U, 1U); object = &s_sem; break;
    case IPC_QUEUE_RECV:
        initialized = rtos_queue_init(&s_queue, s_storage, sizeof(uint32_t), 2U);
        object = &s_queue;
        break;
    case IPC_EVENT: initialized = rtos_event_init(&s_event); object = &s_event; break;
    case IPC_MUTEX:
        initialized = rtos_mutex_init(&s_mutex); object = &s_mutex;
        if (initialized == RTOS_OK) initialized = rtos_mutex_take(&s_mutex, RTOS_NO_WAIT);
        break;
    default: break;
    }
    rtos_bool_t a = initialized == RTOS_OK && ipc_create(0U, operation, RTOS_WAIT_FOREVER);
    rtos_bool_t a_wait = a && ipc_wait_blocked(0U, object);
    rtos_bool_t b = a_wait && ipc_create(1U, operation, RTOS_WAIT_FOREVER);
    rtos_bool_t b_wait = b && ipc_wait_blocked(1U, object);
    if (a_wait && b_wait) {
        (void)rtos_task_set_priority(&s_helper[0], IPC_HELPER_PRIO); /* effective no-op */
        switch (operation) {
        case IPC_SEM: (void)rtos_sem_give(&s_sem); break;
        case IPC_QUEUE_RECV: {
            uint32_t value = 0x12345678U;
            (void)rtos_queue_send(&s_queue, &value, RTOS_NO_WAIT, RTOS_FALSE);
            break;
        }
        case IPC_EVENT: (void)rtos_event_set(&s_event, 1U); break;
        case IPC_MUTEX: (void)rtos_mutex_give(&s_mutex); break;
        default: break;
        }
        for (uint32_t tick = 0U; tick < 30U && s_order_count == 0U; tick++) {
            rtos_task_delay(1U);
        }
    }
    ipc_report(name, a_wait && b_wait && s_order_count != 0U && s_order[0] == 1U,
               s_order[0], 1U);
    ipc_delete_helpers();
    switch (operation) {
    case IPC_SEM: (void)rtos_sem_deinit(&s_sem); break;
    case IPC_QUEUE_RECV: (void)rtos_queue_deinit(&s_queue); break;
    case IPC_EVENT: (void)rtos_event_deinit(&s_event); break;
    case IPC_MUTEX: ipc_mutex_cleanup(mutex_count); break;
    default: break;
    }
}

static void ipc_test_queue_size_overflow(void)
{
    uint32_t input = 0x78563412U;
    uint32_t output = 0U;
    rtos_status_t initialized = rtos_queue_init(&s_queue, s_storage, sizeof(uint32_t), 2U);
    (void)rtos_queue_send(&s_queue, &input, RTOS_NO_WAIT, RTOS_FALSE);
    rtos_status_t invalid = rtos_queue_init(&s_queue, s_storage, 2U, UINT32_MAX / 2U + 1U);
    /* Never use the impossible geometry on the failing baseline. Attempt a
     * further memcpy only when the original geometry was preserved. */
    rtos_bool_t preserved = s_queue.item_size == sizeof(uint32_t) &&
                            s_queue.capacity == 2U && s_queue.count == 1U;
    if (preserved) {
        (void)rtos_queue_recv(&s_queue, &output, RTOS_NO_WAIT);
    }
    ipc_report("queue_storage_size_overflow", initialized == RTOS_OK &&
               invalid == RTOS_ERR_PARAM && preserved && output == input,
               (uint32_t)invalid, (uint32_t)RTOS_ERR_PARAM);
    (void)rtos_queue_deinit(&s_queue);
}

static void ipc_test_queue_transfer_size(uint32_t operation, const char *name)
{
    uint32_t value = 0x11223344U;
    uint32_t output = 0U;
    rtos_status_t initialized = rtos_queue_init(&s_queue, s_storage, sizeof(uint32_t), 2U);
    if (operation == IPC_QUEUE_SEND && initialized == RTOS_OK) {
        (void)rtos_queue_send(&s_queue, &value, RTOS_NO_WAIT, RTOS_FALSE);
        (void)rtos_queue_send(&s_queue, &value, RTOS_NO_WAIT, RTOS_FALSE);
    }
    rtos_bool_t created = initialized == RTOS_OK && ipc_create(0U, operation, RTOS_WAIT_FOREVER);
    rtos_bool_t blocked = created && ipc_wait_blocked(0U, &s_queue);
    rtos_status_t transfer = RTOS_ERR_PARAM;
    if (blocked) {
        RTOS_PORT_ENTER_CRITICAL();
        /* Deliberately damage only metadata; the actual helper buffer is still
         * four bytes, so the old implementation's copy remains memory-safe. */
        s_helper[0].wait_node.transfer_size = 1U;
        if (operation == IPC_QUEUE_RECV) {
            transfer = rtos_queue_send(&s_queue, &value, RTOS_NO_WAIT, RTOS_FALSE);
        } else {
            transfer = rtos_queue_recv(&s_queue, &output, RTOS_NO_WAIT);
        }
        RTOS_PORT_EXIT_CRITICAL();
    }
    rtos_bool_t completed = blocked && ipc_wait_done(0U);
    rtos_bool_t storage_ok = RTOS_FALSE;
    if (completed && s_queue.count == 1U) {
        if (operation == IPC_QUEUE_RECV) {
            storage_ok = rtos_queue_recv(&s_queue, &output, RTOS_NO_WAIT) == RTOS_OK &&
                         output == value;
        } else {
            storage_ok = output == value;
        }
    }
    ipc_report(name, completed && transfer == RTOS_OK && storage_ok &&
               s_arg[0].result == (uint32_t)RTOS_ERR_PARAM,
               s_arg[0].result, (uint32_t)RTOS_ERR_PARAM);
    ipc_delete_helpers();
    (void)rtos_queue_deinit(&s_queue);
}

void audit_ipc_run(void)
{
    s_pass = 0U;
    s_fail = 0U;
    s_order_count = 0U;
    ipc_test_event_timeout_lifetime();
    ipc_test_active_reinit(IPC_SEM, "sem_active_reinit");
    ipc_test_active_reinit(IPC_QUEUE_RECV, "queue_receiver_active_reinit");
    ipc_test_active_reinit(IPC_QUEUE_SEND, "queue_sender_active_reinit");
    ipc_test_active_reinit(IPC_EVENT, "event_active_reinit");
    ipc_test_active_reinit(IPC_MUTEX, "mutex_active_reinit");
    ipc_test_mutex_registration();
    ipc_test_fifo(IPC_SEM, "sem_fifo_same_priority");
    ipc_test_fifo(IPC_QUEUE_RECV, "queue_fifo_same_priority");
    ipc_test_fifo(IPC_EVENT, "event_fifo_same_priority");
    ipc_test_fifo(IPC_MUTEX, "mutex_fifo_same_priority");
    ipc_test_queue_size_overflow();
    ipc_test_queue_transfer_size(IPC_QUEUE_RECV, "queue_receiver_size_mismatch");
    ipc_test_queue_transfer_size(IPC_QUEUE_SEND, "queue_sender_size_mismatch");
    printf("AT|IPC|SUMMARY|pass=%lu fail=%lu\r\n", (unsigned long)s_pass,
           (unsigned long)s_fail);
}
