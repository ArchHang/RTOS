/**
 * @file robustness_services.c
 * @brief Target regressions for USB resource ownership and performance windows.
 */
#include "robustness_services.h"

#include <stdbool.h>
#include <stdint.h>

#include "robustness.h"
#include "rtos.h"

#if RTOS_CONFIG_USE_USB
static void check_usb_repeat_init(void)
{
    const uint8_t pattern = 0xA7U;
    rtos_status_t status = rtos_usb_init();
    robustness_check("usb_osal_first_init", status == RTOS_OK);
    if (status != RTOS_OK) {
        return;
    }

    rtos_usb_sem_t *sem = rtos_usb_sem_create(1U, 1U);
    uint8_t *block = rtos_usb_malloc(64U);
    robustness_check("usb_osal_repeat_setup", (sem != NULL) && (block != NULL));
    if ((sem == NULL) || (block == NULL)) {
        rtos_usb_sem_delete(sem);
        rtos_usb_free(block);
        return;
    }

    block[0] = pattern;
    status = rtos_usb_init();
    robustness_check("usb_osal_repeat_rejected", status == RTOS_ERR_ALREADY);
    robustness_check("usb_osal_repeat_preserves_bytes", block[0] == pattern);
    uint8_t *next_block = rtos_usb_malloc(64U);
    robustness_check("usb_osal_repeat_preserves_block_owner",
                     (next_block != NULL) && (next_block != block));
    robustness_check("usb_osal_repeat_preserves_live_semaphore",
                     rtos_usb_sem_take(sem, RTOS_NO_WAIT) == RTOS_OK);

    if (next_block != block) {
        rtos_usb_free(next_block);
    }
    rtos_usb_free(block);
    rtos_usb_sem_delete(sem);
}
#endif

#if RTOS_CONFIG_USE_PERF_MONITOR && RTOS_CONFIG_PERF_HOTPATH_STATS
static void check_perf_current_slice(void)
{
    rtos_perf_task_t before = { 0 };
    rtos_perf_task_t after = { 0 };
#if RTOS_CONFIG_PERF_USE_DWT
    const uint32_t sample_delta = 20000U;
    const uint32_t guard = 1000000U;
#else
    const uint32_t sample_delta = 2U;
    const uint32_t guard = 2000000U;
#endif

    rtos_perf_reset();
    rtos_status_t before_status = rtos_perf_get_task(NULL, &before);
    uint32_t start = rtos_perf_get_cycles();
    for (uint32_t i = 0U; i < guard; i++) {
        if ((rtos_perf_get_cycles() - start) >= sample_delta) {
            break;
        }
    }
    uint32_t elapsed = rtos_perf_get_cycles() - start;
    rtos_status_t after_status = rtos_perf_get_task(NULL, &after);

    bool is_sample_valid = (before_status == RTOS_OK) && (after_status == RTOS_OK)
                           && (elapsed >= sample_delta)
                           && (before.task_id == after.task_id)
                           && (before.switch_count == after.switch_count);
    robustness_check("perf_running_slice_sample_without_switch", is_sample_valid);
    robustness_check("perf_running_slice_updates_cpu_and_cycles",
                     is_sample_valid && (after.cpu_usage_x100 >= 9900U)
                     && ((after.run_cycles - before.run_cycles) >= sample_delta));
}
#endif

/** @brief Run service regressions from the robustness fixture task. */
void robustness_services_run(void)
{
#if RTOS_CONFIG_USE_USB
    check_usb_repeat_init();
#endif
#if RTOS_CONFIG_USE_PERF_MONITOR && RTOS_CONFIG_PERF_HOTPATH_STATS
    check_perf_current_slice();
#endif
}
