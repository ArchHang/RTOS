/**
 * @file lowlevel_checks.c
 * @brief 上下文提交的防御分支、真实 FPU 寄存器与整数任务混合回归。
 *
 * 临时内核状态仅存在于完整临界区内。三个测试 TCB 均由公开接口注册；
 * 提交函数不安装 PSP，也不请求 PendSV，测试结束前恢复全部内核和相关 TCB。
 * 两个 FPU 任务各检查 10000 轮，整数任务也运行 10000 轮，全部由控制任务回收。
 */
#include "lowlevel_checks.h"

#include <stdio.h>
#include <string.h>

#include "rtos.h"
#include "rtos_internal.h"

enum {
    LOWLEVEL_TASK_COUNT = 3U,
    LOWLEVEL_INTEGER_SLOT = 2U,
    LOWLEVEL_STACK_WORDS = 256U,
    LOWLEVEL_WAIT_TICKS = 3000U,
    LOWLEVEL_DELAY_MASK = 63U
};

typedef struct lowlevel_integer_state {
    uint32_t rounds;
    uint32_t value;
    uint32_t stop_requested;
    uint32_t done;
    uint32_t errors;
} lowlevel_integer_state_t;

static rtos_tcb_t s_tasks[LOWLEVEL_TASK_COUNT];
static rtos_stack_t s_stacks[LOWLEVEL_TASK_COUNT][LOWLEVEL_STACK_WORDS]
    __attribute__((aligned(8)));
static uint32_t s_created[LOWLEVEL_TASK_COUNT];
static lowlevel_fpu_context_t s_fpu[2];
static lowlevel_integer_state_t s_integer;
static rtos_kernel_t s_saved_kernel;
static rtos_tcb_t s_saved_current;
static rtos_tcb_t s_saved_peer;
static uint32_t s_pass;
static uint32_t s_fail;

static void lowlevel_record(const char *name, uint32_t condition)
{
    if (condition != 0U) {
        s_pass++;
    } else {
        s_fail++;
    }
    printf("LC|%s|%s\r\n", name, condition != 0U ? "PASS" : "FAIL");
}

static void lowlevel_snapshot_enter(rtos_tcb_t *current, rtos_tcb_t *peer)
{
    RTOS_PORT_ENTER_CRITICAL();
    s_saved_kernel = rtos_kernel;
    s_saved_current = *current;
    s_saved_peer = *peer;
}

static void lowlevel_snapshot_restore(rtos_tcb_t *current, rtos_tcb_t *peer)
{
    /* 恢复所有统计字段，兼容启用 HOTPATH_STATS 时的提交钩子。 */
    *current = s_saved_current;
    *peer = s_saved_peer;
    rtos_kernel = s_saved_kernel;
    RTOS_PORT_EXIT_CRITICAL();
}

static void lowlevel_no_switch(const char *name, rtos_sched_state_t state,
                              uint32_t has_current, rtos_tcb_t *current,
                              rtos_tcb_t *peer)
{
    lowlevel_snapshot_enter(current, peer);
    rtos_tcb_t *expected = has_current != 0U ? current : NULL;
    rtos_kernel.sched_state = state;
    rtos_kernel.current_tcb = expected;
    rtos_kernel.next_tcb = state == RTOS_SCHED_RUNNING ? NULL : peer;
    rtos_tcb_t *result = rtos_sched_commit_context_switch();
    uint32_t correct = (result == expected) &&
        (rtos_kernel.current_tcb == expected) && (rtos_kernel.next_tcb == NULL) &&
        (current->state == s_saved_current.state) &&
        (peer->state == s_saved_peer.state) &&
        (current->switch_count == s_saved_current.switch_count) &&
        (peer->switch_count == s_saved_peer.switch_count);
    lowlevel_snapshot_restore(current, peer);
    lowlevel_record(name, correct);
}

static void lowlevel_commit_peer(const char *name, uint32_t has_current,
                                rtos_task_state_t previous_state, uint32_t public_entry,
                                rtos_tcb_t *current, rtos_tcb_t *peer)
{
    lowlevel_snapshot_enter(current, peer);
    current->state = previous_state;
    peer->state = RTOS_TASK_READY;
    rtos_kernel.sched_state = RTOS_SCHED_RUNNING;
    rtos_kernel.current_tcb = has_current != 0U ? current : NULL;
    rtos_kernel.next_tcb = peer;
    rtos_tcb_t *result = NULL;
    if (public_entry != 0U) {
        void (*entry)(void) = rtos_sched_context_switch;
        entry();
    } else {
        result = rtos_sched_commit_context_switch();
    }
    rtos_task_state_t expected_previous = previous_state;
    if ((has_current != 0U) && (previous_state == RTOS_TASK_RUNNING)) {
        expected_previous = RTOS_TASK_READY;
    }
    uint32_t correct = ((public_entry != 0U) || (result == peer)) &&
        (rtos_kernel.current_tcb == peer) && (rtos_kernel.next_tcb == NULL) &&
        (current->state == expected_previous) && (peer->state == RTOS_TASK_RUNNING) &&
        (peer->switch_count == s_saved_peer.switch_count + 1U) &&
        (current->switch_count == s_saved_current.switch_count) &&
        (rtos_kernel.switch_count == s_saved_kernel.switch_count);
    lowlevel_snapshot_restore(current, peer);
    lowlevel_record(name, correct);
}

