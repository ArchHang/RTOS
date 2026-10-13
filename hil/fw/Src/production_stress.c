/**
 * @file production_stress.c
 * @brief Reproducible mixed IPC acquisition, processing, alarm and control test.
 *
 * Sixteen static workers use priorities 5 through 20. Two real timer ISRs and
 * four synthetic acquisition tasks send to the same bounded queue; four tasks
 * consume it. Validation recomputes the expected payload and uses a bounded
 * sequence window, allowing consumers to finish out of dequeue order. Event
 * bits are level flags, not counted messages; different CLEAR consumers own
 * disjoint masks because this kernel clears each waiter immediately in priority
 * order. Shared statistics and list inspection are protected by critical sections.
 */
#include "production_stress.h"

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "production_irq.h"
#include "rtos.h"
#include "rtos_heap.h"

#ifndef PRODUCTION_STRESS_SEED
#define PRODUCTION_STRESS_SEED 1U
#endif
#ifndef PRODUCTION_STRESS_SECONDS
#define PRODUCTION_STRESS_SECONDS 30U
#endif
#ifndef PRODUCTION_STRESS_QUEUE_CAPACITY
#define PRODUCTION_STRESS_QUEUE_CAPACITY 8U
#endif
#ifndef PRODUCTION_STRESS_INJECT_CORRUPTION
#define PRODUCTION_STRESS_INJECT_CORRUPTION 0U
#endif

#define STRESS_WORKERS 16U
#define STRESS_SOURCES 6U
#define STRESS_STACK_WORDS 256U
#define STRESS_WINDOW_BITS 1024U
#define STRESS_WINDOW_WORDS (STRESS_WINDOW_BITS / 32U)
#define STRESS_PHASE_BOOT 0U
#define STRESS_PHASE_NORMAL 1U
#define STRESS_PHASE_HIGH 2U
#define STRESS_PHASE_RECOVER 3U
#define STRESS_ANY_MASK 0x33U
#define STRESS_ALL_MASK 0xC0U
#define STRESS_TIMEOUT_MASK 0x100U
#define STRESS_CONTROL_PERIOD 10U
#define STRESS_CONTROL_DEADLINE 10U
#define STRESS_SHUTDOWN_TICKS 2000U
#define STRESS_STACK_GUARD_WORDS 32U
#define STRESS_LOG_PRIORITY 21U
#define STRESS_INIT_QUEUE 1U
#define STRESS_INIT_IRQ_SEM 2U
#define STRESS_INIT_LEASE_SEM 4U
#define STRESS_INIT_MUTEX 8U
#define STRESS_INIT_EVENT 16U

_Static_assert(PRODUCTION_STRESS_SECONDS >= 6U, "Stress run must span three phases");
_Static_assert(PRODUCTION_STRESS_SECONDS <= 900U, "Bounded stress duration exceeded");
_Static_assert(PRODUCTION_STRESS_QUEUE_CAPACITY >= 1U, "Queue must have storage");
_Static_assert(PRODUCTION_STRESS_QUEUE_CAPACITY <= 64U, "Queue storage budget exceeded");
_Static_assert(RTOS_CONFIG_TICK_RATE_HZ == 1000U, "Stress deadlines use millisecond ticks");
_Static_assert(RTOS_CONFIG_MAX_PRIORITIES > 20U, "Sixteen distinct priorities required");
_Static_assert(RTOS_CONFIG_PERF_STACK_WATERMARK == 1, "Historical stack fill is required");

typedef struct stress_worker {
    uint32_t id;
    uint32_t random_state;
} stress_worker_t;

typedef struct stress_ledger {
    uint32_t next_sequence;
    uint32_t validated;
    uint32_t pending[STRESS_WINDOW_WORDS];
} stress_ledger_t;

typedef struct stress_state {
    uint32_t phase;
    uint32_t stop_producers;
    uint32_t stop_workers;
    uint32_t parked_mask;
    uint32_t heartbeats[STRESS_WORKERS];
    uint32_t completed[STRESS_WORKERS];
    uint32_t sent[4];
    uint32_t send_timeout[4];
    uint32_t received;
    uint32_t receive_timeout;
    uint32_t sem_taken[2];
    uint32_t sem_timeout[2];
    uint32_t sem_peak;
    uint32_t lease_peak;
    uint32_t event_success[2];
    uint32_t event_timeout[3];
    uint32_t queue_receive_peak;
    uint32_t queue_send_peak;
    uint32_t event_peak;
    uint32_t mutex_peak;
    uint32_t queue_full[4];
    uint32_t queue_empty[4];
    uint32_t inheritance_seen;
    uint32_t control_jitter_max;
    uint32_t control_response_max;
    uint32_t control_deadline_misses;
    uint32_t watchdog_misses;
    uint32_t errors;
    uint32_t checksum_errors;
    uint32_t injected;
    const char *first_error;
    int32_t first_actual;
    int32_t first_expected;
} stress_state_t;

typedef struct stress_baseline {
    uint32_t heap_free;
    uint32_t heap_max;
    uint32_t allocations;
    uint32_t frees;
} stress_baseline_t;

typedef struct stress_golden {
    uint32_t source;
    uint32_t sequence;
    uint32_t seed;
    uint32_t payload;
    uint32_t checksum;
} stress_golden_t;

/* Generated independently on the host with explicit 32-bit multiplication. */
static const stress_golden_t golden_vectors[4] = {
    {0U, 0U, 1U, 0x688990C0U, 0xCD4A8FE7U},
    {3U, 12345U, 0x12345678U, 0xD2F73410U, 0xAD927625U},
    {5U, 0xFFFFFFFFU, 0xA5A5A5A5U, 0x160C0C9EU, 0x5B258CDBU},
    {4U, 100000U, 0xDEADBEEFU, 0xDC8009A8U, 0x019F76CBU}
};

static rtos_tcb_t worker_tcbs[STRESS_WORKERS];
static rtos_stack_t worker_stacks[STRESS_WORKERS][STRESS_STACK_WORDS]
    __attribute__((aligned(8)));
static stress_worker_t worker_args[STRESS_WORKERS];
static production_packet_t queue_storage[PRODUCTION_STRESS_QUEUE_CAPACITY];
static rtos_queue_t data_queue;
static rtos_sem_t irq_sem;
static rtos_sem_t lease_sem;
static rtos_mutex_t resource_mutex;
static rtos_event_t shared_event;
static stress_state_t shared_state;
static stress_ledger_t ledgers[STRESS_SOURCES];
static uint32_t resource_version; /* Accessed only while resource_mutex is owned. */
static uint32_t initialized;
static uint32_t created;
static uint32_t check_pass;
static uint32_t check_fail;
static uint32_t injection_reported; /* Controller-only log state. */
static uint32_t watchdog_heartbeat[STRESS_WORKERS];
static uint32_t watchdog_work[STRESS_WORKERS];
static uint32_t watchdog_heartbeat_tick[STRESS_WORKERS];
static uint32_t watchdog_work_tick[STRESS_WORKERS];
static uint32_t watchdog_last_check;

