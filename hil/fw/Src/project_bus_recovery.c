#include "audit_regressions.h"
#include "rtos.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define BUS_STACK_WORDS 256U
#define BUS_WORKER_COUNT 2U
#define BUS_WAIT_TICKS 20U
#define BUS_COMMAND_BASE 0x5100U
#define BUS_RESTART_COMMAND 0x7200U

typedef enum bus_operation {
    BUS_HANDOFF,
    BUS_CLOSE,
    BUS_RESTART,
} bus_operation_t;

typedef struct bus_registers {
    uint32_t generation;
    uint32_t command;
    uint32_t writes;
} bus_registers_t;

typedef struct bus_request {
    uint32_t generation;
    uint32_t command;
    rtos_status_t take_status;
    rtos_status_t give_status;
    bool is_owner_at_return;
    bool is_done;
} bus_request_t;

static rtos_mutex_t bus_mutex;
static bus_registers_t bus_registers;
static bus_request_t bus_requests[BUS_WORKER_COUNT];
static rtos_tcb_t bus_tasks[BUS_WORKER_COUNT];
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT)
    rtos_stack_t bus_stacks[BUS_WORKER_COUNT][BUS_STACK_WORDS];

static void submit_bus_request(void *argument)
{
    bus_request_t *request = argument;
    request->take_status = rtos_mutex_take(&bus_mutex, RTOS_WAIT_FOREVER);
    if (request->take_status == RTOS_OK) {
        request->is_owner_at_return = bus_mutex.owner == rtos_task_get_current();
        /* Quarantine a stale success before it can mutate the simulated device. */
        if (request->is_owner_at_return && (request->generation == bus_registers.generation)) {
            bus_registers.command = request->command;
            ++bus_registers.writes;
            request->give_status = rtos_mutex_give(&bus_mutex);
        }
    }
    RTOS_PORT_ENTER_CRITICAL();
    request->is_done = true;
    RTOS_PORT_EXIT_CRITICAL();
    (void)rtos_task_suspend(NULL);
}

static bool wait_until_queued(void)
{
    for (uint32_t tick = 0U; tick < BUS_WAIT_TICKS; ++tick) {
        RTOS_PORT_ENTER_CRITICAL();
        bool is_queued = (bus_mutex.wait_head != NULL)
                         && (bus_mutex.wait_head->tcb == &bus_tasks[0])
                         && (bus_mutex.wait_head->next != NULL)
                         && (bus_mutex.wait_head->next->tcb == &bus_tasks[1]);
        RTOS_PORT_EXIT_CRITICAL();
        if (is_queued) {
            return true;
        }
        (void)rtos_task_delay(1U);
    }
    return false;
}

static bool wait_until_done(void)
{
    for (uint32_t tick = 0U; tick < BUS_WAIT_TICKS; ++tick) {
        RTOS_PORT_ENTER_CRITICAL();
        bool done = bus_requests[0].is_done && bus_requests[1].is_done;
        RTOS_PORT_EXIT_CRITICAL();
        if (done) {
            return true;
        }
        (void)rtos_task_delay(1U);
    }
    return false;
}

static bool check_request(const bus_request_t *request, rtos_status_t expected)
{
    return request->is_done && (request->take_status == expected)
           && ((expected != RTOS_OK)
               || (request->is_owner_at_return && (request->give_status == RTOS_OK)));
}