static void lowlevel_commit_self(rtos_tcb_t *current, rtos_tcb_t *peer)
{
    lowlevel_snapshot_enter(current, peer);
    rtos_kernel.sched_state = RTOS_SCHED_RUNNING;
    rtos_kernel.current_tcb = current;
    rtos_kernel.next_tcb = current;
    current->state = RTOS_TASK_RUNNING;
    rtos_tcb_t *result = rtos_sched_commit_context_switch();
    uint32_t correct = (result == current) && (rtos_kernel.current_tcb == current) &&
        (rtos_kernel.next_tcb == NULL) && (current->state == RTOS_TASK_RUNNING) &&
        (current->switch_count == s_saved_current.switch_count + 1U) &&
        (peer->state == s_saved_peer.state) &&
        (peer->switch_count == s_saved_peer.switch_count) &&
        (rtos_kernel.switch_count == s_saved_kernel.switch_count);
    lowlevel_snapshot_restore(current, peer);
    lowlevel_record("commit_self_keeps_original_count", correct);
}

static void lowlevel_commit_checks(rtos_tcb_t *current, rtos_tcb_t *peer)
{
    lowlevel_no_switch("commit_prestart_null", RTOS_SCHED_NOT_STARTED, 0U, current, peer);
    lowlevel_no_switch("commit_prestart_current", RTOS_SCHED_NOT_STARTED, 1U, current, peer);
    lowlevel_no_switch("commit_suspended_current", RTOS_SCHED_SUSPENDED, 1U, current, peer);
    lowlevel_no_switch("commit_stopped_current", RTOS_SCHED_STOPPED, 1U, current, peer);
    lowlevel_no_switch("commit_running_no_next", RTOS_SCHED_RUNNING, 1U, current, peer);
    lowlevel_commit_self(current, peer);
    lowlevel_commit_peer("commit_ready_peer", 1U, RTOS_TASK_RUNNING, 0U, current, peer);
    lowlevel_commit_peer("commit_nonrunning_previous", 1U, RTOS_TASK_BLOCKED, 0U,
                        current, peer);
    lowlevel_commit_peer("commit_null_previous", 0U, RTOS_TASK_RUNNING, 0U, current, peer);
    lowlevel_commit_peer("commit_public_void_abi", 1U, RTOS_TASK_RUNNING, 1U, current, peer);
}

static void lowlevel_park(uint32_t *errors)
{
    for (;;) {
        rtos_status_t status = rtos_task_suspend(NULL);
        if (status != RTOS_OK) {
            RTOS_PORT_ENTER_CRITICAL();
            (*errors)++;
            RTOS_PORT_EXIT_CRITICAL();
        }
        /* 外部恢复或挂起失败时仍让出 CPU，不使控制任务超时失效。 */
        rtos_task_delay(1U);
    }
}

void lowlevel_fpu_probe_schedule(lowlevel_fpu_context_t *context)
{
    RTOS_PORT_ENTER_CRITICAL();
    uint32_t round = context->rounds;
    RTOS_PORT_EXIT_CRITICAL();
    if ((round & LOWLEVEL_DELAY_MASK) == 0U) {
        rtos_task_delay(1U);
    } else {
        rtos_task_yield();
    }
}

uint32_t lowlevel_fpu_probe_check_round(lowlevel_fpu_context_t *context)
{
    uint32_t bad_count = 0U;
    uint32_t bad_register = 0U;
    uint32_t bad_expected = 0U;
    uint32_t bad_observed = 0U;
    for (uint32_t index = 0U; index < LOWLEVEL_FPU_REGISTER_COUNT; index++) {
        if (context->observed[index] != context->expected[index]) {
            if (bad_count == 0U) {
                bad_register = index + 16U;
                bad_expected = context->expected[index];
                bad_observed = context->observed[index];
            }
            bad_count++;
        }
    }
    RTOS_PORT_ENTER_CRITICAL();
    if ((context->errors == 0U) && (bad_count != 0U)) {
        context->first_bad_register = bad_register;
        context->first_expected = bad_expected;
        context->first_observed = bad_observed;
    }
    context->errors += bad_count;
    context->rounds++;
    uint32_t again = (context->rounds < LOWLEVEL_FPU_ROUNDS) &&
        (context->stop_requested == 0U);
    RTOS_PORT_EXIT_CRITICAL();
    return again;
}