uint32_t production_stress_payload(uint32_t source, uint32_t sequence, uint32_t seed)
{
    uint32_t value = seed ^ (source * 0x9E3779B9U) ^ sequence;
    value ^= value >> 16U;
    value *= 0x7FEB352DU;
    value ^= value >> 15U;
    value *= 0x846CA68BU;
    return value ^ (value >> 16U);
}

uint32_t production_stress_checksum(const production_packet_t *packet)
{
    if (packet == NULL) {
        return 0U;
    }
    return (packet->source * 0x9E3779B9U) ^ packet->sequence
           ^ packet->payload ^ 0xA5C31F27U;
}

static void stress_record_failure(const char *name, int32_t actual, int32_t expected)
{
    RTOS_PORT_ENTER_CRITICAL();
    if (shared_state.errors == 0U) {
        shared_state.first_error = name;
        shared_state.first_actual = actual;
        shared_state.first_expected = expected;
    }
    shared_state.errors++;
    RTOS_PORT_EXIT_CRITICAL();
}

/* Synchronous UART output must not hold a higher priority than the control loop.
 * Only the controller logs; workers publish errors through shared_state instead. */
static void stress_log(const char *format, ...)
{
    rtos_tcb_t *controller = rtos_task_get_current();
    rtos_prio_t priority = rtos_task_get_priority(controller);
    rtos_status_t status = rtos_task_set_priority(controller, STRESS_LOG_PRIORITY);
    if (status != RTOS_OK) {
        stress_record_failure("LOG_PRIORITY_LOWER", (int32_t)status, (int32_t)RTOS_OK);
    }
    va_list arguments;
    va_start(arguments, format);
    int result = vprintf(format, arguments);
    va_end(arguments);
    if (result < 0) {
        stress_record_failure("UART_LOG_WRITE", (int32_t)result, 0);
    }
    status = rtos_task_set_priority(controller, priority);
    if (status != RTOS_OK) {
        stress_record_failure("LOG_PRIORITY_RESTORE", (int32_t)status, (int32_t)RTOS_OK);
    }
}

static void stress_check(uint32_t is_correct, const char *name)
{
    if (is_correct != 0U) {
        check_pass++;
        stress_log("STRESS|CHECK|name=%s status=PASS\r\n", name);
    } else {
        check_fail++;
        stress_log("STRESS|FAIL|check=%s\r\n", name);
    }
}

static void stress_verify_golden(void)
{
    for (uint32_t index = 0U; index < 4U; index++) {
        const stress_golden_t *golden = &golden_vectors[index];
        uint32_t payload = production_stress_payload(golden->source, golden->sequence,
                                                     golden->seed);
        production_packet_t packet = {golden->source, golden->sequence, golden->payload, 0U};
        stress_check(payload == golden->payload, "payload_matches_host_golden");
        stress_check(production_stress_checksum(&packet) == golden->checksum,
                     "checksum_matches_host_golden");
    }
}

static uint32_t stress_phase(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    uint32_t phase = shared_state.phase;
    RTOS_PORT_EXIT_CRITICAL();
    return phase;
}

static uint32_t stress_stopping(uint32_t producer)
{
    RTOS_PORT_ENTER_CRITICAL();
    uint32_t stop = shared_state.stop_workers;
    if (producer != 0U) {
        stop |= shared_state.stop_producers;
    }
    RTOS_PORT_EXIT_CRITICAL();
    return stop;
}

static void stress_heartbeat(uint32_t id)
{
    RTOS_PORT_ENTER_CRITICAL();
    shared_state.heartbeats[id]++;
    RTOS_PORT_EXIT_CRITICAL();
}

static void stress_completed(uint32_t id)
{
    RTOS_PORT_ENTER_CRITICAL();
    shared_state.completed[id]++;
    RTOS_PORT_EXIT_CRITICAL();
}

static uint32_t stress_random(stress_worker_t *worker)
{
    uint32_t value = worker->random_state;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    worker->random_state = value;
    return value;
}

static void stress_park(uint32_t id)
{
    RTOS_PORT_ENTER_CRITICAL();
    shared_state.parked_mask |= 1U << id;
    RTOS_PORT_EXIT_CRITICAL();
    rtos_status_t status = rtos_task_suspend(NULL);
    stress_record_failure("WORKER_PARK_RETURN", (int32_t)status, (int32_t)RTOS_OK);
    for (;;) {
        rtos_task_delay(100U);
    }
}

static rtos_status_t stress_resource_sample(uint32_t *value)
{
    rtos_status_t status = rtos_sem_take(&lease_sem, 5U);
    if (status != RTOS_OK) {
        return status;
    }
    status = rtos_mutex_take(&resource_mutex, 5U);
    if (status == RTOS_OK) {
        *value = resource_version;
        status = rtos_mutex_give(&resource_mutex);
    }
    rtos_status_t release = rtos_sem_give(&lease_sem);
    if (release != RTOS_OK) {
        stress_record_failure("LEASE_GIVE", (int32_t)release, (int32_t)RTOS_OK);
    }
    return status;
}

