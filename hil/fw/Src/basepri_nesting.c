/**
 * @file basepri_nesting.c
 * @brief Assert-off invalid-nesting state preservation before scheduler startup.
 *
 * The test deliberately calls EXIT at zero and ENTER at UINT32_MAX. These are
 * invalid inputs, not supported locking operations. It verifies that rejection
 * returns without wrapping the count or changing the caller's CPU masks or saved
 * slot; it does not claim that an invalid count has been repaired. Each sample
 * is taken before explicit fixture restoration, and UART output follows restore.
 * No tasks or RTOS ticks may access critical metadata during these injections.
 */
#include "audit_regressions.h"

#include <stdint.h>
#include <stdio.h>

#include "stm32f4xx_hal.h"

#include "rtos.h"

#if RTOS_CONFIG_ASSERT_ENABLED != 0
#error "basepri_nesting.c requires RTOS_CONFIG_ASSERT_ENABLED=0 in the entire firmware"
#endif
#if !defined(FIRMWARE_AUDIT) || (FIRMWARE_AUDIT != 1)
#error "basepri_nesting.c requires the dedicated audit-entry HIL variant"
#endif
#if defined(RTOS_HIL_CRITICAL_TEST)
#error "Invalid-nesting tests must not contain port test hooks"
#endif

#define NESTING_SLOT_SENTINEL (0xA5A55A5AU)

typedef struct nesting_state {
    uint32_t basepri;
    uint32_t primask;
    uint32_t count;
    uint32_t slot;
} nesting_state_t;

static uint32_t pass_count;
static uint32_t fail_count;
static uint32_t case_count;

static void run_case(uint32_t is_enter, uint32_t basepri, uint32_t primask)
{
    nesting_state_t saved;
    nesting_state_t actual;
    uint32_t expected_count = is_enter != 0U ? UINT32_MAX : 0U;
    uint32_t is_valid;

    saved.primask = __get_PRIMASK();
    __disable_irq();
    saved.basepri = __get_BASEPRI();
    saved.count = rtos_port_crit_nest;
    saved.slot = rtos_port_saved_mask;
    rtos_port_crit_nest = expected_count;
    rtos_port_saved_mask = NESTING_SLOT_SENTINEL;
    __set_BASEPRI(basepri);
    __set_PRIMASK(primask);
    __ISB();
    if (is_enter != 0U) {
        RTOS_PORT_ENTER_CRITICAL();
    } else {
        RTOS_PORT_EXIT_CRITICAL();
    }
    actual.basepri = __get_BASEPRI();
    actual.primask = __get_PRIMASK();
    actual.count = rtos_port_crit_nest;
    actual.slot = rtos_port_saved_mask;
    /* Restore only after observing rejection; invalid metadata is never kept
     * across a UART call or the later scheduler startup. */
    __disable_irq();
    rtos_port_crit_nest = saved.count;
    rtos_port_saved_mask = saved.slot;
    __set_BASEPRI(saved.basepri);
    __set_PRIMASK(saved.primask);
    __ISB();
    is_valid = (actual.basepri == basepri) && (actual.primask == primask)
        && (actual.count == expected_count) && (actual.slot == NESTING_SLOT_SENTINEL);
    case_count++;
    if (is_valid != 0U) {
        pass_count++;
    } else {
        fail_count++;
    }
    (void)printf("NEST|CASE|%s|id=%lu operation=%s in_bp=0x%02lX in_pm=%lu "
                 "out_bp=0x%02lX out_pm=%lu count=0x%08lX expected_count=0x%08lX "
                 "slot=0x%08lX expected_slot=0x%08lX returned=1 repair=fixture_only\r\n",
                 is_valid != 0U ? "PASS" : "FAIL", (unsigned long)case_count,
                 is_enter != 0U ? "overflow_enter" : "underflow_exit",
                 (unsigned long)basepri, (unsigned long)primask,
                 (unsigned long)actual.basepri, (unsigned long)actual.primask,
                 (unsigned long)actual.count, (unsigned long)expected_count,
                 (unsigned long)actual.slot, (unsigned long)NESTING_SLOT_SENTINEL);
}

/**
 * @brief Run all 20 invalid-input cases before RTOS tick handling is enabled.
 * @return No result; counters are reported by the audit task after startup.
 */
void audit_pre_start(void)
{
    static const uint32_t masks[] = {0U, 0x20U, 0x50U, 0x80U, 0xF0U};

    if ((rtos_sched_get_state() != RTOS_SCHED_NOT_STARTED) || (__get_IPSR() != 0U)
        || (__get_BASEPRI() != 0U) || (__get_PRIMASK() != 0U)) {
        fail_count++;
        (void)printf("NEST|CHECK|FAIL|prestart_unmasked_thread_required\r\n");
        return;
    }
    for (uint32_t operation = 0U; operation < 2U; operation++) {
        for (uint32_t mask = 0U; mask < sizeof(masks) / sizeof(masks[0]); mask++) {
            run_case(operation, masks[mask], 0U);
            run_case(operation, masks[mask], 1U);
        }
    }
}

/**
 * @brief Report pre-start observations without injecting invalid task state.
 * @param arg [in] Unused audit-task argument; may be NULL.
 * @return Does not return; waits after NEST|DONE.
 */
void audit_regressions_task(void *arg)
{
    (void)arg;
    if (case_count != 20U) {
        fail_count++;
    }
    (void)printf("NEST|SUMMARY|pass=%lu fail=%lu cases=%lu asserts=0\r\n",
                 (unsigned long)pass_count, (unsigned long)fail_count,
                 (unsigned long)case_count);
    (void)printf("NEST|DONE|status=%lu\r\n", fail_count == 0U ? 0UL : 1UL);
    for (;;) {
        rtos_task_delay(1000U);
    }
}