void lowlevel_fpu_probe_finish(lowlevel_fpu_context_t *context)
{
    RTOS_PORT_ENTER_CRITICAL();
    context->done = 1U;
    RTOS_PORT_EXIT_CRITICAL();
    lowlevel_park(&context->errors);
}

static void lowlevel_integer_task(void *context)
{
    lowlevel_integer_state_t *state = (lowlevel_integer_state_t *)context;
    uint32_t value = 0x12345678U;
    for (uint32_t round = 0U; round < LOWLEVEL_FPU_ROUNDS; round++) {
        RTOS_PORT_ENTER_CRITICAL();
        uint32_t stop_requested = state->stop_requested;
        RTOS_PORT_EXIT_CRITICAL();
        if (stop_requested != 0U) {
            break;
        }
        value = ((value << 3U) | (value >> 29U)) ^ 0x9E3779B9U;
        if ((round & LOWLEVEL_DELAY_MASK) == 0U) {
            rtos_task_delay(1U);
        } else {
            rtos_task_yield();
        }
        RTOS_PORT_ENTER_CRITICAL();
        state->rounds = round + 1U;
        state->value = value;
        RTOS_PORT_EXIT_CRITICAL();
    }
    RTOS_PORT_ENTER_CRITICAL();
    state->done = 1U;
    RTOS_PORT_EXIT_CRITICAL();
    lowlevel_park(&state->errors);
}

static uint32_t lowlevel_wait_done(void)
{
    /* 每轮真正 delay(1)，有限次等待不依赖子任务主动报告错误。 */
    for (uint32_t tick = 0U; tick < LOWLEVEL_WAIT_TICKS; tick++) {
        RTOS_PORT_ENTER_CRITICAL();
        uint32_t done = (s_fpu[0].done != 0U) && (s_fpu[1].done != 0U) &&
            (s_integer.done != 0U) &&
            (s_tasks[0].state == RTOS_TASK_SUSPENDED) &&
            (s_tasks[1].state == RTOS_TASK_SUSPENDED) &&
            (s_tasks[LOWLEVEL_INTEGER_SLOT].state == RTOS_TASK_SUSPENDED);
        RTOS_PORT_EXIT_CRITICAL();
        if (done != 0U) {
            return 1U;
        }
        rtos_task_delay(1U);
    }
    return 0U;
}

static void lowlevel_cleanup(void)
{
    static const char *const names[LOWLEVEL_TASK_COUNT] = {
        "cleanup_fpu_a", "cleanup_fpu_b", "cleanup_integer_peer"
    };
    RTOS_PORT_ENTER_CRITICAL();
    s_fpu[0].stop_requested = 1U;
    s_fpu[1].stop_requested = 1U;
    s_integer.stop_requested = 1U;
    RTOS_PORT_EXIT_CRITICAL();
    /* 控制任务优先级更高；删除 READY/DELAYED/SUSPENDED 的静态任务均合法。
     * 子任务的 context 和栈为静态存储，不会在 PendSV 尚有引用时被释放。 */
    for (uint32_t slot = 0U; slot < LOWLEVEL_TASK_COUNT; slot++) {
        if (s_created[slot] != 0U) {
            rtos_status_t status = rtos_task_delete(&s_tasks[slot]);
            lowlevel_record(names[slot], status == RTOS_OK);
            if (status == RTOS_OK) {
                s_created[slot] = 0U;
            }
        }
    }
}