static void stress_producer(stress_worker_t *worker)
{
    uint32_t source = worker->id - 1U;
    uint32_t sequence = 0U;
    while (stress_stopping(1U) == 0U) {
        stress_heartbeat(worker->id);
        uint32_t phase = stress_phase();
        if (phase != STRESS_PHASE_BOOT) {
            uint32_t sample = 0U;
            rtos_status_t status = stress_resource_sample(&sample);
            if (status == RTOS_OK) {
                production_packet_t packet = {source, sequence, 0U, 0U};
                packet.payload = production_stress_payload(source, sequence,
                                                          PRODUCTION_STRESS_SEED);
                packet.checksum = production_stress_checksum(&packet);
                if ((PRODUCTION_STRESS_INJECT_CORRUPTION != 0U)
                    && (source == 0U) && (sequence == 3U)) {
                    packet.checksum ^= 1U;
                }
                status = rtos_queue_send(&data_queue, &packet, 2U, RTOS_FALSE);
                RTOS_PORT_ENTER_CRITICAL();
                if (status == RTOS_OK) {
                    shared_state.sent[source]++;
                    if ((PRODUCTION_STRESS_INJECT_CORRUPTION != 0U)
                        && (source == 0U) && (sequence == 3U)) {
                        shared_state.injected++;
                    }
                    sequence++;
                    shared_state.completed[worker->id]++;
                } else if (status == RTOS_ERR_TIMEOUT) {
                    shared_state.send_timeout[source]++;
                }
                RTOS_PORT_EXIT_CRITICAL();
                if ((status != RTOS_OK) && (status != RTOS_ERR_TIMEOUT)) {
                    stress_record_failure("QUEUE_SEND", (int32_t)status, (int32_t)RTOS_OK);
                }
                if ((status == RTOS_OK) && ((sequence & 7U) == 0U)) {
                    (void)rtos_event_set(&shared_event, 1U << (4U + (source & 1U)));
                }
                (void)sample; /* Configuration snapshot is exercised, not a real sensor input. */
            } else if (status != RTOS_ERR_TIMEOUT) {
                stress_record_failure("RESOURCE_SAMPLE", (int32_t)status, (int32_t)RTOS_OK);
            }
        }
        uint32_t delay = 8U + (stress_random(worker) & 3U);
        if (phase == STRESS_PHASE_HIGH) {
            delay = 1U + (stress_random(worker) & 3U);
        } else if (phase == STRESS_PHASE_RECOVER) {
            delay += 4U;
        }
        rtos_task_delay(delay);
    }
    stress_park(worker->id);
}

/* Called with kernel interrupts masked. A receive can complete before an earlier
 * consumer gets CPU time; the window tracks identities rather than completion order. */
static void stress_advance_ledger(stress_ledger_t *ledger)
{
    for (uint32_t count = 0U; count < STRESS_WINDOW_BITS; count++) {
        uint32_t slot = ledger->next_sequence & (STRESS_WINDOW_BITS - 1U);
        uint32_t mask = 1U << (slot & 31U);
        if ((ledger->pending[slot >> 5U] & mask) == 0U) {
            break;
        }
        ledger->pending[slot >> 5U] &= ~mask;
        ledger->next_sequence++;
    }
}

static void stress_validate_packet(const production_packet_t *packet)
{
    RTOS_PORT_ENTER_CRITICAL();
    shared_state.received++;
    RTOS_PORT_EXIT_CRITICAL();
    if ((packet->source >= STRESS_SOURCES)
        || (packet->checksum != production_stress_checksum(packet))) {
        RTOS_PORT_ENTER_CRITICAL();
        shared_state.checksum_errors++;
        RTOS_PORT_EXIT_CRITICAL();
        stress_record_failure("PACKET_CHECKSUM", (int32_t)packet->source,
                              (int32_t)STRESS_SOURCES);
        return;
    }
    uint32_t expected = production_stress_payload(packet->source, packet->sequence,
                                                 PRODUCTION_STRESS_SEED);
    if (packet->payload != expected) {
        stress_record_failure("PACKET_PAYLOAD", (int32_t)packet->payload, (int32_t)expected);
        return;
    }
    RTOS_PORT_ENTER_CRITICAL();
    stress_ledger_t *ledger = &ledgers[packet->source];
    uint32_t slot = packet->sequence & (STRESS_WINDOW_BITS - 1U);
    uint32_t mask = 1U << (slot & 31U);
    uint32_t is_invalid = (packet->sequence < ledger->next_sequence) ? 1U : 0U;
    if (is_invalid == 0U) {
        is_invalid = ((packet->sequence - ledger->next_sequence) >= STRESS_WINDOW_BITS)
                     || ((ledger->pending[slot >> 5U] & mask) != 0U);
    }
    if (is_invalid == 0U) {
        ledger->pending[slot >> 5U] |= mask;
        ledger->validated++;
        stress_advance_ledger(ledger);
    }
    uint32_t expected_sequence = ledger->next_sequence;
    RTOS_PORT_EXIT_CRITICAL();
    if (is_invalid != 0U) {
        stress_record_failure("PACKET_DUPLICATE_OR_GAP", (int32_t)packet->sequence,
                              (int32_t)expected_sequence);
    }
}

static void stress_consumer(stress_worker_t *worker)
{
    while (stress_stopping(0U) == 0U) {
        stress_heartbeat(worker->id);
        production_packet_t packet;
        rtos_status_t status = rtos_queue_recv(&data_queue, &packet, 5U);
        if (status == RTOS_OK) {
            stress_validate_packet(&packet);
            stress_completed(worker->id);
        } else if (status == RTOS_ERR_TIMEOUT) {
            RTOS_PORT_ENTER_CRITICAL();
            shared_state.receive_timeout++;
            RTOS_PORT_EXIT_CRITICAL();
        } else {
            stress_record_failure("QUEUE_RECV", (int32_t)status, (int32_t)RTOS_OK);
        }
        /* Bounded service capacity creates explicit backpressure in the high phase. */
        rtos_task_delay((stress_phase() == STRESS_PHASE_HIGH) ? 4U : 1U);
    }
    stress_park(worker->id);
}

static void stress_sem_service(stress_worker_t *worker)
{
    uint32_t index = worker->id - 9U;
    while (stress_stopping(0U) == 0U) {
        stress_heartbeat(worker->id);
        rtos_status_t status = rtos_sem_take(&irq_sem, 5U);
        RTOS_PORT_ENTER_CRITICAL();
        if (status == RTOS_OK) {
            shared_state.sem_taken[index]++;
            shared_state.completed[worker->id]++;
        } else if (status == RTOS_ERR_TIMEOUT) {
            shared_state.sem_timeout[index]++;
        }
        RTOS_PORT_EXIT_CRITICAL();
        if ((status != RTOS_OK) && (status != RTOS_ERR_TIMEOUT)) {
            stress_record_failure("IRQ_SEM_TAKE", (int32_t)status, (int32_t)RTOS_OK);
        }
        rtos_task_delay(1U);
    }
    stress_park(worker->id);
}

