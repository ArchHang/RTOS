/**
 * @file lowlevel_checks.h
 * @brief 上下文提交接口与实际 FPU 寄存器保留的独立 HIL 检查。
 *
 * 数组项是 IEEE-754 寄存器的原始 32 位模式，禁止用浮点运算代替逐位比较。
 * 控制任务初始化 expected；探针任务独占 observed；共享进度在临界区内访问。
 */
#ifndef F446_RTOS_HIL_FW_INC_LOWLEVEL_CHECKS_H_
#define F446_RTOS_HIL_FW_INC_LOWLEVEL_CHECKS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOWLEVEL_FPU_REGISTER_COUNT (16U)
#define LOWLEVEL_FPU_ROUNDS (10000U)

/** @brief Port/ARM 探针与 C 检查函数之间的布局约定。 */
typedef struct lowlevel_fpu_context {
    uint32_t expected[LOWLEVEL_FPU_REGISTER_COUNT];
    uint32_t observed[LOWLEVEL_FPU_REGISTER_COUNT];
    uint32_t rounds;
    uint32_t errors;
    uint32_t first_bad_register;
    uint32_t first_expected;
    uint32_t first_observed;
    uint32_t stop_requested;
    uint32_t done;
} lowlevel_fpu_context_t;

/**
 * @brief 在运行中的普通任务内执行检查，输出 LC 断言和最终汇总。
 * @details 调用任务必须高于最低优先级；本函数创建并删除三个静态测试任务。
 *          临时提交测试在同一临界区内恢复完整内核及涉及的 TCB 后才退出。
 *          不得与已布防的 RTOS_HIL_AUDIT_PENDSV 注入钩子同时执行。
 */
void lowlevel_checks_run(void);

/**
 * @brief ARM 专用 naked 任务入口，保持 S16-S31 的模式直到测试结束。
 * @param context [in,out] 已初始化的 lowlevel_fpu_context_t，存储须保持有效。
 * @details 任务不从入口返回；结束后由控制任务删除。
 */
void lowlevel_fpu_probe_task(void *context);

/**
 * @brief 从寄存器探针发起一次真实 yield 或 delay(1)。
 * @param context [in,out] 当前探针的进度状态。
 * @details 只在探针任务内调用；按 AAPCS 保留 S16-S31。
 */
void lowlevel_fpu_probe_schedule(lowlevel_fpu_context_t *context);

/**
 * @brief 比较已由 VSTM 保存的 16 项寄存器模式，并提交本轮进度。
 * @param context [in,out] observed 由当前探针任务独占。
 * @return 1 继续下一轮；0 已完成或控制任务请求停止。
 */
uint32_t lowlevel_fpu_probe_check_round(lowlevel_fpu_context_t *context);

/**
 * @brief 发布探针完成状态并永久停驻，等待控制任务删除。
 * @param context [in,out] 当前探针的状态与错误计数。
 * @details 此函数不正常返回，不释放调用者的 TCB、任务栈或 context。
 */
void lowlevel_fpu_probe_finish(lowlevel_fpu_context_t *context);

#ifdef __cplusplus
}
#endif

#endif /* F446_RTOS_HIL_FW_INC_LOWLEVEL_CHECKS_H_ */
