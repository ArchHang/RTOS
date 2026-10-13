/**
 * @file flash_diag.c
 * @brief 同一测试 ELF 内的 STM32F446 Flash 指令缓存开关诊断。
 *
 * 仅由测试控制任务调用；回调期间中断正常运行，禁止其他任务或 ISR 修改 Flash。
 * 所有诊断输出与资源快照位于回调自有计时之外；不复位 DWT，不修改时钟或 prefetch。
 *
 * RM0390 第 3.4 节：128 bit Flash 取数，64 行指令缓存，8 行 Flash 数据缓存。
 * RM0390 第 3.8.1 节：仅在 ICEN=0 时操作 ICRST，缓存开关不改变 LATENCY。
 * https://www.st.com.cn/resource/en/reference_manual/dm00135183.pdf
 * ES0298 第 2.2.3 节：保留 PRFTEN，避免关闭 prefetch 后的调试休眠勘误条件。
 * 勘误官方下载入口：
 * https://www.st.com/en/microcontrollers-microprocessors/stm32f446/documentation.html
 */
#include "flash_diag.h"

#include <stdint.h>
#include <stdio.h>

#include "stm32f4xx.h"

#include "rtos_heap.h"
#include "rtos_perf.h"
#include "rtos_port.h"
#include "rtos_sched.h"

#if !defined(FIRMWARE_FLASH_DIAG) || (FIRMWARE_FLASH_DIAG != 1)
#error "flash_diag.c requires the isolated FIRMWARE_FLASH_DIAG=1 test variant"
#endif

#if !defined(STM32F446xx) || !defined(__ARM_ARCH_7EM__)
#error "flash_diag.c supports only STM32F446 Cortex-M4 test firmware"
#endif

typedef struct flash_diag_snapshot {
    const rtos_tcb_t *current_tcb;
    uint32_t tick_count;
    uint32_t task_count;
    uint32_t heap_free_bytes;
    uint32_t ready_bitmap;
} flash_diag_snapshot_t;

typedef struct flash_diag_result {
    uint32_t pass_count;
    uint32_t fail_count;
} flash_diag_result_t;

static rtos_status_t check_benchmark_context(void)
{
    if ((__get_IPSR() != 0U) || ((__get_CONTROL() & CONTROL_nPRIV_Msk) != 0U)
        || (__get_PRIMASK() != 0U) || (__get_BASEPRI() != 0U)
        || (__get_FAULTMASK() != 0U) || (rtos_port_get_critical_nesting() != 0U)
        || (rtos_sched_get_state() != RTOS_SCHED_RUNNING)
        || (rtos_kernel.current_tcb == NULL)) {
        return RTOS_ERR_SCHED;
    }
    return RTOS_OK;
}

static void capture_snapshot(flash_diag_snapshot_t *snapshot)
{
    RTOS_PORT_ENTER_CRITICAL();
    snapshot->current_tcb = rtos_kernel.current_tcb;
    snapshot->tick_count = rtos_sched_get_tick_count();
    snapshot->task_count = rtos_perf_task_registry_count();
    snapshot->heap_free_bytes = rtos_heap_get_free();
    snapshot->ready_bitmap = rtos_kernel.ready_bitmap;
    RTOS_PORT_EXIT_CRITICAL();
}

static uint32_t calculate_tick_delta(uint32_t begin_tick, uint32_t end_tick)
{
    if (end_tick >= begin_tick) {
        return end_tick - begin_tick;
    }
    /* 一次回调小于完整 Tick 周期，显式处理最多一次回绕，避免无符号减法下溢。 */
    return (UINT32_MAX - begin_tick) + 1U + end_tick;
}

static rtos_status_t set_instruction_cache(uint32_t target_acr)
{
    uint32_t saved_primask = __get_PRIMASK();
    uint32_t previous_acr;
    uint32_t disabled_acr;
    rtos_status_t status = RTOS_ERR_PARAM;

    __disable_irq();
    __DSB();
    previous_acr = FLASH->ACR;
    disabled_acr = previous_acr & ~(uint32_t)FLASH_ACR_ICEN;

    /* 在完整 IRQ 保护窗口内读改写，禁止改动等待周期和其他加速器配置。 */
    if (((FLASH->SR & FLASH_SR_BSY) == 0U)
        && ((previous_acr & (FLASH_ACR_ICRST | FLASH_ACR_DCRST)) == 0U)
        && (((previous_acr ^ target_acr) & ~(uint32_t)FLASH_ACR_ICEN) == 0U)) {
        FLASH->ACR = disabled_acr;
        if ((FLASH->ACR & FLASH_ACR_ICEN) == 0U) {
            FLASH->ACR = disabled_acr | FLASH_ACR_ICRST;
            FLASH->ACR = disabled_acr;
            FLASH->ACR = target_acr;
            __DSB();
            __ISB();
            if (FLASH->ACR == target_acr) {
                status = RTOS_OK;
            }
        }
        if (status != RTOS_OK) {
            FLASH->ACR = previous_acr;
            __DSB();
            __ISB();
        }
    }
    __set_PRIMASK(saved_primask);
    __ISB();
    return status;
}