static void stress_event_service(stress_worker_t *worker)
{
    uint32_t index = worker->id - 11U;
    uint32_t mask = STRESS_TIMEOUT_MASK;
    uint32_t mode = RTOS_EVENT_WAIT_ANY;
    if (index == 0U) {
        mask = STRESS_ANY_MASK;
        mode |= RTOS_EVENT_CLEAR_ON_EXIT;
    } else if (index == 1U) {
        mask = STRESS_ALL_MASK;
        mode = RTOS_EVENT_WAIT_ALL | RTOS_EVENT_CLEAR_ON_EXIT;
    }
    while (stress_stopping(0U) == 0U) {
        stress_heartbeat(worker->id);
        uint32_t before = rtos_sched_get_tick_count();
        uint32_t bits = rtos_event_wait(&shared_event, mask, mode, 7U);
        uint32_t elapsed = rtos_sched_get_tick_count() - before;
        uint32_t matched = (index == 1U) ? ((bits & mask) == mask) : ((bits & mask) != 0U);
        RTOS_PORT_ENTER_CRITICAL();
        if ((matched != 0U) && (index < 2U)) {
            shared_state.event_success[index]++;
            shared_state.completed[worker->id]++;
        } else {
            shared_state.event_timeout[index]++;
        }
        RTOS_PORT_EXIT_CRITICAL();
        if ((index == 2U) && ((matched != 0U) || (elapsed < 7U))) {
            stress_record_failure("EVENT_TIMEOUT_CONTRACT", (int32_t)elapsed, 7);
        }
        rtos_task_delay(1U);
    }
    stress_park(worker->id);
}

static void stress_control(stress_worker_t *worker)
{
    rtos_tick_t last_wake = rtos_sched_get_tick_count();
    while (stress_stopping(0U) == 0U) {
        stress_heartbeat(worker->id);
        if (stress_phase() != STRESS_PHASE_BOOT) {
            uint32_t now = rtos_sched_get_tick_count();
            uint32_t jitter = now - last_wake;
            RTOS_PORT_ENTER_CRITICAL();
            if (jitter > shared_state.control_jitter_max) {
                shared_state.control_jitter_max = jitter;
            }
            RTOS_PORT_EXIT_CRITICAL();
            rtos_status_t status = rtos_mutex_take(&resource_mutex, 10U);
            if (status == RTOS_OK) {
                resource_version++;
                status = rtos_mutex_give(&resource_mutex);
                stress_completed(worker->id);
            }
            if (status != RTOS_OK) {
                stress_record_failure("CONTROL_MUTEX", (int32_t)status, (int32_t)RTOS_OK);
            }
            uint32_t response = rtos_sched_get_tick_count() - last_wake;
            RTOS_PORT_ENTER_CRITICAL();
            if (response > shared_state.control_response_max) {
                shared_state.control_response_max = response;
            }
            if (response > STRESS_CONTROL_DEADLINE) {
                shared_state.control_deadline_misses++;
            }
            RTOS_PORT_EXIT_CRITICAL();
        }
        rtos_task_delay_until(&last_wake, STRESS_CONTROL_PERIOD);
    }
    stress_park(worker->id);
}

static void stress_maintenance(stress_worker_t *worker)
{
    while (stress_stopping(0U) == 0U) {
        stress_heartbeat(worker->id);
        if (stress_phase() != STRESS_PHASE_BOOT) {
            rtos_status_t status = rtos_mutex_take(&resource_mutex, 10U);
            if (status == RTOS_OK) {
                resource_version++;
                /* A bounded device-bus hold lets high-priority clients really block. */
                rtos_task_delay(2U);
                status = rtos_mutex_give(&resource_mutex);
                stress_completed(worker->id);
            }
            if (status != RTOS_OK) {
                stress_record_failure("MAINTENANCE_MUTEX", (int32_t)status, (int32_t)RTOS_OK);
            }
            (void)rtos_event_set(&shared_event, 0x40U);
            rtos_task_delay(1U);
            (void)rtos_event_set(&shared_event, 0x80U);
        }
        rtos_task_delay(5U + (stress_random(worker) & 3U));
    }
    stress_park(worker->id);
}

static void stress_health(stress_worker_t *worker)
{
    while (stress_stopping(0U) == 0U) {
        stress_heartbeat(worker->id);
        rtos_status_t status = rtos_mutex_take(&resource_mutex, 10U);
        if (status == RTOS_OK) {
            uint32_t version = resource_version;
            status = rtos_mutex_give(&resource_mutex);
            (void)version;
            stress_completed(worker->id);
        }
        if (status != RTOS_OK) {
            stress_record_failure("HEALTH_MUTEX", (int32_t)status, (int32_t)RTOS_OK);
        }
        rtos_task_delay(20U);
    }
    stress_park(worker->id);
}

static void stress_worker_entry(void *arg)
{
    stress_worker_t *worker = (stress_worker_t *)arg;
    if (worker->id == 0U) {
        stress_control(worker);
    } else if (worker->id <= 4U) {
        stress_producer(worker);
    } else if (worker->id <= 8U) {
        stress_consumer(worker);
    } else if (worker->id <= 10U) {
        stress_sem_service(worker);
    } else if (worker->id <= 13U) {
        stress_event_service(worker);
    } else if (worker->id == 14U) {
        stress_maintenance(worker);
    } else {
        stress_health(worker);
    }
}

/* Only known static worker nodes are accepted before dereferencing a link. */
static uint32_t stress_node_index(const rtos_wait_node_t *node)
{
    for (uint32_t index = 0U; index < created; index++) {
        if (node == &worker_tcbs[index].wait_node) {
            return index;
        }
    }
    return STRESS_WORKERS;
}

static uint32_t stress_tcb_index(const rtos_tcb_t *tcb)
{
    for (uint32_t index = 0U; index < created; index++) {
        if (tcb == &worker_tcbs[index]) {
            return index;
        }
    }
    return STRESS_WORKERS;
}

static uint32_t stress_list_check(rtos_wait_node_t **head, const void *object,
                                  uint32_t is_ordered, uint32_t *bitmap)
{
    rtos_wait_node_t *node = *head;
    rtos_wait_node_t *previous = NULL;
    uint32_t visited = 0U;
    uint32_t count = 0U;
    uint32_t priority_bits = 0U;
    while (node != NULL) {
        uint32_t index = stress_node_index(node);
        if ((index >= created) || ((visited & (1U << index)) != 0U)) {
            stress_record_failure("WAIT_LINK_IDENTITY", (int32_t)index, (int32_t)created);
            break;
        }
        rtos_tcb_t *tcb = &worker_tcbs[index];
        if ((tcb->priority >= 32U) || (tcb->priority >= RTOS_CONFIG_MAX_PRIORITIES)) {
            stress_record_failure("WAIT_LINK_PRIORITY_RANGE", (int32_t)tcb->priority, 31);
            break;
        }
        if ((node->prev != previous) || (node->tcb != tcb)
            || (node->list_head != head) || (node->wait_obj != object)
            || (tcb->blocked_on != object)
            || ((tcb->state != RTOS_TASK_BLOCKED) && (tcb->state != RTOS_TASK_DELAYED))) {
            stress_record_failure("WAIT_LINK_MEMBERSHIP", (int32_t)index, 0);
            break;
        }
        if ((is_ordered != 0U) && (previous != NULL)
            && (previous->tcb->priority > tcb->priority)) {
            stress_record_failure("WAIT_LINK_PRIORITY", (int32_t)index, 0);
        }
        visited |= 1U << index;
        priority_bits |= 1U << tcb->priority;
        count++;
        previous = node;
        node = node->next;
    }
    for (uint32_t index = 0U; index < created; index++) {
        if ((worker_tcbs[index].wait_node.list_head == head)
            && ((visited & (1U << index)) == 0U)) {
            stress_record_failure("WAIT_LINK_ORPHAN", (int32_t)index, 0);
        }
    }
    if (bitmap != NULL) {
        *bitmap = priority_bits;
    }
    return count;
}

