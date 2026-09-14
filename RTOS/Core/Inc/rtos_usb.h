/**
 * @file    rtos_usb.h
 * @brief   USB 子系统接口(含 CherryUSB OSAL 适配层 + CDC ACM 用户封装)
 *
 * @details 本模块为 RTOS 提供 USB 子系统, 设计目标:
 *            1. 可裁剪: RTOS_CONFIG_USE_USB=0 时全部代码编译为空, 零开销。
 *            2. 不侵入内核: 纯适配层, 不修改调度器/任务/IPC 等内核源码,
 *               复用现有信号量/互斥锁/队列/定时器/任务原语, 与其他模块和谐共存。
 *            3. 自包含: 内置 CherryUSB 协议栈(位于 lib/third/CherryUSB/),
 *               通过 rtos_usb_osal.c 提供 usb_osal_* 兼容实现, 用户无需另行
 *               下载或移植 OSAL。
 *            4. 简单易用: 提供 rtos_usb_cdc_* 系列函数, 一行初始化即可使用
 *               USB 虚拟串口(CDC ACM), 内部自动处理描述符/端点/回调/ZLP。
 *
 *          架构:
 *
 *            +-------------------------------------------+
 *            |        用户应用 (rtos_usb_cdc_*)          |
 *            +-------------------------------------------+
 *            |  CDC ACM 封装 (rtos_usb.c 内置)           |
 *            +-------------------------------------------+
 *            |  CherryUSB core/class (lib/third/...)     |
 *            +-------------------------------------------+
 *            |  rtos_usb_osal.c (usb_osal_* 兼容实现)    |  <-- OSAL 适配层
 *            +-------------------------------------------+
 *            |  rtos_usb.c (对象池/堆/ISR/CDC 封装)      |  <-- RTOS USB 引擎
 *            +-------------------------------------------+
 *            |  rtos_sem/mutex/queue/timer/task (内核)   |
 *            +-------------------------------------------+
 *            |  用户 USB IP 驱动 (CherryUSB port/<chip>) |
 *            +-------------------------------------------+
 *
 *          类型衔接策略:
 *            CherryUSB 的 usb_osal.h 将 usb_osal_sem_t / usb_osal_mutex_t 等
 *            定义为 void *。本 RTOS 在内部用 rtos_usb_sem_t / rtos_usb_mutex_t
 *            等结构体管理 OSAL 对象(静态池), rtos_usb_osal.c 中的 usb_osal_*
 *            函数在 void* 与内部结构体指针间做转换。因此本头文件不重定义
 *            usb_osal_*_t 类型, 避免与 CherryUSB usb_osal.h 冲突。
 *
 *          对象管理模式:
 *            CherryUSB 的 osal 采用"create 返回句柄"模式, 而本 RTOS 的 IPC
 *            对象是静态的(用户传入对象指针)。为弥合差异, rtos_usb.c 内部维护
 *            静态对象池(sem/mutex/mq/thread/timer 各一个数组), create 时从池中
 *            取一个空闲对象并初始化, delete 时归还。池容量由 rtos_config.h 配置。
 *            线程对象内嵌固定大小栈数组(RTOS_CONFIG_USB_THREAD_STACK_SIZE 字),
 *            避免动态分配栈。
 *
 *          内存分配:
 *            rtos_usb_malloc/free 提供 USB 协议栈所需的动态内存(URB/描述符/
 *            class 结构), 内部用双档固定块池(<=64B 小块, <=512B 大块)实现,
 *            O(1) 分配释放, 无碎片, 全程临界区保护。
 *
 *          CDC ACM 用户封装:
 *            rtos_usb_cdc_init() 一行调用完成: 描述符注册 → 接口初始化 →
 *            端点添加 → usbd_initialize。内部维护 RX 环形缓冲与 TX 完成信号量,
 *            将 CherryUSB 的异步端点传输转换为 rtos_usb_cdc_read/write 的
 *            阻塞式 API, 大幅降低使用门槛。
 *
 *          中断接入(重要! 必须遵守优先级约束):
 *            USB ISR 优先级数值必须 >= RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY
 *            (即处于"syscall 优先级域"), 这样内核临界区(BASEPRI)才能屏蔽它。
 *            原因: USB ISR 内部调用 rtos_sem_give 等内核 API, 会修改就绪表/
 *            等待链表等调度器数据结构; 若 ISR 未被临界区屏蔽, 在任务正操作
 *            调度器时打断将导致数据结构损坏。这与 FreeRTOS 的
 *            configMAX_SYSCALL_INTERRUPT_PRIORITY 约束完全一致。
 *            注: 临界区持续时间极短(微秒级), 不会"丢失"USB 中断 —— 硬件会
 *            将其标记为 pending, 临界区退出后立即触发。
 *            用户在工程里:
 *              1. STM32CubeMX 配置 USB 中断优先级为 5~15(数值大=低优先级,
 *                 处于 syscall 域, 可被临界区屏蔽)。切勿设为 0~4!
 *              2. 调用 rtos_usb_register_isr(IRQn, handler, arg) 注册处理函数
 *              3. 在 USB IRQHandler 中调用 rtos_usb_irq_dispatch(IRQn)
 *            或使用 CDC 封装时直接在 IRQHandler 中调用 rtos_usb_cdc_isr(busid)。
 *
 *          安全性:
 *            - 所有 create 返回 NULL 表示池满(不崩溃, 调用方应检查)
 *            - delete 接受 NULL(空操作)
 *            - malloc 线程安全(临界区), free 校验指针范围
 *            - 线程通过 rtos_task_create 创建, 自动接入 rtos_perf 性能监视
 */
