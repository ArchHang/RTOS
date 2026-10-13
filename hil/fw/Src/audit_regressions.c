#include "audit_regressions.h"
#include "audit_ipc.h"
#include "audit_memory_timer.h"
#include "audit_usb_port.h"
#include "rtos.h"
#include "main.h"
#include <stdio.h>

static unsigned pass_count, fail_count;
static rtos_tcb_t *published;
static volatile unsigned observed, ran;
static rtos_tcb_t constructing_tcb;
static rtos_stack_t constructing_stack[256U] __attribute__((aligned(8)));
static rtos_tcb_t *constructing_target;
static volatile unsigned constructing_hook_ran;
static volatile unsigned constructing_resume_rejected, constructing_priority_rejected;
static volatile unsigned constructing_notify_rejected, constructing_suspend_rejected;

static void check(const char *name, int ok)
{
    if (ok) ++pass_count; else ++fail_count;
    printf("AT|SCHEDTASK|%s|%s\r\n", name, ok ? "PASS" : "FAIL");
}

static void child(void *arg)
{
    (void)arg;
    observed = (published == rtos_task_get_current());
    ran = 1U;
    rtos_task_suspend(NULL);
    for (;;) rtos_task_delay(1000U);
}

/* Model permitted ISR mutations at the construction window. On the old
 * implementation resume would enqueue a task with top_of_stack == NULL;
 * keep interrupts masked until that unsafe decision has been repaired. */
void audit_task_constructing_hook(rtos_tcb_t *tcb)
{
    if (tcb != constructing_target) return;
    RTOS_PORT_ENTER_CRITICAL();
    rtos_tcb_t before = *tcb;
    rtos_tcb_t *saved_next = rtos_kernel.next_tcb;
    uint32_t saved_pending = SCB->ICSR & SCB_ICSR_PENDSVSET_Msk;
    constructing_hook_ran = 1U;
    constructing_resume_rejected = rtos_task_resume(tcb) != RTOS_OK;
    if (tcb->state == RTOS_TASK_READY) {
        rtos_sched_remove_ready(tcb);
        tcb->state = before.state;
    }
    constructing_priority_rejected = rtos_task_set_priority(tcb, 7U) != RTOS_OK;
    constructing_notify_rejected = rtos_task_notify(tcb, 0x1234U, RTOS_NOTIFY_VALUE) != RTOS_OK;
    constructing_suspend_rejected = rtos_task_suspend(tcb) != RTOS_OK;
    /* All metadata changes are local to this reserved TCB. Delete is verified
     * through its shared membership guard, since deleting the old reservation
     * here would remove the private registry node and is unsafe to repair. */
    *tcb = before;
    rtos_kernel.next_tcb = saved_next;
    SCB->ICSR = saved_pending ? SCB_ICSR_PENDSVSET_Msk : SCB_ICSR_PENDSVCLR_Msk;
    RTOS_PORT_EXIT_CRITICAL();
}

static void construction_tests(void)
{
    constructing_hook_ran = 0U;
    constructing_resume_rejected = 0U;
    constructing_priority_rejected = 0U;
    constructing_notify_rejected = 0U;
    constructing_suspend_rejected = 0U;
    constructing_target = &constructing_tcb;
    rtos_status_t status = rtos_task_create_static(&constructing_tcb, constructing_stack,
                                                  256U, child, NULL, 20U, "construct");
    constructing_target = NULL;
    check("construct_window_exercised", status == RTOS_OK && constructing_hook_ran == 1U);
    check("construct_resume_rejected", constructing_resume_rejected == 1U);
    check("construct_priority_rejected", constructing_priority_rejected == 1U);
    check("construct_notify_rejected", constructing_notify_rejected == 1U);
    check("construct_suspend_rejected", constructing_suspend_rejected == 1U);
    if (status == RTOS_OK) {
        RTOS_PORT_ENTER_CRITICAL();
        rtos_stack_t *saved_sp = constructing_tcb.top_of_stack;
        rtos_task_state_t saved_state = constructing_tcb.state;
        rtos_tcb_t *saved_all = constructing_tcb.next_all;
        rtos_status_t duplicate = rtos_task_create_static(&constructing_tcb, constructing_stack,
                                                         256U, child, NULL, 20U, "duplicate");
        int preserved = duplicate == RTOS_ERR_ALREADY &&
                        constructing_tcb.top_of_stack == saved_sp &&
                        constructing_tcb.state == saved_state &&
                        constructing_tcb.next_all == saved_all;
        RTOS_PORT_EXIT_CRITICAL();
        check("static_duplicate_preserves_task", preserved);
        check("static_delete_live", rtos_task_delete(&constructing_tcb) == RTOS_OK);
        check("static_delete_after_delete", rtos_task_delete(&constructing_tcb) == RTOS_ERR_PARAM);
        rtos_status_t recreated = rtos_task_create_static(&constructing_tcb, constructing_stack,
                                                         256U, child, NULL, 20U, "recreated");
        check("static_recreate_after_delete", recreated == RTOS_OK);
        if (recreated == RTOS_OK) (void)rtos_task_delete(&constructing_tcb);
    }
}