static void stress_update_peak(uint32_t value, uint32_t *peak)
{
    if (value > *peak) {
        *peak = value;
    }
}

static void stress_observe(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    uint32_t count = stress_list_check(&data_queue.recv_wait_head, &data_queue, 1U, NULL);
    stress_update_peak(count, &shared_state.queue_receive_peak);
    count = stress_list_check(&data_queue.send_wait_head, &data_queue, 1U, NULL);
    stress_update_peak(count, &shared_state.queue_send_peak);
    uint32_t bitmap = 0U;
    count = stress_list_check(&irq_sem.wait_head, &irq_sem, 0U, &bitmap);
    stress_update_peak(count, &shared_state.sem_peak);
    if (bitmap != irq_sem.wait_bitmap) {
        stress_record_failure("IRQ_SEM_BITMAP", (int32_t)irq_sem.wait_bitmap, (int32_t)bitmap);
    }
    count = stress_list_check(&lease_sem.wait_head, &lease_sem, 0U, &bitmap);
    stress_update_peak(count, &shared_state.lease_peak);
    if (bitmap != lease_sem.wait_bitmap) {
        stress_record_failure("LEASE_BITMAP", (int32_t)lease_sem.wait_bitmap, (int32_t)bitmap);
    }
    count = stress_list_check(&shared_event.wait_head, &shared_event, 1U, NULL);
    stress_update_peak(count, &shared_state.event_peak);
    count = stress_list_check(&resource_mutex.wait_head, &resource_mutex, 1U, NULL);
    stress_update_peak(count, &shared_state.mutex_peak);
    if (resource_mutex.owner != NULL) {
        uint32_t owner_index = stress_tcb_index(resource_mutex.owner);
        if (owner_index >= created) {
            stress_record_failure("MUTEX_OWNER_IDENTITY", (int32_t)owner_index,
                                  (int32_t)created);
        } else if ((resource_mutex.owner->priority < resource_mutex.owner->base_priority)
                   && (resource_mutex.wait_head != NULL)) {
            shared_state.inheritance_seen++;
        }
    }
    if (data_queue.count == data_queue.capacity) {
        shared_state.queue_full[shared_state.phase]++;
    } else if (data_queue.count == 0U) {
        shared_state.queue_empty[shared_state.phase]++;
    }
    if (data_queue.count > data_queue.capacity) {
        stress_record_failure("QUEUE_COUNT", (int32_t)data_queue.count,
                              (int32_t)data_queue.capacity);
    }
    RTOS_PORT_EXIT_CRITICAL();
}

static rtos_status_t stress_initialize(void)
{
    rtos_status_t status = rtos_queue_init(&data_queue, queue_storage,
                                         (uint32_t)sizeof(production_packet_t),
                                         PRODUCTION_STRESS_QUEUE_CAPACITY);
    if (status != RTOS_OK) {
        return status;
    }
    initialized |= STRESS_INIT_QUEUE;
    status = rtos_sem_init(&irq_sem, 0U, 64U);
    if (status != RTOS_OK) {
        return status;
    }
    initialized |= STRESS_INIT_IRQ_SEM;
    status = rtos_sem_init(&lease_sem, 2U, 2U);
    if (status != RTOS_OK) {
        return status;
    }
    initialized |= STRESS_INIT_LEASE_SEM;
    status = rtos_mutex_init(&resource_mutex);
    if (status != RTOS_OK) {
        return status;
    }
    initialized |= STRESS_INIT_MUTEX;
    status = rtos_event_init(&shared_event);
    if (status != RTOS_OK) {
        return status;
    }
    initialized |= STRESS_INIT_EVENT;
    for (uint32_t index = 0U; index < STRESS_WORKERS; index++) {
        worker_args[index].id = index;
        worker_args[index].random_state = PRODUCTION_STRESS_SEED ^ ((index + 1U) * 0x9E3779B9U);
        if (worker_args[index].random_state == 0U) {
            worker_args[index].random_state = 1U;
        }
        status = rtos_task_create_static(&worker_tcbs[index], worker_stacks[index],
                                         STRESS_STACK_WORDS, stress_worker_entry,
                                         &worker_args[index], 5U + index, "prod_worker");
        if (status != RTOS_OK) {
            return status;
        }
        created++;
    }
    return RTOS_OK;
}

static void stress_print_progress(uint32_t elapsed)
{
    stress_state_t snapshot;
    RTOS_PORT_ENTER_CRITICAL();
    snapshot = shared_state;
    RTOS_PORT_EXIT_CRITICAL();
    if ((snapshot.injected != 0U) && (injection_reported == 0U)) {
        stress_log("STRESS|INJECT|checksum_corruption|source=0|sequence=3\r\n");
        injection_reported = 1U;
    }
    if (snapshot.checksum_errors != 0U) {
        stress_log("STRESS|FAIL|packet_integrity checksum_errors=%lu\r\n",
                   (unsigned long)snapshot.checksum_errors);
    }
    stress_log("STRESS|PROGRESS|ticks=%lu phase=%lu recv=%lu errors=%lu full=%lu\r\n",
                 (unsigned long)elapsed, (unsigned long)snapshot.phase,
                 (unsigned long)snapshot.received, (unsigned long)snapshot.errors,
                 (unsigned long)snapshot.queue_full[snapshot.phase]);
    if (snapshot.errors != 0U) {
        stress_log("STRESS|FAIL|first=%s actual=%ld expected=%ld errors=%lu\r\n",
                     snapshot.first_error, (long)snapshot.first_actual,
                     (long)snapshot.first_expected, (unsigned long)snapshot.errors);
    }
}

static uint32_t stress_task_timeout_sum(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    uint32_t sum = 0U;
    for (uint32_t index = 0U; index < 4U; index++) {
        sum += shared_state.send_timeout[index];
    }
    RTOS_PORT_EXIT_CRITICAL();
    return sum;
}

