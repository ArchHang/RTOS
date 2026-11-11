/**
 * @file robustness_core.h
 * @brief Core task, mask and scheduling regressions for the robustness firmware.
 */
#ifndef ROBUSTNESS_CORE_H_
#define ROBUSTNESS_CORE_H_

#include "rtos_task.h"

/** @brief Run the core regressions from the robustness controller task. */
void robustness_core_run(void);

/**
 * @brief Record the idle stack hook invocation during the physical guard test.
 * @param[in] task Task reported by the idle stack checker.
 */
void robustness_core_record_stack_overflow(rtos_tcb_t *task);

#endif
