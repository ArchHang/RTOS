/** @file f446_reap_irq_pressure.c
 * @brief Reclaim self-deleted tasks when an IRQ wakes a second allocator.
 */

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "hil_target.h"
#include "rtos.h"

enum {
    REAP_IRQ_OBSERVER_PRIORITY = 5U,
    REAP_IRQ_CHILD_PRIORITY = 6U,
    REAP_IRQ_TEMP_PRIORITY = 8U,
    REAP_IRQ_OBSERVER_STACK_WORDS = 256U,
    REAP_IRQ_CHILD_STACK_WORDS = 160U,
    REAP_IRQ_TEMP_STACK_WORDS = 512U,
    REAP_IRQ_MAX_CHILDREN = 128U,
    REAP_IRQ_WAIT_TICKS = 100U
};

static rtos_tcb_t s_observer;
static _Alignas(8) rtos_stack_t s_observer_stack[REAP_IRQ_OBSERVER_STACK_WORDS];
static rtos_sem_t s_trigger;
static rtos_sem_t s_done;
static _Atomic bool s_is_armed;
static _Atomic uint32_t s_irq_count;
static _Atomic uint32_t s_children_exited;
static rtos_status_t s_irq_status;
static rtos_status_t s_observer_wait;
static rtos_status_t s_observer_create;
static rtos_status_t s_observer_delete;
static uint32_t s_before_observer_max;

void f446_reap_irq_pressure_run(void *argument);
void EXTI2_IRQHandler(void);
void __wrap_rtos_heap_free(void *pointer);
void __real_rtos_heap_free(void *pointer);

void EXTI2_IRQHandler(void)
{
    (void)atomic_fetch_add_explicit(&s_irq_count, 1U, memory_order_relaxed);
    s_irq_status = rtos_sem_give(&s_trigger);
}

void __wrap_rtos_heap_free(void *pointer)
{
    /* Wakes the higher-priority allocator at the first reaper release. */
    if (pointer != NULL && atomic_exchange_explicit(&s_is_armed, false, memory_order_relaxed)) {
        NVIC_SetPendingIRQ(EXTI2_IRQn);
        __DSB();
        __ISB();
    }
    __real_rtos_heap_free(pointer);
}

static void run_child(void *argument)
{
    (void)argument;
    (void)atomic_fetch_add_explicit(&s_children_exited, 1U, memory_order_relaxed);
}

static void run_observer(void *argument)
{
    (void)argument;
    s_observer_wait = rtos_sem_take(&s_trigger, REAP_IRQ_WAIT_TICKS);
    if (s_observer_wait == RTOS_OK) {
        rtos_tcb_t *temporary = NULL;
        s_before_observer_max = rtos_heap_get_max_alloc();
        s_observer_create = rtos_task_create(&temporary, REAP_IRQ_TEMP_STACK_WORDS, run_child,
                                             NULL, REAP_IRQ_TEMP_PRIORITY, "reap-irq-temp");
        if (s_observer_create == RTOS_OK) {
            s_observer_delete = rtos_task_delete(temporary);
        }
    }
    /* The coordinator checks the corresponding take, including timeout. */
    (void)rtos_sem_give(&s_done);
    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}

void f446_reap_irq_pressure_run(void *argument)
{
    (void)argument;
    const uint32_t initial_max = rtos_heap_get_max_alloc();
    const uint32_t initial_live = rtos_heap_get_alloc_count() - rtos_heap_get_free_count();
    const bool trigger_ready = rtos_sem_init(&s_trigger, 0U, 1U) == RTOS_OK;
    const bool done_ready = rtos_sem_init(&s_done, 0U, 1U) == RTOS_OK;
    s_observer_wait = RTOS_ERR_SCHED;
    s_observer_create = RTOS_ERR_SCHED;
    s_observer_delete = RTOS_ERR_SCHED;
    s_irq_status = RTOS_ERR_SCHED;

    const bool observer_ready = trigger_ready && done_ready &&
        rtos_task_create_static(&s_observer, s_observer_stack, REAP_IRQ_OBSERVER_STACK_WORDS,
                                run_observer, NULL, REAP_IRQ_OBSERVER_PRIORITY, "reap-irq-hi") ==
            RTOS_OK;
    uint32_t created = 0U;
    bool completed = false;
    bool churn_ok = observer_ready;
    if (observer_ready) {
        const uint32_t irq_priority = RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY >>
                                      (8U - __NVIC_PRIO_BITS);
        NVIC_SetPriority(EXTI2_IRQn, irq_priority);
        NVIC_ClearPendingIRQ(EXTI2_IRQn);
        NVIC_EnableIRQ(EXTI2_IRQn);
        atomic_store_explicit(&s_is_armed, true, memory_order_relaxed);
        while (created < REAP_IRQ_MAX_CHILDREN &&
               atomic_load_explicit(&s_irq_count, memory_order_relaxed) == 0U) {
            rtos_tcb_t *child = NULL;
            if (rtos_task_create(&child, REAP_IRQ_CHILD_STACK_WORDS, run_child, NULL,
                                 REAP_IRQ_CHILD_PRIORITY, "reap-irq-child") != RTOS_OK) {
                churn_ok = false;
                break;
            }
            ++created;
        }
        atomic_store_explicit(&s_is_armed, false, memory_order_relaxed);
        completed = rtos_sem_take(&s_done, REAP_IRQ_WAIT_TICKS) == RTOS_OK;
        NVIC_DisableIRQ(EXTI2_IRQn);
        NVIC_ClearPendingIRQ(EXTI2_IRQn);
    }

    const uint32_t irq = atomic_load_explicit(&s_irq_count, memory_order_relaxed);
    const uint32_t exited = atomic_load_explicit(&s_children_exited, memory_order_relaxed);
    const bool setup_ok = churn_ok && completed && irq == 1U && s_irq_status == RTOS_OK &&
        s_observer_wait == RTOS_OK && exited == created &&
        s_before_observer_max < REAP_IRQ_TEMP_STACK_WORDS * sizeof(rtos_stack_t);
    const bool allocation_ok = s_observer_create == RTOS_OK && s_observer_delete == RTOS_OK;
    const bool observer_clean = !observer_ready || rtos_task_delete(&s_observer) == RTOS_OK;
    const bool trigger_clean = !trigger_ready || rtos_sem_deinit(&s_trigger) == RTOS_OK;
    const bool done_clean = !done_ready || rtos_sem_deinit(&s_done) == RTOS_OK;
    rtos_task_delay(4U);
    const bool clean = observer_clean && trigger_clean && done_clean &&
        rtos_heap_get_max_alloc() == initial_max &&
        rtos_heap_get_alloc_count() - rtos_heap_get_free_count() == initial_live;

    (void)printf("REAPIRQ|RESULT|created=%lu|exited=%lu|irq=%lu|max_before=%lu|alloc=%ld|"
                 "delete=%ld|setup=%u|clean=%u\r\n", (unsigned long)created, (unsigned long)exited,
                 (unsigned long)irq, (unsigned long)s_before_observer_max,
                 (long)s_observer_create, (long)s_observer_delete,
                 (unsigned int)setup_ok, (unsigned int)clean);
    const uint32_t passed = (uint32_t)setup_ok + (uint32_t)allocation_ok + (uint32_t)clean;
    (void)printf("REAPIRQ|DONE|pass=%lu|fail=%lu\r\n",
                 (unsigned long)passed, (unsigned long)(3U - passed));
}