static bool run_bus_operation(bus_operation_t operation)
{
    rtos_tcb_t *supervisor = rtos_task_get_current();
    uint32_t tasks_created = 0U;
    bool is_initialized = rtos_mutex_init(&bus_mutex) == RTOS_OK;
    bool is_valid = is_initialized && (rtos_mutex_take(&bus_mutex, RTOS_NO_WAIT) == RTOS_OK)
                    && (rtos_mutex_take(&bus_mutex, RTOS_NO_WAIT) == RTOS_OK);

    bus_registers = (bus_registers_t){.generation = 1U};
    for (uint32_t index = 0U; is_valid && (index < BUS_WORKER_COUNT); ++index) {
        bus_requests[index] = (bus_request_t){
            .generation = bus_registers.generation,
            .command = BUS_COMMAND_BASE + index,
            .take_status = RTOS_ERR_SCHED,
            .give_status = RTOS_ERR_SCHED,
        };
        is_valid = rtos_task_create_static(&bus_tasks[index], bus_stacks[index], BUS_STACK_WORDS,
                                           submit_bus_request, &bus_requests[index],
                                           (rtos_prio_t)(supervisor->base_priority + 1U + index),
                                           "bus_client") == RTOS_OK;
        if (is_valid) {
            ++tasks_created;
        }
    }

    is_valid = is_valid && wait_until_queued();
    is_valid = is_valid && (rtos_mutex_give(&bus_mutex) == RTOS_OK)
               && (bus_mutex.owner == supervisor) && (bus_mutex.recursion_count == 1U);
    is_valid = is_valid && (rtos_mutex_give(&bus_mutex) == RTOS_OK)
               && (bus_mutex.owner == &bus_tasks[0]) && (bus_mutex.recursion_count == 1U)
               && (bus_tasks[0].state == RTOS_TASK_READY)
               && (bus_mutex.wait_head != NULL) && (bus_mutex.wait_head->tcb == &bus_tasks[1]);

    if (is_valid && (operation != BUS_HANDOFF)) {
        is_valid = rtos_mutex_deinit(&bus_mutex) == RTOS_OK;
        is_initialized = !is_valid;
        ++bus_registers.generation;
        if (is_valid && (operation == BUS_RESTART)) {
            is_initialized = rtos_mutex_init(&bus_mutex) == RTOS_OK;
            is_valid = is_initialized && (rtos_mutex_take(&bus_mutex, RTOS_NO_WAIT) == RTOS_OK);
            if (is_valid) {
                bus_registers.command = BUS_RESTART_COMMAND;
                ++bus_registers.writes;
            }
        }
    }

    is_valid = is_valid && wait_until_done();
    if (operation == BUS_HANDOFF) {
        is_valid = is_valid && (bus_registers.command == BUS_COMMAND_BASE + 1U)
                   && (bus_registers.writes == BUS_WORKER_COUNT) && (bus_mutex.owner == NULL);
    } else if (operation == BUS_RESTART) {
        is_valid = is_valid && (bus_registers.command == BUS_RESTART_COMMAND)
                   && (bus_registers.writes == 1U) && (bus_mutex.owner == supervisor);
        is_valid = (rtos_mutex_give(&bus_mutex) == RTOS_OK) && is_valid;
    } else {
        is_valid = is_valid && (bus_registers.writes == 0U);
    }

    if (tasks_created == BUS_WORKER_COUNT) {
        rtos_status_t expected = (operation == BUS_HANDOFF) ? RTOS_OK : RTOS_ERR_DELETED;
        is_valid = check_request(&bus_requests[0], expected)
                   && check_request(&bus_requests[1], expected) && is_valid;
    }
    for (uint32_t index = 0U; index < tasks_created; ++index) {
        is_valid = (rtos_task_delete(&bus_tasks[index]) == RTOS_OK) && is_valid;
    }
    if (is_initialized) {
        is_valid = (rtos_mutex_deinit(&bus_mutex) == RTOS_OK) && is_valid;
    }
    return is_valid;
}

void audit_pre_start(void)
{
}

void audit_regressions_task(void *argument)
{
    static const char *const names[] = {"normal_handoff", "deinit_pending", "reinit_pending"};
    uint32_t failures = 0U;

    (void)argument;
    for (uint32_t index = 0U; index < (sizeof(names) / sizeof(names[0])); ++index) {
        bool is_valid = run_bus_operation((bus_operation_t)index);
        if (!is_valid) {
            ++failures;
        }
        (void)printf("BUSREC|%s|%s|granted=%d|queued=%d|owns=%u|writes=%lu\r\n", names[index],
                     is_valid ? "PASS" : "FAIL", (int)bus_requests[0].take_status,
                     (int)bus_requests[1].take_status,
                     (unsigned int)bus_requests[0].is_owner_at_return,
                     (unsigned long)bus_registers.writes);
    }
    (void)printf("BUSREC|DONE|pass=%lu|fail=%lu\r\n",
                 (unsigned long)((sizeof(names) / sizeof(names[0])) - failures),
                 (unsigned long)failures);
    for (;;) {
        (void)rtos_task_delay(1000U);
    }
}
