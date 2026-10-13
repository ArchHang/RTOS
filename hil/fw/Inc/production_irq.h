/**
 * @file production_irq.h
 * @brief STM32F446 production stress test dual-timer interrupt fixture.
 *
 * Only the isolated stress firmware owns TIM6/TIM7. The caller owns IPC objects
 * and must keep them initialized until stop returns. Snapshot counters are protected
 * by the RTOS critical section; event bits are level flags, not counted messages.
 */
#ifndef F446_RTOS_HIL_FW_INC_PRODUCTION_IRQ_H_
#define F446_RTOS_HIL_FW_INC_PRODUCTION_IRQ_H_

#include <stdint.h>

#include "rtos_types.h"

typedef struct production_irq_config {
    rtos_queue_t *queue;
    rtos_sem_t *sem;
    rtos_event_t *event;
    uint32_t seed;
} production_irq_config_t;

typedef struct production_irq_snapshot {
    uint32_t entries[2];
    uint32_t natural[2];
    uint32_t forced[2];
    uint32_t sent[2];
    uint32_t rejected[2];
    uint32_t spurious[2];
    uint32_t ipsr[2];
    uint32_t priority[2];
    uint32_t sem_give_success;
    uint32_t sem_give_overflow;
    uint32_t event_set_count;
    uint32_t nesting_count;
    uint32_t forced_requests;
    uint32_t forced_observed;
    uint32_t api_error_count;
    uint32_t depth_max;
    uint32_t is_running;
} production_irq_snapshot_t;

/**
 * @brief Start TIM6 at 1 ms/priority 7 and TIM7 at 701 us/priority 6.
 *
 * Both ISR sources send checked packets, give the same semaphore, and set event
 * bits 0/1. Every 128th TIM6 update pends TIM7 to exercise actual preemption.
 * Natural updates and software pending requests are counted separately and may
 * coincide in one TIM7 entry. Queue-full/semaphore-full are expected backpressure.
 * Requires privileged unmasked thread context and both timers/NVIC lines idle;
 * busy peripherals are rejected. No allocation or SysTick modification occurs.
 * @param config [in] Initialized IPC objects and deterministic payload seed.
 * @return RTOS_OK, RTOS_ERR_NULL/PARAM for invalid arguments, RTOS_ERR_ISR/SCHED
 *         for unsuitable context, or RTOS_ERR_ALREADY for busy timer/fixture.
 */
rtos_status_t production_irq_start(const production_irq_config_t *config);

/**
 * @brief Quiesce both IRQ sources and restore saved idle peripheral state.
 *
 * Timer visible registers, prescaler reload, NVIC priorities, and clock enable bits
 * are restored. The prescaler phase of a previously stopped timer is not retained.
 * No original pending interrupt is discarded because start rejects pending lines.
 * @return RTOS_OK, RTOS_ERR_ISR/SCHED for unsuitable context, RTOS_ERR_ALREADY
 *         when stopped, or RTOS_ERR_PARAM if restoration verification fails.
 */
rtos_status_t production_irq_stop(void);

/**
 * @brief Copy a consistent set of counters; stopped counters remain stable.
 * @param snapshot [out] Caller-owned destination; cannot be NULL.
 * @return RTOS_OK, RTOS_ERR_NULL, or RTOS_ERR_ISR for an ISR caller.
 */
rtos_status_t production_irq_snapshot(production_irq_snapshot_t *snapshot);

/**
 * @brief Return the accumulated fixture error status in thread context.
 * @return RTOS_OK when started at least once without API/context/restore errors,
 *         RTOS_ERR_PARAM for recorded errors, RTOS_ERR_ALREADY before first start,
 *         or RTOS_ERR_ISR for an ISR caller.
 */
rtos_status_t production_irq_status(void);

#endif /* F446_RTOS_HIL_FW_INC_PRODUCTION_IRQ_H_ */