#ifndef RTOS_USB_H_
#define RTOS_USB_H_

#include "rtos_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== OSAL 等待超时常量(CherryUSB 兼容) ============================== */
/** @brief CherryUSB osal 期望的永久等待值。与 usb_osal.h 中定义一致, 此处提前定义
 *         供 rtos_usb_osal.c 在包含 usb_osal.h 前使用; 用 #ifndef 防止重定义警告。 */
#ifndef USB_OSAL_WAITING_FOREVER
#define USB_OSAL_WAITING_FOREVER (0xFFFFFFFFU)
#endif

#if RTOS_CONFIG_USE_USB

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "rtos_types.h"
#include "rtos_port.h"
#include "rtos_sem.h"
#include "rtos_mutex.h"
#include "rtos_queue.h"
#include "rtos_timer.h"
#include "rtos_task.h"

/* ============================== OSAL 内部对象(句柄式封装静态 RTOS 对象) ============================== */

/**
 * @brief OSAL 信号量句柄对象(内部使用)。
 * @details 包装一个 rtos_sem_t 计数信号量 + 池使用标志。
 *          CherryUSB 的 usb_osal_sem_create 返回本对象指针(经 void* 转换)。
 */
typedef struct rtos_usb_sem {
    rtos_sem_t sem; /**< 底层计数信号量。 */
    uint8_t in_use; /**< 池使用标志(0=空闲, 1=已分配)。 */
} rtos_usb_sem_t;

/** @brief OSAL 互斥锁句柄对象(带优先级继承)。 */
typedef struct rtos_usb_mutex {
    rtos_mutex_t mutex; /**< 底层互斥锁(带优先级继承)。 */
    uint8_t in_use;
} rtos_usb_mutex_t;

/**
 * @brief OSAL 消息队列句柄对象。
 * @details 内嵌固定深度(RTOS_CONFIG_USB_MQ_DEPTH)的 uintptr_t 队列存储,
 *          避免 malloc。CherryUSB mq 仅传递事件值/指针, uintptr_t 足够。 */
typedef struct rtos_usb_mq {
    rtos_queue_t queue; /**< 底层消息队列。 */
    uintptr_t storage[RTOS_CONFIG_USB_MQ_DEPTH]; /**< 内嵌存储区。 */
    uint8_t in_use;
} rtos_usb_mq_t;

/**
 * @brief OSAL 线程句柄对象。
 * @details 内嵌 TCB + 固定大小栈数组。栈按 8 字节对齐(Cortex-M EABI 要求)。
 *          create 时若调用方请求栈大于内嵌栈, 自动截断为内嵌栈大小。 */