static rtos_status_t report_mode(const char *mode_name)
{
    if (printf("FD|MODE|%s|acr=0x%08lx|icache=%lu|dcache=%lu|prefetch=%lu|latency=%lu\r\n",
               mode_name, (unsigned long)FLASH->ACR,
               (unsigned long)((FLASH->ACR & FLASH_ACR_ICEN) != 0U),
               (unsigned long)((FLASH->ACR & FLASH_ACR_DCEN) != 0U),
               (unsigned long)((FLASH->ACR & FLASH_ACR_PRFTEN) != 0U),
               (unsigned long)(FLASH->ACR & FLASH_ACR_LATENCY)) < 0) {
        return RTOS_ERR_PARAM;
    }
    return RTOS_OK;
}

static rtos_status_t report_snapshots(const char *mode_name,
                                    const flash_diag_snapshot_t *before,
                                    const flash_diag_snapshot_t *after)
{
    uint32_t tick_delta = calculate_tick_delta(before->tick_count, after->tick_count);

    if (printf("FD|STATE|%s|ticks_before=%lu|ticks_after=%lu|tick_delta=%lu"
               "|tasks_before=%lu|tasks_after=%lu|registry_valid=%lu"
               "|heap_before=%lu|heap_after=%lu|current_before=0x%08lx"
               "|current_after=0x%08lx|ready_before=0x%08lx|ready_after=0x%08lx\r\n",
               mode_name, (unsigned long)before->tick_count,
               (unsigned long)after->tick_count, (unsigned long)tick_delta,
               (unsigned long)before->task_count, (unsigned long)after->task_count,
               (unsigned long)(RTOS_CONFIG_USE_PERF_MONITOR != 0),
               (unsigned long)before->heap_free_bytes, (unsigned long)after->heap_free_bytes,
               (unsigned long)(uintptr_t)before->current_tcb,
               (unsigned long)(uintptr_t)after->current_tcb,
               (unsigned long)before->ready_bitmap, (unsigned long)after->ready_bitmap) < 0) {
        return RTOS_ERR_PARAM;
    }
    return RTOS_OK;
}

static rtos_status_t run_cache_mode(const char *mode_name, uint32_t target_acr,
                                  flash_diag_benchmark_t benchmark_callback,
                                  flash_diag_result_t *result)
{
    flash_diag_snapshot_t before;
    flash_diag_snapshot_t after;
    rtos_status_t status = set_instruction_cache(target_acr);

    if (status != RTOS_OK) {
        return status;
    }
    result->pass_count++;
    status = report_mode(mode_name);
    if (status != RTOS_OK) {
        return status;
    }
    status = check_benchmark_context();
    if (status != RTOS_OK) {
        return status;
    }
    capture_snapshot(&before);

    /* 不能把阻塞回调放在保护窗口中：PendSV 和 SysTick 必须能继续运行。 */
    benchmark_callback();

    status = check_benchmark_context();
    if (status != RTOS_OK) {
        return status;
    }
    capture_snapshot(&after);
    status = report_snapshots(mode_name, &before, &after);
    if (status != RTOS_OK) {
        return status;
    }
    if ((FLASH->ACR != target_acr) || (before.current_tcb != after.current_tcb)
        || (before.task_count != after.task_count)
        || (before.heap_free_bytes != after.heap_free_bytes)) {
        return RTOS_ERR_PARAM;
    }
    result->pass_count++;
    return RTOS_OK;
}

rtos_status_t flash_diag_run(flash_diag_benchmark_t benchmark_callback)
{
    static const char *const mode_names[] = {
        "ICACHE_ON_FIRST", "ICACHE_OFF", "ICACHE_ON_AGAIN"
    };
    flash_diag_result_t result = {0U, 0U};
    rtos_status_t status;
    rtos_status_t restore_status;
    uint32_t saved_acr;
    uint32_t target_acr;

    if (benchmark_callback == NULL) {
        return RTOS_ERR_NULL;
    }
    status = check_benchmark_context();
    if (status != RTOS_OK) {
        return status;
    }
    saved_acr = FLASH->ACR;
    for (uint32_t mode_index = 0U;
         mode_index < (uint32_t)(sizeof(mode_names) / sizeof(mode_names[0])); mode_index++) {
        target_acr = saved_acr | FLASH_ACR_ICEN;
        if (mode_index == 1U) {
            target_acr = saved_acr & ~(uint32_t)FLASH_ACR_ICEN;
        }
        status = run_cache_mode(mode_names[mode_index], target_acr, benchmark_callback, &result);
        if (status != RTOS_OK) {
            result.fail_count++;
            break;
        }
    }

    /* 单一收尾路径：某阶段失败也必须恢复和核验调用方原来的配置。 */
    restore_status = set_instruction_cache(saved_acr);
    if ((restore_status != RTOS_OK) || (FLASH->ACR != saved_acr)) {
        result.fail_count++;
        status = RTOS_ERR_PARAM;
    } else {
        result.pass_count++;
    }
    if (printf("FD|RESTORE|saved=0x%08lx|actual=0x%08lx|status=%ld\r\n",
               (unsigned long)saved_acr, (unsigned long)FLASH->ACR,
               (long)restore_status) < 0) {
        result.fail_count++;
        status = RTOS_ERR_PARAM;
    }
    if (result.fail_count != 0U) {
        if (printf("FD|FAIL|status=%ld\r\n", (long)status) < 0) {
            result.fail_count++;
            status = RTOS_ERR_PARAM;
        }
    }
    if (printf("FD|SUMMARY|pass=%lu fail=%lu|status=%ld\r\n",
               (unsigned long)result.pass_count, (unsigned long)result.fail_count,
               (long)status) < 0) {
        return RTOS_ERR_PARAM;
    }
    return status;
}
