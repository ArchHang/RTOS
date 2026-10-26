/**
 * @file basepri_regressions.h
 * @brief STM32F446 interrupt-mask regression fixture for the existing audit entry.
 *
 * These entry points replace audit_regressions.c only in the basepri HIL variant.
 * The daily main and the original audit fixture remain unchanged.
 */
#ifndef RTOS_HIL_FW_INC_BASEPRI_REGRESSIONS_H_
#define RTOS_HIL_FW_INC_BASEPRI_REGRESSIONS_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Record fixture metadata before scheduler startup, without changing masks.
 * @return No result; the task entry performs the actual tests after startup.
 */
void audit_pre_start(void);

/**
 * @brief Run mask interleavings, real IRQ interference and scheduling checks.
 * @param arg [in] Unused audit-task argument; may be NULL.
 * @return Does not return; prints BP|DONE and waits after completing the checks.
 */
void audit_regressions_task(void *arg);

/**
 * @brief Handle the TIM7 vector owned exclusively by this HIL fixture.
 * @return No result. The CMSIS startup vector requires this exact symbol name.
 */
void TIM7_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_HIL_FW_INC_BASEPRI_REGRESSIONS_H_ */