typedef struct rtos_usb_thread {
    rtos_tcb_t tcb; /**< 任务控制块。 */
    rtos_stack_t stack[RTOS_CONFIG_USB_THREAD_STACK_SIZE]
        __attribute__((aligned(RTOS_CONFIG_STACK_ALIGNMENT))); /**< 内嵌栈。 */
    uint8_t in_use;
} rtos_usb_thread_t;

/** @brief OSAL 定时器句柄对象(内部使用, usb_osal_timer_create 返回的 struct
 *         usb_osal_timer 中的 ->timer 字段指向本对象)。 */
typedef struct rtos_usb_timer {
    rtos_timer_t timer; /**< 底层软件定时器。 */
    uint8_t in_use;
} rtos_usb_timer_t;

/* ============================== OSAL 句柄式封装(供 rtos_usb_osal.c 调用) ============================== */
/* 以下函数实现"create 返回句柄 / delete 归还"模式, 内部管理静态对象池。
 * rtos_usb_osal.c 中的 usb_osal_* 函数通过 void* 与这些内部类型互转。 */

/** @brief 创建信号量。返回句柄, NULL=池满。 */
rtos_usb_sem_t *rtos_usb_sem_create(uint32_t init_count, uint32_t max_count);
/** @brief 删除信号量(归还池)。接受 NULL。 */
void rtos_usb_sem_delete(rtos_usb_sem_t *sem);
/** @brief 获取信号量。timeout 单位 Tick。 */
rtos_status_t rtos_usb_sem_take(rtos_usb_sem_t *sem, rtos_tick_t timeout);
/** @brief 释放信号量(可在 ISR 调用)。 */
rtos_status_t rtos_usb_sem_give(rtos_usb_sem_t *sem);
/** @brief 重置信号量计数为 0。 */
void rtos_usb_sem_reset(rtos_usb_sem_t *sem);

/** @brief 创建互斥锁。 */
rtos_usb_mutex_t *rtos_usb_mutex_create(void);
/** @brief 删除互斥锁。 */
void rtos_usb_mutex_delete(rtos_usb_mutex_t *mutex);
/** @brief 获取互斥锁(永久等待, 不可在 ISR)。 */
rtos_status_t rtos_usb_mutex_take(rtos_usb_mutex_t *mutex);
/** @brief 释放互斥锁。 */
rtos_status_t rtos_usb_mutex_give(rtos_usb_mutex_t *mutex);

/**
 * @brief 创建消息队列。
 * @param max_msgs 容量(受内嵌 storage 限制, 不超过 RTOS_CONFIG_USB_MQ_DEPTH)。
 * @return 句柄, NULL=池满或参数过大。
 */
rtos_usb_mq_t *rtos_usb_mq_create(uint32_t max_msgs);
/** @brief 删除消息队列。 */
void rtos_usb_mq_delete(rtos_usb_mq_t *mq);
/** @brief 发送事件值(队尾)。ISR 中 timeout 须为 RTOS_NO_WAIT。 */
rtos_status_t rtos_usb_mq_send(rtos_usb_mq_t *mq, uintptr_t addr, rtos_tick_t timeout);
/** @brief 接收事件值。timeout 单位 Tick。 */
rtos_status_t rtos_usb_mq_recv(rtos_usb_mq_t *mq, uintptr_t *addr, rtos_tick_t timeout);

/**
 * @brief 创建线程。
 * @param name       线程名。
 * @param stack_size 栈大小(字数)。若超过 RTOS_CONFIG_USB_THREAD_STACK_SIZE, 截断。
 * @param prio       优先级(0=最高, 与本 RTOS 一致; CherryUSB 也是小值=高优先级)。
 * @param entry      入口函数。
 * @param arg        入口参数。
 * @return 句柄, NULL=池满。
 * @note  线程通过 rtos_task_create 创建, 自动接入 rtos_perf 性能监视与栈高水位。
 */