static void stress_watchdog_initialize(void)
{
    uint32_t now = rtos_sched_get_tick_count();
    for (uint32_t index = 0U; index < STRESS_WORKERS; index++) {
        watchdog_heartbeat_tick[index] = now;
        watchdog_work_tick[index] = now;
    }
    watchdog_last_check = now;
}

/* Each role must execute and deliver its service result within two seconds.
 * This is a scenario acceptance budget, not an assertion of scheduler fairness. */
static void stress_watchdog_check(void)
{
    uint32_t now = rtos_sched_get_tick_count();
    if ((now - watchdog_last_check) < 1000U) {
        return;
    }
    watchdog_last_check = now;
    RTOS_PORT_ENTER_CRITICAL();
    for (uint32_t index = 0U; index < STRESS_WORKERS; index++) {
        uint32_t heartbeat = shared_state.heartbeats[index];
        uint32_t work = shared_state.completed[index];
        if (index == 13U) {
            work = shared_state.event_timeout[2];
        }
        if (heartbeat != watchdog_heartbeat[index]) {
            watchdog_heartbeat[index] = heartbeat;
            watchdog_heartbeat_tick[index] = now;
        }
        if (work != watchdog_work[index]) {
            watchdog_work[index] = work;
            watchdog_work_tick[index] = now;
        }
        if (((now - watchdog_heartbeat_tick[index]) >= 2000U)
            || ((now - watchdog_work_tick[index]) >= 2000U)) {
            shared_state.watchdog_misses++;
            stress_record_failure("ROLE_PROGRESS_WATCHDOG", (int32_t)index, 2000);
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
}

static void stress_run_phase(uint32_t phase, uint32_t duration)
{
    uint32_t start = rtos_sched_get_tick_count();
    uint32_t next_log = 5000U;
    uint32_t before_heartbeats[STRESS_WORKERS];
    RTOS_PORT_ENTER_CRITICAL();
    shared_state.phase = phase;
    memcpy(before_heartbeats, shared_state.heartbeats, sizeof(before_heartbeats));
    RTOS_PORT_EXIT_CRITICAL();
    production_irq_snapshot_t before;
    rtos_status_t status = production_irq_snapshot(&before);
    stress_check((status == RTOS_OK), "phase_irq_snapshot_start");
    uint32_t timeout_before = stress_task_timeout_sum();
    stress_log("STRESS|PHASE|phase=%lu ticks=%lu\r\n", (unsigned long)phase,
                 (unsigned long)duration);
    while ((rtos_sched_get_tick_count() - start) < duration) {
        stress_observe();
        stress_watchdog_check();
        uint32_t elapsed = rtos_sched_get_tick_count() - start;
        if (elapsed >= next_log) {
            stress_print_progress(elapsed);
            next_log += 5000U;
        }
        rtos_task_delay(1U);
    }
    production_irq_snapshot_t after;
    status = production_irq_snapshot(&after);
    stress_check((status == RTOS_OK), "phase_irq_snapshot_end");
    uint32_t all_progress = 1U;
    RTOS_PORT_ENTER_CRITICAL();
    for (uint32_t index = 0U; index < STRESS_WORKERS; index++) {
        if (shared_state.heartbeats[index] == before_heartbeats[index]) {
            all_progress = 0U;
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
    stress_check(all_progress, "all_sixteen_workers_progress_each_phase");
    if (phase == STRESS_PHASE_HIGH) {
        stress_check((after.rejected[0] + after.rejected[1])
                     > (before.rejected[0] + before.rejected[1]), "irq_backpressure_hit");
        stress_check(stress_task_timeout_sum() > timeout_before, "task_send_timeout_hit");
    } else if (phase == STRESS_PHASE_RECOVER) {
        stress_check(shared_state.queue_empty[phase] != 0U, "recovery_reaches_empty_queue");
    }
    stress_print_progress(duration);
}

static uint32_t stress_total_sent(const production_irq_snapshot_t *irq)
{
    RTOS_PORT_ENTER_CRITICAL();
    uint32_t total = irq->sent[0] + irq->sent[1];
    for (uint32_t index = 0U; index < 4U; index++) {
        total += shared_state.sent[index];
    }
    RTOS_PORT_EXIT_CRITICAL();
    return total;
}

static void stress_stop_and_drain(production_irq_snapshot_t *irq)
{
    RTOS_PORT_ENTER_CRITICAL();
    shared_state.stop_producers = 1U;
    RTOS_PORT_EXIT_CRITICAL();
    rtos_status_t status = production_irq_stop();
    stress_check(status == RTOS_OK, "irq_stop_restores_peripherals");
    status = production_irq_snapshot(irq);
    stress_check(status == RTOS_OK, "irq_stopped_snapshot");
    uint32_t start = rtos_sched_get_tick_count();
    uint32_t is_drained = 0U;
    while ((rtos_sched_get_tick_count() - start) < STRESS_SHUTDOWN_TICKS) {
        stress_observe();
        uint32_t total = stress_total_sent(irq);
        RTOS_PORT_ENTER_CRITICAL();
        is_drained = ((shared_state.parked_mask & 0x1EU) == 0x1EU)
                     && (data_queue.count == 0U) && (shared_state.received == total);
        RTOS_PORT_EXIT_CRITICAL();
        if (is_drained != 0U) {
            break;
        }
        rtos_task_delay(1U);
    }
    stress_check(is_drained, "bounded_graceful_queue_drain");
    RTOS_PORT_ENTER_CRITICAL();
    shared_state.stop_workers = 1U;
    RTOS_PORT_EXIT_CRITICAL();
    start = rtos_sched_get_tick_count();
    uint32_t is_parked = 0U;
    while ((rtos_sched_get_tick_count() - start) < 200U) {
        RTOS_PORT_ENTER_CRITICAL();
        is_parked = shared_state.parked_mask == 0xFFFFU;
        for (uint32_t index = 0U; index < created; index++) {
            if (worker_tcbs[index].state != RTOS_TASK_SUSPENDED) {
                is_parked = 0U;
            }
        }
        RTOS_PORT_EXIT_CRITICAL();
        if (is_parked != 0U) {
            break;
        }
        rtos_task_delay(1U);
    }
    stress_check(is_parked, "all_workers_acknowledge_shutdown");
}

static uint32_t stress_stack_free_words(const rtos_tcb_t *tcb)
{
    uint32_t index = 0U; /* PERF fills the whole unused region, including word zero. */
    while ((index < tcb->stack_size) && (tcb->stack_base[index] == 0xA5A5A5A5U)) {
        index++;
    }
    return index;
}

static void stress_verify_workers(void)
{
    for (uint32_t index = 0U; index < created; index++) {
        uint32_t free_words = stress_stack_free_words(&worker_tcbs[index]);
        stress_log("STRESS|WORKER|id=%lu priority=%lu heartbeat=%lu completed=%lu "
                     "stack_free_words=%lu switches=%lu\r\n", (unsigned long)index,
                     (unsigned long)worker_tcbs[index].base_priority,
                     (unsigned long)shared_state.heartbeats[index],
                     (unsigned long)shared_state.completed[index], (unsigned long)free_words,
                     (unsigned long)worker_tcbs[index].switch_count);
        stress_check(free_words >= STRESS_STACK_GUARD_WORDS, "worker_stack_margin");
        stress_check((worker_tcbs[index].stack_magic == 0xDEADBEEFU)
                     && (worker_tcbs[index].stack_base[0] == 0xA5A5A5A5U),
                     "worker_stack_magic_and_low_watermark");
        stress_check(worker_tcbs[index].state == RTOS_TASK_SUSPENDED, "worker_is_suspended");
        stress_check(worker_tcbs[index].priority == worker_tcbs[index].base_priority,
                     "worker_priority_restored");
        uint32_t has_work = shared_state.completed[index] != 0U;
        if (index == 13U) {
            has_work = shared_state.event_timeout[2] != 0U;
        }
        stress_check(has_work, "worker_role_has_real_result");
    }
    rtos_tcb_t *controller = rtos_task_get_current();
    uint32_t free_words = stress_stack_free_words(controller);
    stress_log("STRESS|CONTROLLER|stack_free_words=%lu\r\n", (unsigned long)free_words);
    stress_check(free_words >= STRESS_STACK_GUARD_WORDS, "controller_stack_margin");
}

static void stress_verify_messages(const production_irq_snapshot_t *irq)
{
    for (uint32_t source = 0U; source < STRESS_SOURCES; source++) {
        uint32_t sent = (source < 4U) ? shared_state.sent[source] : irq->sent[source - 4U];
        uint32_t pending = 0U;
        for (uint32_t word = 0U; word < STRESS_WINDOW_WORDS; word++) {
            pending |= ledgers[source].pending[word];
        }
        stress_log("STRESS|SOURCE|source=%lu sent=%lu validated=%lu prefix=%lu pending=%lu\r\n",
                     (unsigned long)source, (unsigned long)sent,
                     (unsigned long)ledgers[source].validated,
                     (unsigned long)ledgers[source].next_sequence, (unsigned long)pending);
        stress_check(sent != 0U, "source_has_successful_send");
        stress_check((ledgers[source].validated == sent)
                     && (ledgers[source].next_sequence == sent) && (pending == 0U),
                     "every_successful_identity_validated_exactly_once");
    }
    uint32_t sem_taken = shared_state.sem_taken[0] + shared_state.sem_taken[1];
    uint32_t sem_pending = rtos_sem_get_count(&irq_sem);
    stress_log("STRESS|SEMAPHORE|accepted=%lu taken0=%lu taken1=%lu pending=%lu overflow=%lu\r\n",
                 (unsigned long)irq->sem_give_success, (unsigned long)shared_state.sem_taken[0],
                 (unsigned long)shared_state.sem_taken[1], (unsigned long)sem_pending,
                 (unsigned long)irq->sem_give_overflow);
    stress_check(sem_taken + sem_pending == irq->sem_give_success, "irq_semaphore_conservation");
    stress_check((shared_state.sem_taken[0] != 0U) && (shared_state.sem_taken[1] != 0U),
                 "both_shared_semaphore_consumers_serviced");
}

static void stress_verify_coverage(const production_irq_snapshot_t *irq)
{
    stress_log("STRESS|COVERAGE|queue_recv_waiters=%lu queue_send_waiters=%lu "
                 "sem_waiters=%lu lease_waiters=%lu event_waiters=%lu mutex_waiters=%lu "
                 "pi_observed=%lu high_full=%lu recovery_empty=%lu\r\n",
                 (unsigned long)shared_state.queue_receive_peak,
                 (unsigned long)shared_state.queue_send_peak, (unsigned long)shared_state.sem_peak,
                 (unsigned long)shared_state.lease_peak, (unsigned long)shared_state.event_peak,
                 (unsigned long)shared_state.mutex_peak,
                 (unsigned long)shared_state.inheritance_seen,
                 (unsigned long)shared_state.queue_full[STRESS_PHASE_HIGH],
                 (unsigned long)shared_state.queue_empty[STRESS_PHASE_RECOVER]);
    stress_check(shared_state.queue_receive_peak >= 4U, "four_simultaneous_queue_receivers");
    stress_check(shared_state.queue_send_peak >= 1U, "blocking_queue_sender_exercised");
    stress_check(shared_state.sem_peak >= 2U, "two_simultaneous_semaphore_waiters");
    stress_check(shared_state.event_peak >= 3U, "three_simultaneous_event_waiters");
    stress_check(shared_state.lease_peak >= 1U, "shared_resource_semaphore_contention");
    stress_check(shared_state.inheritance_seen != 0U, "real_priority_inheritance_observed");
    stress_check((shared_state.event_success[0] != 0U) && (shared_state.event_success[1] != 0U)
                 && (shared_state.event_timeout[2] != 0U), "event_any_all_timeout_exercised");
    stress_check(shared_state.control_deadline_misses == 0U, "synthetic_control_deadline");
    stress_check(shared_state.watchdog_misses == 0U, "two_second_role_progress_watchdog");
    stress_log("STRESS|CONTROL|period_ticks=%lu deadline_ticks=%lu jitter_max=%lu "
                 "response_max=%lu misses=%lu watchdog_misses=%lu\r\n",
                 (unsigned long)STRESS_CONTROL_PERIOD, (unsigned long)STRESS_CONTROL_DEADLINE,
                 (unsigned long)shared_state.control_jitter_max,
                 (unsigned long)shared_state.control_response_max,
                 (unsigned long)shared_state.control_deadline_misses,
                 (unsigned long)shared_state.watchdog_misses);
    stress_log("STRESS|IRQ|entries0=%lu entries1=%lu sent0=%lu sent1=%lu rejected0=%lu "
                 "rejected1=%lu nested=%lu depth_max=%lu forced=%lu errors=%lu\r\n",
                 (unsigned long)irq->entries[0], (unsigned long)irq->entries[1],
                 (unsigned long)irq->sent[0], (unsigned long)irq->sent[1],
                 (unsigned long)irq->rejected[0], (unsigned long)irq->rejected[1],
                 (unsigned long)irq->nesting_count, (unsigned long)irq->depth_max,
                 (unsigned long)irq->forced_observed, (unsigned long)irq->api_error_count);
    stress_check((irq->entries[0] != 0U) && (irq->entries[1] != 0U), "both_real_timer_irqs");
    stress_check((irq->nesting_count != 0U) && (irq->depth_max >= 2U)
                 && (irq->forced_observed != 0U), "actual_nvic_nested_preemption");
    stress_check((irq->api_error_count == 0U) && (irq->is_running == 0U), "irq_clean_stop");
    stress_check(production_irq_status() == RTOS_OK, "irq_context_and_register_contracts");
}

static void stress_verify_empty_lists(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    uint32_t is_empty = (data_queue.send_wait_head == NULL)
                        && (data_queue.recv_wait_head == NULL) && (irq_sem.wait_head == NULL)
                        && (lease_sem.wait_head == NULL) && (resource_mutex.wait_head == NULL)
                        && (shared_event.wait_head == NULL) && (irq_sem.wait_bitmap == 0U)
                        && (lease_sem.wait_bitmap == 0U);
    RTOS_PORT_EXIT_CRITICAL();
    stress_check(is_empty, "all_fixture_wait_chains_empty");
    stress_check(resource_mutex.owner == NULL, "shared_mutex_released");
    stress_check(rtos_sem_get_count(&lease_sem) == 2U, "all_resource_leases_returned");
}

static void stress_cleanup(void)
{
    for (uint32_t index = 0U; index < created; index++) {
        rtos_status_t status = rtos_task_delete(&worker_tcbs[index]);
        stress_check(status == RTOS_OK, "static_worker_delete");
    }
    if ((initialized & STRESS_INIT_EVENT) != 0U) {
        stress_check(rtos_event_deinit(&shared_event) == RTOS_OK, "event_deinit");
    }
    if ((initialized & STRESS_INIT_MUTEX) != 0U) {
        stress_check(rtos_mutex_deinit(&resource_mutex) == RTOS_OK, "mutex_deinit");
    }
    if ((initialized & STRESS_INIT_LEASE_SEM) != 0U) {
        stress_check(rtos_sem_deinit(&lease_sem) == RTOS_OK, "lease_sem_deinit");
    }
    if ((initialized & STRESS_INIT_IRQ_SEM) != 0U) {
        stress_check(rtos_sem_deinit(&irq_sem) == RTOS_OK, "irq_sem_deinit");
    }
    if ((initialized & STRESS_INIT_QUEUE) != 0U) {
        stress_check(rtos_queue_deinit(&data_queue) == RTOS_OK, "queue_deinit");
    }
}

static void stress_verify_heap(const stress_baseline_t *baseline)
{
    uint32_t current_free = rtos_heap_get_free();
    uint32_t current_max = rtos_heap_get_max_alloc();
    stress_log("STRESS|HEAP|before=%lu after=%lu max_before=%lu max_after=%lu\r\n",
                 (unsigned long)baseline->heap_free, (unsigned long)current_free,
                 (unsigned long)baseline->heap_max, (unsigned long)current_max);
    stress_check((current_free == baseline->heap_free) && (current_max == baseline->heap_max),
                 "static_fixture_preserves_heap");
    stress_check((rtos_heap_get_alloc_count() == baseline->allocations)
                 && (rtos_heap_get_free_count() == baseline->frees), "no_fixture_heap_allocation");
}

static void stress_finish(void)
{
    uint32_t errors = shared_state.errors;
    if (errors != 0U) {
        stress_print_progress(0U);
    }
    stress_log("STRESS|SUMMARY|pass=%lu fail=%lu worker_errors=%lu\r\n",
                 (unsigned long)check_pass, (unsigned long)(check_fail + errors),
                 (unsigned long)errors);
    stress_log("STRESS|DONE|status=%ld\r\n", (long)(((check_fail + errors) == 0U) ? 0 : -1));
    for (;;) {
        rtos_task_delay(1000U);
    }
}

void production_stress_task(void *arg)
{
    (void)arg;
    stress_baseline_t baseline = {rtos_heap_get_free(), rtos_heap_get_max_alloc(),
                                  rtos_heap_get_alloc_count(), rtos_heap_get_free_count()};
    stress_log("STRESS|BEGIN|seed=%lu seconds=%lu queue_capacity=%lu workers=%lu "
                 "priorities=5..20 synthetic=1 inject=%lu\r\n",
                 (unsigned long)PRODUCTION_STRESS_SEED, (unsigned long)PRODUCTION_STRESS_SECONDS,
                 (unsigned long)PRODUCTION_STRESS_QUEUE_CAPACITY, (unsigned long)STRESS_WORKERS,
                 (unsigned long)PRODUCTION_STRESS_INJECT_CORRUPTION);
    stress_verify_golden();
    rtos_status_t status = stress_initialize();
    stress_check(status == RTOS_OK, "initialize_sixteen_workers_and_ipc");
    if (status != RTOS_OK) {
        stress_cleanup();
        stress_verify_heap(&baseline);
        stress_finish();
    }
    /* IRQs and producers remain stopped while all consumers really enter wait lists. */
    for (uint32_t tick = 0U; tick < 25U; tick++) {
        stress_observe();
        rtos_task_delay(1U);
    }
    production_irq_config_t irq_config = {&data_queue, &irq_sem, &shared_event,
                                          PRODUCTION_STRESS_SEED};
    status = production_irq_start(&irq_config);
    stress_check(status == RTOS_OK, "start_two_hardware_interrupt_sources");
    if (status != RTOS_OK) {
        stress_cleanup();
        stress_verify_heap(&baseline);
        stress_finish();
    }
    uint32_t duration = PRODUCTION_STRESS_SECONDS * 1000U;
    uint32_t third = duration / 3U;
    stress_watchdog_initialize();
    stress_run_phase(STRESS_PHASE_NORMAL, third);
    stress_run_phase(STRESS_PHASE_HIGH, third);
    stress_run_phase(STRESS_PHASE_RECOVER, duration - (2U * third));
    production_irq_snapshot_t irq;
    stress_stop_and_drain(&irq);
    stress_observe();
    stress_verify_workers();
    stress_verify_messages(&irq);
    stress_verify_coverage(&irq);
    stress_verify_empty_lists();
    stress_cleanup();
    stress_verify_heap(&baseline);
    stress_check(shared_state.errors == 0U, "all_runtime_contracts");
    stress_finish();
}