static void lowlevel_fpu_results(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    uint32_t first_rounds = s_fpu[0].rounds;
    uint32_t second_rounds = s_fpu[1].rounds;
    uint32_t first_errors = s_fpu[0].errors;
    uint32_t second_errors = s_fpu[1].errors;
    uint32_t integer_rounds = s_integer.rounds;
    uint32_t integer_value = s_integer.value;
    uint32_t integer_errors = s_integer.errors;
    RTOS_PORT_EXIT_CRITICAL();
    lowlevel_record("fpu_a_10000_rounds", first_rounds == LOWLEVEL_FPU_ROUNDS);
    lowlevel_record("fpu_b_10000_rounds", second_rounds == LOWLEVEL_FPU_ROUNDS);
    lowlevel_record("fpu_a_s16_s31_exact_bits", first_errors == 0U);
    lowlevel_record("fpu_b_s16_s31_exact_bits", second_errors == 0U);
    lowlevel_record("integer_peer_10000_rounds", integer_rounds == LOWLEVEL_FPU_ROUNDS);
    /* 10000 次明确的无符号 ROTL3/XOR 的离线整数参考结果。 */
    lowlevel_record("integer_peer_exact_result",
                    (integer_value == 0x24DD6091U) && (integer_errors == 0U));
    printf("LC|DETAIL|rounds_a=%lu rounds_b=%lu integer=%lu errors_a=%lu errors_b=%lu\r\n",
           (unsigned long)first_rounds, (unsigned long)second_rounds,
           (unsigned long)integer_rounds, (unsigned long)first_errors,
           (unsigned long)second_errors);
    if ((first_errors != 0U) || (second_errors != 0U)) {
        RTOS_PORT_ENTER_CRITICAL();
        uint32_t bad_register = first_errors != 0U ?
            s_fpu[0].first_bad_register : s_fpu[1].first_bad_register;
        uint32_t expected = first_errors != 0U ?
            s_fpu[0].first_expected : s_fpu[1].first_expected;
        uint32_t observed = first_errors != 0U ?
            s_fpu[0].first_observed : s_fpu[1].first_observed;
        RTOS_PORT_EXIT_CRITICAL();
        printf("LC|DETAIL|register=s%lu expected=%08lx observed=%08lx\r\n",
               (unsigned long)bad_register, (unsigned long)expected,
               (unsigned long)observed);
    }
}

void lowlevel_checks_run(void)
{
    static const char *const names[LOWLEVEL_TASK_COUNT] = {
        "setup_fpu_a", "setup_fpu_b", "setup_integer_peer"
    };
    s_pass = 0U;
    s_fail = 0U;
    rtos_tcb_t *current = rtos_task_get_current();
    RTOS_PORT_ENTER_CRITICAL();
    uint32_t allowed = (current != NULL) &&
        (rtos_kernel.sched_state == RTOS_SCHED_RUNNING) &&
        (RTOS_PORT_IN_ISR() == RTOS_FALSE) &&
        (current->priority < RTOS_CONFIG_MAX_PRIORITIES - 1U) &&
        (s_created[0] == 0U) && (s_created[1] == 0U) &&
        (s_created[LOWLEVEL_INTEGER_SLOT] == 0U);
    rtos_prio_t priority = allowed != 0U ? current->priority + 1U : 0U;
    RTOS_PORT_EXIT_CRITICAL();
    lowlevel_record("controller_context", allowed);
    if (allowed == 0U) {
        printf("LC|SUMMARY|pass=%lu fail=%lu\r\n",
               (unsigned long)s_pass, (unsigned long)s_fail);
        return;
    }
    memset(s_fpu, 0, sizeof(s_fpu));
    memset(&s_integer, 0, sizeof(s_integer));
    for (uint32_t index = 0U; index < LOWLEVEL_FPU_REGISTER_COUNT; index++) {
        /* 不同的有限正规数位模式；每个任务、每个寄存器都可区分。 */
        s_fpu[0].expected[index] = 0x3F800100U + (index << 12U);
        s_fpu[1].expected[index] = 0x40080200U + (index << 12U);
    }
    for (uint32_t slot = 0U; slot < LOWLEVEL_TASK_COUNT; slot++) {
        rtos_task_func_t entry = slot == LOWLEVEL_INTEGER_SLOT ?
            lowlevel_integer_task : lowlevel_fpu_probe_task;
        void *context = slot == LOWLEVEL_INTEGER_SLOT ?
            (void *)&s_integer : (void *)&s_fpu[slot];
        rtos_status_t status = rtos_task_create_static(&s_tasks[slot], s_stacks[slot],
            LOWLEVEL_STACK_WORDS, entry, context, priority, "lowlevel-check");
        s_created[slot] = status == RTOS_OK ? 1U : 0U;
        lowlevel_record(names[slot], status == RTOS_OK);
    }
    if ((s_created[0] != 0U) && (s_created[1] != 0U) &&
        (s_created[LOWLEVEL_INTEGER_SLOT] != 0U)) {
        lowlevel_commit_checks(current, &s_tasks[LOWLEVEL_INTEGER_SLOT]);
        uint32_t done = lowlevel_wait_done();
        lowlevel_record("mixed_contexts_finish_before_timeout", done);
        lowlevel_fpu_results();
    }
    lowlevel_cleanup();
    printf("LC|SUMMARY|pass=%lu fail=%lu\r\n",
           (unsigned long)s_pass, (unsigned long)s_fail);
}