rtos_usb_thread_t *rtos_usb_thread_create(const char *name, uint32_t stack_size, rtos_prio_t prio,
                                          rtos_task_func_t entry, void *arg);
/** @brief 删除线程。thread=NULL 表示删除当前线程。 */
void rtos_usb_thread_delete(rtos_usb_thread_t *thread);
/** @brief 暂时降低当前线程优先级以让其他线程运行(CherryUSB usb_osal_thread_schedule_other)。 */
void rtos_usb_thread_schedule_other(void);

/**
 * @brief 创建定时器(内部用)。
 * @param name       名字。
 * @param ms         周期(毫秒), 内部转 Tick。
 * @param cb         回调(运行于定时器服务任务上下文, 可调用阻塞 API)。
 * @param arg        回调参数。
 * @param is_periodic true=周期, false=单次。
 * @return 句柄, NULL=池满。
 */
rtos_usb_timer_t *rtos_usb_timer_create(const char *name, uint32_t ms, rtos_timer_cb_t cb,
                                        void *arg, bool is_periodic);
/** @brief 删除定时器。 */
void rtos_usb_timer_delete(rtos_usb_timer_t *timer);
/** @brief 启动定时器。 */
void rtos_usb_timer_start(rtos_usb_timer_t *timer);
/** @brief 停止定时器。 */
void rtos_usb_timer_stop(rtos_usb_timer_t *timer);

/* ============================== USB 子系统公共 API ============================== */

/**
 * @brief 初始化 USB 子系统。
 * @return RTOS_OK 或错误码。
 * @note  在 rtos_init() 之后、rtos_start() 之前调用一次。
 *        内部清零对象池与 ISR 表, 不创建任务(任务由 USB 协议栈 create)。
 */
rtos_status_t rtos_usb_init(void);

/** @brief USB ISR 处理函数类型。 */
typedef void (*rtos_usb_isr_handler_t)(void *arg);

/**
 * @brief 注册 USB 中断处理函数。
 * @param irqn    中断号(用户自定义标识, 通常用 CMSIS IRQn 编号)。
 * @param handler 中断处理函数(由 USB 协议栈提供, 如 USBD_IRQHandler)。
 * @param arg     传给处理函数的参数(通常为 busid)。
 * @return RTOS_OK / RTOS_ERR_NULL / RTOS_ERR_NO_MEM(表满)。
 * @note  本函数只建立 irqn -> handler 映射, 不操作 NVIC。用户需在工程里
 *        (如 STM32 HAL)使能 USB 中断并设置优先级为 syscall 域(数值 >=
 *        RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY, STM32 4 位优先级建议 5~15),
 *        以便内核临界区能屏蔽它, 保证调度器数据结构安全。切勿设为 0~4!
 */
rtos_status_t rtos_usb_register_isr(uint32_t irqn, rtos_usb_isr_handler_t handler, void *arg);

/**
 * @brief USB 中断分发。
 * @param irqn 中断号(与注册时一致)。
 * @details 在用户 USB IRQHandler 中调用, 查表执行注册的处理函数。
 *          典型用法:
 *            void OTG_FS_IRQHandler(void) { rtos_usb_irq_dispatch(OTG_FS_IRQn); }
 *          若未注册对应 irqn, 本函数空操作(安全)。
 */
void rtos_usb_irq_dispatch(uint32_t irqn);

/* ============================== USB 堆(动态内存) ============================== */

/**
 * @brief USB 堆分配(线程安全, ISR 安全)。
 * @param size 请求字节数。
 * @return 指针, NULL=堆满或 size 过大(>512B)。
 * @note  内部双档固定块池: size<=64B 走小块池, size<=512B 走大块池, 否则失败。
 */
void *rtos_usb_malloc(size_t size);

/**
 * @brief USB 堆释放。
 * @param ptr 之前由 rtos_usb_malloc 返回的指针。接受 NULL。
 */
void rtos_usb_free(void *ptr);

/**
 * @brief 毫秒延时(任务上下文)。
 * @note  不可在 ISR 调用(内部用 rtos_task_delay)。
 */
void rtos_usb_msleep(uint32_t ms);

