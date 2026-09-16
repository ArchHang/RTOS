/**
 * @file    rtos_sem.c
 * @brief   信号量实现
 *
 * @details 计数信号量基于等待链表实现。take 时:
 *            1. 计数 > 0: 计数减一，立即返回成功。
 *            2. 计数 = 0: 当前任务挂入等待链表并阻塞(可选超时)。
 *          give 时:
 *            1. 等待链表非空: 唤醒最高优先级任务(由它消耗这次 give)。
 *            2. 等待链表为空: 计数加一(不超过 max_count)。
 *          中断中调用 give 会唤醒等待任务，若该任务优先级更高则触发抢占。
 *          中断中调用 take 必须 timeout=RTOS_NO_WAIT(不阻塞)。
 *
 *          等待链表按优先级排序，保证高优先级任务先获得信号量。
 */
#include "rtos_sem.h"
#include "rtos_internal.h"
#include "rtos_sched.h"
#include "rtos_task.h" /* rtos_tcb_t 完整定义(访问 current_tcb->wait_result) */
#include <string.h>

rtos_status_t rtos_sem_init(rtos_sem_t *sem, uint32_t init_cnt, uint32_t max_cnt)
{
    if ((sem == NULL) || (max_cnt == 0U) || (init_cnt > max_cnt)) {
        return RTOS_ERR_PARAM;
    }
    memset(sem, 0, sizeof(*sem));
    sem->count = init_cnt;
    sem->max_count = max_cnt;
    sem->wait_head = NULL;
    sem->is_initialized = 1U;
    return RTOS_OK;
}

rtos_status_t rtos_sem_take(rtos_sem_t *sem, rtos_tick_t timeout)
{
    if (sem == NULL) {
        return RTOS_ERR_NULL;
    }
    if (sem->is_initialized == 0U) {
        return RTOS_ERR_PARAM;
    }

    rtos_bool_t in_isr = RTOS_PORT_IN_ISR();
    if (in_isr && (timeout != RTOS_NO_WAIT)) {
        return RTOS_ERR_ISR; /* 中断中不允许阻塞 */
    }

    RTOS_PORT_ENTER_CRITICAL();

    /* 有可用资源: 计数减一，直接返回 */
    if (sem->count > 0U) {
        sem->count--;
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_OK;
    }

    /* 无资源且不等待 */
    if (timeout == RTOS_NO_WAIT) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_TIMEOUT;
    }

    /* 中断中不应走到这里(已拦截) */
    /* 阻塞当前任务，挂入等待链表 */
    rtos_internal_block_current_on((void *)sem, &sem->wait_head, timeout);
    RTOS_PORT_EXIT_CRITICAL();

    /* 被唤醒后(此处当前任务已恢复运行)，检查唤醒结果 */
    rtos_tcb_t *cur = rtos_kernel.current_tcb;
    return cur->wait_result; /* RTOS_OK=被 give 唤醒, RTOS_ERR_TIMEOUT=超时 */
}

rtos_status_t rtos_sem_give(rtos_sem_t *sem)
{
    rtos_tcb_t *woken;

    if (sem == NULL) {
        return RTOS_ERR_NULL;
    }
    RTOS_ASSERT_ISR_OK(); /* [guard G-3] 违约优先级中断调用时立即捕获 */
    if (sem->is_initialized == 0U) {
        return RTOS_ERR_PARAM;
    }

    RTOS_PORT_ENTER_CRITICAL();

    /* 优先唤醒等待者(若有) */
    woken = rtos_internal_wake_highest(&sem->wait_head);
    if (woken != NULL) {
        /* 被唤醒任务消耗这次 give，计数不变 */
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_OK;
    }

    /* 无等待者: 计数加一(检查上限) */
    if (sem->count >= sem->max_count) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_OVERFLOW;
    }
    sem->count++;

    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

uint32_t rtos_sem_get_count(const rtos_sem_t *sem)
{
    if (sem == NULL) {
        return 0U;
    }
    return sem->count;
}

rtos_status_t rtos_sem_deinit(rtos_sem_t *sem)
{
    if (sem == NULL) {
        return RTOS_ERR_NULL;
    }
    RTOS_PORT_ENTER_CRITICAL();
    /* 唤醒所有等待者，告知对象已销毁 */
    rtos_internal_wake_all(&sem->wait_head, RTOS_ERR_DELETED);
    sem->is_initialized = 0U;
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}
