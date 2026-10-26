/**
 * @file production_highirq.h
 * @brief Independent priority-zero TIM2 load for the production stress fixture.
 *
 * The high-priority handler never calls the RTOS or its critical primitives.
 * Nonzero BASEPRI samples are expected when it preempts a kernel critical body.
 * Snapshot and lifetime operations use short PRIMASK sections to protect the
 * separate ISR-owned statistics; they preserve the caller's original PRIMASK.
 */
#ifndef F446_RTOS_HIL_FW_INC_PRODUCTION_HIGHIRQ_H_
#define F446_RTOS_HIL_FW_INC_PRODUCTION_HIGHIRQ_H_

#include <stdint.h>

#include "rtos_types.h"

/* Bit positions identify the exact failed restoration check. A zero mask is
 * required; these diagnostics never relax a register equality requirement. */
#define PRODUCTION_HIGHIRQ_RESTORE_CR1 (1U << 0U)
#define PRODUCTION_HIGHIRQ_RESTORE_CR2 (1U << 1U)
#define PRODUCTION_HIGHIRQ_RESTORE_DIER (1U << 2U)
#define PRODUCTION_HIGHIRQ_RESTORE_PSC (1U << 3U)
#define PRODUCTION_HIGHIRQ_RESTORE_ARR (1U << 4U)
#define PRODUCTION_HIGHIRQ_RESTORE_CNT (1U << 5U)
#define PRODUCTION_HIGHIRQ_RESTORE_SR (1U << 6U)
#define PRODUCTION_HIGHIRQ_RESTORE_PRIORITY (1U << 7U)
#define PRODUCTION_HIGHIRQ_RESTORE_ENABLE (1U << 8U)
#define PRODUCTION_HIGHIRQ_RESTORE_PENDING (1U << 9U)
#define PRODUCTION_HIGHIRQ_RESTORE_CLOCK (1U << 10U)
#define PRODUCTION_HIGHIRQ_RESTORE_UPDATE_TIMEOUT (1U << 11U)

typedef struct production_highirq_snapshot {
    uint32_t entries;
    uint32_t natural_updates;
    uint32_t forced_requests;
    uint32_t forced_observed;
    uint32_t nonzero_basepri_samples;
    uint32_t nested_entries;
    uint32_t triple_nested_entries;
    uint32_t spurious_entries;
    uint32_t last_basepri;
    uint32_t last_ipsr;
    uint32_t priority;
    uint32_t work_checksum;
    uint32_t errors;
    uint32_t is_running;
    uint32_t restored;
    uint32_t restore_mismatch;
    uint32_t stop_pending_before;
    uint32_t stop_pending_after;
    uint32_t restore_pending_after;
    uint32_t restore_sr_actual;
    uint32_t configure_reload_polls;
    uint32_t restore_reload_polls;
    uint32_t reload_timeouts;
} production_highirq_snapshot_t;

/**
 * @brief Acquire an idle TIM2 and start priority-zero 37/71-us periodic load.
 *
 * Requires privileged thread context, no BASEPRI/FAULTMASK, the F446 84-MHz
 * APB1 timer clock, and an idle timer/NVIC line. An incoming PRIMASK=1 is allowed
 * so the parent fixture can atomically start all three timers. Busy timers are
 * rejected before any timer or NVIC register is modified.
 * @return RTOS_OK, RTOS_ERR_ALREADY for occupied resources, RTOS_ERR_ISR/SCHED
 *         for unsuitable context, or RTOS_ERR_PARAM for the clock configuration.
 */
rtos_status_t production_highirq_start(void);

/**
 * @brief Stop TIM2 and restore every modified timer, NVIC and clock register.
 *
 * Requires thread context and ownership acquired by start. Original idle timer
 * register values are restored; the stopped prescaler phase is not retained.
 * Interrupt sources and APB writes are quiesced before clearing the NVIC pending
 * latch. Snapshot restore_mismatch reports the individual failed equalities.
 * Software updates use a bounded UIF acknowledgement before status is cleared;
 * a barrier by itself does not acknowledge the timer's internal update event.
 * @return RTOS_OK, RTOS_ERR_ISR, RTOS_ERR_ALREADY when not owned, or
 *         RTOS_ERR_PARAM when restoration or pending-request checks fail.
 */
rtos_status_t production_highirq_stop(void);

/**
 * @brief Copy a coherent ISR-statistics snapshot under a saved PRIMASK.
 * @param snapshot [out] Caller-owned destination; cannot be NULL.
 * @return RTOS_OK, RTOS_ERR_NULL, or RTOS_ERR_ISR for an ISR caller.
 */
rtos_status_t production_highirq_snapshot(production_highirq_snapshot_t *snapshot);

/**
 * @brief Pend TIM2 from a nested TIM7 handler to prove three active IRQ levels.
 *
 * This function contains no RTOS calls. It atomically publishes a reason and
 * pends TIM2, preserving PRIMASK; a natural update may coincide with the request.
 * Calls from an inappropriate ISR are recorded as errors and do not pend TIM2.
 * @return No result; counters are exposed by production_highirq_snapshot().
 */
void production_highirq_force_nested(void);

/**
 * @brief CMSIS TIM2 vector executing bounded integer work and register sampling.
 * @return No result. The startup vector requires this exact symbol name.
 */
void TIM2_IRQHandler(void);

#endif /* F446_RTOS_HIL_FW_INC_PRODUCTION_HIGHIRQ_H_ */