/* ============================== 用户友好的 CDC ACM 封装 ============================== */
/* 以下 API 在内部自动处理 CherryUSB 的描述符注册、接口/端点初始化、
 * 异步传输回调、ZLP(零长度包)处理、RX 环形缓冲, 将复杂的 USB CDC ACM
 * 使用简化为类似串口的 read/write 接口。
 *
 * 典型用法:
 *
 *   int main(void) {
 *       rtos_init();
 *       rtos_usb_init();
 *       rtos_usb_cdc_init(0, USB_BASE_ADDR);   // 一行初始化虚拟串口
 *       rtos_start();
 *   }
 *
 *   // 中断接入(二选一, 切勿同时使用!):
 *   // 方式A: 若编译了 CherryUSB 的 ST glue 文件(usb_glue_st.c), 它已提供
 *   //        OTG_FS_IRQHandler / USB_IRQHandler 等 ISR 入口, 无需自己定义,
 *   //        只需在 CubeMX 里使能 USB 中断并设置优先级(5~15)。
 *   // 方式B: 若不使用 ST glue(或使用其他 port), 自行定义 ISR 入口:
 *   void OTG_FS_IRQHandler(void) {
 *       rtos_usb_cdc_isr(0);   // 转发给 CherryUSB 的 USBD_IRQHandler
 *   }
 *
 *   // 任意任务中读写:
 *   uint8_t buf[64];
 *   int n = rtos_usb_cdc_read(0, buf, sizeof(buf), 1000);  // 1s 超时
 *   if (n > 0) {
 *       rtos_usb_cdc_write(0, buf, n, 1000);  // 回发
 *   }
 */

/**
 * @brief 初始化 CDC ACM 虚拟串口(一行调用)。
 * @param busid    USB 总线 ID(单 USB IP 通常为 0)。
 * @param reg_base USB 控制器寄存器基址(如 (uintptr_t)USB_OTG_FS)。
 * @return RTOS_OK 或错误码。
 * @note  必须在 rtos_usb_init() 之后、rtos_start() 之前调用。
 *        内部完成: 描述符注册 → CDC ACM 接口初始化 → 端点添加 → usbd_initialize。
 *        初始化后 USB 中断可能立即到来, 请确保已在工程中配置好 USB NVIC。
 */
rtos_status_t rtos_usb_cdc_init(uint8_t busid, uintptr_t reg_base);

/**
 * @brief CDC ACM 中断处理(在 USB IRQHandler 中调用)。
 * @param busid USB 总线 ID。
 * @details 转发到 CherryUSB 的 USBD_IRQHandler。若使用 rtos_usb_register_isr
 *          注册机制, 则无需调用本函数(用 rtos_usb_irq_dispatch 代替)。
 */
void rtos_usb_cdc_isr(uint8_t busid);

/**
 * @brief 检查 CDC ACM 是否已连接(主机已枚举并打开端口)。
 * @param busid USB 总线 ID。
 * @return true=已连接可读写, false=未连接。
 */
bool rtos_usb_cdc_is_connected(uint8_t busid);

/**
 * @brief 写数据到 CDC ACM(阻塞, 带超时)。
 * @param busid      USB 总线 ID。
 * @param data       待发送数据。
 * @param len        数据长度(字节)。
 * @param timeout_ms 超时(毫秒)。0=不等待(若 TX 忙则返回 0), 0xFFFFFFFF=永久等待。
 * @return 实际发送字节数, 0=超时或未连接或失败。
 * @note  内部自动处理 ZLP(当 len 为端点 MPS 整数倍时补发零包)。
 *        多任务调用安全(内部用互斥锁串行化)。
 */
int rtos_usb_cdc_write(uint8_t busid, const uint8_t *data, uint32_t len, uint32_t timeout_ms);

/**
 * @brief 从 CDC ACM 读数据(阻塞, 带超时)。
 * @param busid      USB 总线 ID。
 * @param buf        接收缓冲。
 * @param len        期望读取的最大字节数。
 * @param timeout_ms 超时(毫秒)。0=不等待(若无数据则返回 0), 0xFFFFFFFF=永久等待。
 * @return 实际读取字节数, 0=超时或未连接。
 * @note  内部维护 RX 环形缓冲, OUT 端点收到的数据自动入队。
 *        本函数从缓冲中拷贝数据, 可能返回少于 len 的字节(只要有数据就返回)。
 */