void audit_pre_start(void)
{
    /* These void APIs must leave a not-started scheduler unchanged. */
    RTOS_PORT_ENTER_CRITICAL();
    rtos_sched_suspend();
    int suspend_ok = rtos_sched_get_state() == RTOS_SCHED_NOT_STARTED;
    rtos_sched_resume();
    int resume_ok = rtos_sched_get_state() == RTOS_SCHED_NOT_STARTED;
    /* Repair the baseline's observable state so the fixture can still boot. */
    rtos_kernel.sched_state = RTOS_SCHED_NOT_STARTED;
    rtos_kernel.next_tcb = NULL;
    SCB->ICSR = SCB_ICSR_PENDSVCLR_Msk;
    RTOS_PORT_EXIT_CRITICAL();
    check("suspend_before_start", suspend_ok);
    check("resume_before_start", resume_ok);
    check("notify_wait_before_start", rtos_task_notify_wait(RTOS_NO_WAIT, NULL) == RTOS_ERR_SCHED);

    rtos_tcb_t *saved_idle = rtos_kernel.idle_tcb;
    rtos_status_t duplicate_init = rtos_init();
    check("duplicate_init_preserves_idle", duplicate_init == RTOS_ERR_ALREADY &&
          rtos_kernel.idle_tcb == saved_idle);

    RTOS_PORT_ENTER_CRITICAL();
    uint32_t saved_bitmap = rtos_kernel.ready_bitmap;
    uint32_t saved_mask;
    uint32_t returned_mask;
    uint32_t strict_mask = 32U;
    __asm volatile("mrs %0, basepri" : "=r"(saved_mask) :: "memory");
    rtos_kernel.ready_bitmap = 0U; /* Force only the failed-start path. */
    __asm volatile("msr basepri, %0\n isb" :: "r"(strict_mask) : "memory");
    rtos_status_t empty_start = rtos_start();
    __asm volatile("mrs %0, basepri" : "=r"(returned_mask) :: "memory");
    int preserved_mask = empty_start == RTOS_ERR_SCHED && returned_mask == strict_mask &&
                         rtos_sched_get_state() == RTOS_SCHED_NOT_STARTED;
    __asm volatile("msr basepri, %0\n isb" :: "r"(saved_mask) : "memory");
    rtos_kernel.ready_bitmap = saved_bitmap;
    RTOS_PORT_EXIT_CRITICAL();
    check("empty_start_preserves_basepri", preserved_mask);
}

static void scheduling_tests(void)
{
    rtos_tcb_t *self = rtos_task_get_current();
    construction_tests();
    published = NULL; observed = 0U; ran = 0U;
    rtos_status_t st = rtos_task_create(&published, 256U, child, NULL, 8U, "publish");
    check("create_child_ran", st == RTOS_OK && ran == 1U);
    check("handle_before_preemption", observed == 1U);
    if (published != NULL) rtos_task_delete(published);

    published = NULL; ran = 0U;
    st = rtos_task_create(&published, 256U, child, NULL, self->priority, "yield_peer");
    rtos_sched_suspend();
    rtos_task_yield();
    check("yield_while_suspended", st == RTOS_OK && ran == 0U);
    rtos_sched_resume();
    rtos_task_yield();
    check("yield_after_resume", ran == 1U);
    if (published != NULL) rtos_task_delete(published);

    published = NULL; ran = 0U;
    rtos_task_create(&published, 256U, child, NULL, 20U, "prio_peer");
    rtos_sched_suspend();
    rtos_task_set_priority(published, 8U);
    check("priority_while_suspended", ran == 0U);
    rtos_sched_resume();
    check("priority_after_resume", ran == 1U);
    if (published != NULL) rtos_task_delete(published);

    rtos_sched_suspend();
    rtos_sched_suspend();
    rtos_sched_resume();
    check("nested_suspend", rtos_sched_get_state() == RTOS_SCHED_SUSPENDED);
    rtos_sched_resume();
    check("nested_resume", rtos_sched_get_state() == RTOS_SCHED_RUNNING);

    /* Model a legal ISR wake before an already pending PendSV is serviced.
     * Exercise list and decision transitions under the same critical section;
     * no blocking API is called with interrupts masked. */
    RTOS_PORT_ENTER_CRITICAL();
    rtos_sched_block(self, RTOS_WAIT_FOREVER);
    rtos_sched_schedule();
    rtos_sched_unblock(self);
    int canceled = (rtos_kernel.next_tcb == NULL || rtos_kernel.next_tcb == self);
    int running_state = self->state == RTOS_TASK_RUNNING;
    rtos_kernel.next_tcb = NULL;
    self->state = RTOS_TASK_RUNNING;
    RTOS_PORT_EXIT_CRITICAL();
    check("cancel_obsolete_switch", canceled);
    check("canceled_switch_restores_running", running_state);
    check("idle_cannot_suspend", rtos_task_suspend(rtos_kernel.idle_tcb) == RTOS_ERR_PARAM);
    /* Restore the old baseline if it accepted the idle suspension. */
    if (rtos_kernel.idle_tcb->state == RTOS_TASK_SUSPENDED) rtos_task_resume(rtos_kernel.idle_tcb);
}

void audit_regressions_task(void *arg)
{
    (void)arg;
    printf("AT|META|audit_20261012\r\n");
    scheduling_tests();
    printf("AT|SCHEDTASK|SUMMARY|pass=%u fail=%u\r\n", pass_count, fail_count);
    audit_ipc_run();
    audit_memory_timer_run();
    audit_usb_port_run();
    printf("AT|DONE\r\n");
    for (;;) rtos_task_delay(1000U);
}