int rtos_usb_cdc_read(uint8_t busid, uint8_t *buf, uint32_t len, uint32_t timeout_ms);

/**
 * @brief 查询 RX 缓冲中可读字节数(非阻塞)。
 * @param busid USB 总线 ID。
 * @return 当前缓冲区内字节数。
 */
uint32_t rtos_usb_cdc_available(uint8_t busid);

/**
 * @brief 刷新 RX 缓冲(丢弃未读数据)。
 * @param busid USB 总线 ID。
 */
void rtos_usb_cdc_flush_rx(uint8_t busid);

/* ============================== MSC 大容量存储(复合设备) ============================== */
/* 当 rtos_config.h 中 RTOS_CONFIG_USB_USE_MSC=1 时, rtos_usb_cdc_init() 会在
 * 同一 USB 设备上额外注册 MSC 接口(复合设备: 虚拟串口 + U盘)。
 * 存储介质默认为 RAM 盘(RTOS_CONFIG_USB_MSC_RAM_DISK_SIZE), 断电丢数据。
 *
 * 典型用法:
 *   rtos_usb_init();
 *   rtos_usb_cdc_init(0, (uintptr_t)USB_OTG_FS);  // 同时枚举 CDC + MSC
 *
 * 接入真实介质(SPI Flash/SD 卡): 修改 rtos_usb.c 末尾的
 * usbd_msc_get_cap / usbd_msc_sector_read / usbd_msc_sector_write 三个回调。
 */

/**
 * @brief 查询 MSC 扇区读写统计(观测用)。
 * @param busid USB 总线 ID。
 * @param reads  输出: 累计读扇区数(可为 NULL)。
 * @param writes 输出: 累计写扇区数(可为 NULL)。
 * @note  MSC 未启用时输出 0。
 */
void rtos_usb_msc_get_stats(uint8_t busid, uint32_t *reads, uint32_t *writes);

/* ============================== HID(自定义厂商报告, 复合设备) ============================== */
/* 当 rtos_config.h 中 RTOS_CONFIG_USB_USE_HID=1 时, rtos_usb_cdc_init() 会在
 * 同一 USB 设备上再注册一个 HID 接口(自定义厂商定义报告, 64B IN/OUT)。
 *
 * 报告格式(见 rtos_usb.c 的 s_hid_report_descriptor):
 *   Input 报告(设备→主机)  64 字节
 *   Output 报告(主机→设备) 64 字节
 * 主机侧可用 hidapi / libusb 或 CherryUSB 的 test_hid_inout.py 收发测试。
 *
 * 端点: IN=0x85(中断), OUT=0x01(中断)。
 */

/**
 * @brief 发送 HID Input 报告(阻塞, 带超时)。
 * @param busid      USB 总线 ID。
 * @param data       报告数据。
 * @param len        长度(自动截断到 64 字节)。
 * @param timeout_ms 超时(毫秒)。0=不等待, 0xFFFFFFFF=永久等待。
 * @return 实际发送字节数, 0=超时/未连接/失败。
 */
int rtos_usb_hid_write(uint8_t busid, const uint8_t *data, uint32_t len, uint32_t timeout_ms);

/**
 * @brief 接收 HID Output 报告(阻塞, 带超时)。
 * @param busid      USB 总线 ID。
 * @param buf        接收缓冲。
 * @param len        期望读取的最大字节数。
 * @param timeout_ms 超时(毫秒)。0=不等待(无报告返回 0), 0xFFFFFFFF=永久等待。
 * @return 实际读取字节数(≤64), 0=超时/未连接。
 * @note  内部为单报告槽: 上一个报告未读走时新报告被丢弃。
 */
int rtos_usb_hid_read(uint8_t busid, uint8_t *buf, uint32_t len, uint32_t timeout_ms);

/* ============================== CDC ACM 配置宏(可覆盖) ============================== */
/* 用户可在 rtos_config.h 或编译选项中覆盖以下默认值。 */

/** @brief CDC VID(默认 ST 虚拟串口)。 */
#ifndef RTOS_CONFIG_USB_VID
#define RTOS_CONFIG_USB_VID (0x0483U)
#endif
/** @brief CDC PID(默认 ST 虚拟串口)。 */
#ifndef RTOS_CONFIG_USB_PID
#define RTOS_CONFIG_USB_PID (0x5740U)
#endif
/** @brief CDC RX 环形缓冲大小(字节, 建议 >= 256, 建议 2 的幂以启用掩码优化)。 */
#ifndef RTOS_CONFIG_USB_CDC_RX_BUF_SIZE
#define RTOS_CONFIG_USB_CDC_RX_BUF_SIZE (512U)
#endif
/** @brief CDC 端点 OUT 接收缓冲大小(通常 = 全速 MPS = 64)。 */
#ifndef RTOS_CONFIG_USB_CDC_EP_OUT_BUF_SIZE
#define RTOS_CONFIG_USB_CDC_EP_OUT_BUF_SIZE (64U)
#endif
/**
 * @brief CDC TX 内部 DMA 缓冲大小(字节)。
 * @details 用户数据先拷贝到此缓冲再 DMA 发送, 保证:
 *            1. 缓冲区 DMA-safe(非缓存/对齐), 避免 Cortex-M7 D-cache 一致性问题
 *            2. 超时返回后 DMA 不会读到已被用户复用的缓冲
 *            3. 超过此大小的传输自动分块发送
 *          建议 >= 端点 MPS(全速 64 / 高速 512)的整数倍。
 */
#ifndef RTOS_CONFIG_USB_CDC_TX_BUF_SIZE
#define RTOS_CONFIG_USB_CDC_TX_BUF_SIZE (512U)
#endif

/* 编译期配置 sanity check: RX 缓冲必须 >= EP OUT 缓冲, 否则单次 OUT 包都装不下 */
#if RTOS_CONFIG_USB_CDC_RX_BUF_SIZE < RTOS_CONFIG_USB_CDC_EP_OUT_BUF_SIZE
#error "RTOS_CONFIG_USB_CDC_RX_BUF_SIZE must be >= RTOS_CONFIG_USB_CDC_EP_OUT_BUF_SIZE"
#endif

#else /* !RTOS_CONFIG_USE_USB */

/* ---- 关闭 USB 子系统: 提供空桩, 避免用户代码引用报错 ---- */

#define rtos_usb_init() ((rtos_status_t)RTOS_OK)
#define rtos_usb_register_isr(irqn, h, arg) ((rtos_status_t)RTOS_OK)
#define rtos_usb_irq_dispatch(irqn) ((void)0)
#define rtos_usb_malloc(size) ((void *)0)
#define rtos_usb_free(ptr) ((void)0)
#define rtos_usb_msleep(ms) ((void)0)

#define rtos_usb_cdc_init(busid, reg_base) ((rtos_status_t)RTOS_OK)
#define rtos_usb_cdc_isr(busid) ((void)0)
#define rtos_usb_cdc_is_connected(busid) ((bool)false)
#define rtos_usb_cdc_write(busid, d, l, t) ((int)0)
#define rtos_usb_cdc_read(busid, b, l, t) ((int)0)
#define rtos_usb_cdc_available(busid) ((uint32_t)0)
#define rtos_usb_cdc_flush_rx(busid) ((void)0)
#define rtos_usb_hid_write(busid, d, l, t) ((int)0)
#define rtos_usb_hid_read(busid, b, l, t) ((int)0)
#define rtos_usb_msc_get_stats(busid, r, w)                                                       \
    do {                                                                                           \
        if (r)                                                                                     \
            *(r) = 0U;                                                                             \
        if (w)                                                                                     \
            *(w) = 0U;                                                                             \
    } while (0)

#endif /* RTOS_CONFIG_USE_USB */

#ifdef __cplusplus
}
#endif
#endif /* RTOS_USB_H_ */
