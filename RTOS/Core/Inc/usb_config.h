/**
 * @file    usb_config.h
 * @brief   CherryUSB 协议栈配置文件(内置版)
 *
 * @details 本文件是 CherryUSB 的编译期配置入口, 由 CherryUSB 的 core/class/port
 *          源文件通过 #include "usb_config.h" 引入。CherryUSB 在此处读取
 *          CONFIG_USBDEV_* / CONFIG_USBHOST_* / CONFIG_USB_* 等宏来裁剪协议栈。
 *
 *          本文件同时承担"桥接"职责: 包含 rtos_usb.h 使 CherryUSB 的 osal 层
 *          (usb_osal.h) 声明的 usb_osal_* 函数由本 RTOS 提供(在 rtos_usb_osal.c
 *          中实现), 而非 CherryUSB 自带的 osal/usb_osal_*.c。
 *
 *          集成步骤(用户工程):
 *            1. 在编译选项中确保 Core/Inc 在头文件搜索路径中(通常已配置)。
 *            2. 将 lib/third/CherryUSB 下的 core/, class/, common/, port/<chip>/
 *               源文件加入工程编译。
 *            3. 不要编译 CherryUSB 自带的 osal/usb_osal_*.c (本 RTOS 用
 *               Core/Src/rtos_usb_osal.c 替代)。
 *            4. 在 rtos_config.h 中开启 RTOS_CONFIG_USE_USB=1。
 *
 *          自定义: 用户可在 rtos_config.h 中覆盖此处默认值, 或直接修改本文件。
 */
#ifndef USB_CONFIG_H_
#define USB_CONFIG_H_

#include "rtos_config.h" /* 读取 RTOS_CONFIG_USE_USB 等开关 */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== OSAL 桥接 ============================== */
/* 包含本 RTOS 的 USB 子系统头文件, 使 CherryUSB 的 usb_osal_* 函数声明
 * 与本 RTOS 的实现(rtos_usb_osal.c)链接。CherryUSB 的 usb_osal.h 将
 * usb_osal_sem_t / usb_osal_mutex_t 等定义为 void *, 与本 RTOS 的内部
 * 结构体指针(rtos_usb_sem_t *)通过 void* 转换衔接, 无需类型重定义。 */
#include "rtos_usb.h"

/* ============================== USB 通用配置 ============================== */

/* printf 输出函数(CherryUSB USB_LOG_* 宏依赖) */
#include <stdio.h>
#ifndef CONFIG_USB_PRINTF
#define CONFIG_USB_PRINTF(...) printf(__VA_ARGS__)
#endif

/* 调试日志级别: USB_DBG_ERROR=0 / USB_DBG_WARNING=1 / USB_DBG_INFO=2 / USB_DBG_LOG=3 */
#ifndef CONFIG_USB_DBG_LEVEL
#define CONFIG_USB_DBG_LEVEL USB_DBG_INFO
#endif

/* DMA / dcache 对齐大小(字节) */
#ifndef CONFIG_USB_ALIGN_SIZE
#define CONFIG_USB_ALIGN_SIZE 4
#endif

/* USB_NO_CACHE_RAM_SECTION 属性(用于 DMA 缓冲区) */
#ifndef USB_NOCACHE_RAM_SECTION
#define USB_NOCACHE_RAM_SECTION
#endif

/* 内存池单池最大块数(usb_mempool.h 用) */
#ifndef CONFIG_USB_MEMPOOL_MAX_BLOCK_COUNT
#define CONFIG_USB_MEMPOOL_MAX_BLOCK_COUNT 16
#endif

/* ============================== USB 设备栈配置 ============================== */

/* 最大 USB 设备总线数(单 USB IP 时为 1) */
#ifndef CONFIG_USBDEV_MAX_BUS
#define CONFIG_USBDEV_MAX_BUS 1
#endif

/* EP0 Setup 请求缓冲区长度(字节) */
#ifndef CONFIG_USBDEV_REQUEST_BUFFER_LEN
#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512
#endif

/* EP0 线程模式(可选, 关闭则在 ISR 中处理 Setup) */
/* #define CONFIG_USBDEV_EP0_THREAD */
#ifndef CONFIG_USBDEV_EP0_PRIO
#define CONFIG_USBDEV_EP0_PRIO (RTOS_CONFIG_USB_THREAD_PRIORITY)
#endif
#ifndef CONFIG_USBDEV_EP0_STACKSIZE
#define CONFIG_USBDEV_EP0_STACKSIZE (RTOS_CONFIG_USB_THREAD_STACK_SIZE * 4U)
#endif

/* 每设备最大端点数(部分 DCD port 驱动需要) */
#ifndef CONFIG_USBDEV_EP_NUM
#define CONFIG_USBDEV_EP_NUM 8
#endif

/* ============================== MSC 设备类配置 ============================== */
/* usbd_msc.c 需要: MAX_LUN / MAX_BUFSIZE, 以及 THREAD 或 POLLING 二选一。
 * 此处选择线程模式(CONFIG_USBDEV_MSC_THREAD): MSC 的 SCSI 读写处理运行在
 * 独立线程(经本 RTOS 的 usb_osal_thread_create 创建), 不占用 ISR 时间。 */
#if RTOS_CONFIG_USE_USB && RTOS_CONFIG_USB_USE_MSC

#ifndef CONFIG_USBDEV_MSC_MAX_LUN
#define CONFIG_USBDEV_MSC_MAX_LUN 1
#endif

#ifndef CONFIG_USBDEV_MSC_MAX_BUFSIZE
#define CONFIG_USBDEV_MSC_MAX_BUFSIZE 512
#endif

#define CONFIG_USBDEV_MSC_THREAD

#ifndef CONFIG_USBDEV_MSC_STACKSIZE
/* ★ 必须与线程池槽内嵌栈一致(rtos_usb.c 的 rtos_usb_thread_t.stack,
 *   大小 = RTOS_CONFIG_USB_THREAD_STACK_SIZE 字): rtos_usb_thread_create
 *   会把超过池大小的请求钳制到池大小。若此处配置更大值, MSC 线程栈
 *   会被静默截断(实际栈 = 配置意图的 1/4), 栈溢出为静默内存损坏。
 *   需要更大的 MSC 栈时, 应同步增大 RTOS_CONFIG_USB_THREAD_STACK_SIZE
 *   (注意 RAM 预算: 每个线程槽 4x 字节数)。 */
#define CONFIG_USBDEV_MSC_STACKSIZE (RTOS_CONFIG_USB_THREAD_STACK_SIZE)
#endif

#ifndef CONFIG_USBDEV_MSC_PRIO
#define CONFIG_USBDEV_MSC_PRIO (RTOS_CONFIG_USB_THREAD_PRIORITY)
#endif

/* SCSI INQUIRY 响应中的厂商/产品/版本字符串(usbd_msc.c 要求, 各 <=8/16/4 字符) */
#ifndef CONFIG_USBDEV_MSC_MANUFACTURER_STRING
#define CONFIG_USBDEV_MSC_MANUFACTURER_STRING "RTOS"
#endif
#ifndef CONFIG_USBDEV_MSC_PRODUCT_STRING
#define CONFIG_USBDEV_MSC_PRODUCT_STRING "RAM Disk"
#endif
#ifndef CONFIG_USBDEV_MSC_VERSION_STRING
#define CONFIG_USBDEV_MSC_VERSION_STRING "1.00"
#endif

#endif /* RTOS_CONFIG_USE_USB && RTOS_CONFIG_USB_USE_MSC */

/* ============================== DWC2 FIFO 自定义划分 ============================== */
/* F446 OTG_FS 共 6 个端点(EP0~EP5)、FIFO 总深 320 字(1280B)。
 * CherryUSB 默认 ST 参数只给 EP0~EP3 分配 IN 端点 TX FIFO, 而复合设备
 * (CDC bulk IN + CDC 通知 IN + MSC bulk IN + HID 中断 IN)需要 4 个非控制
 * IN 端点, 因此启用自定义 FIFO 划分(实现在 rtos_usb.c 的
 * dwc2_get_user_fifo_config), 并利用"同一端点号 IN/OUT 为两个独立端点":
 *   EP0 控制 | EP1 IN=CDC bulk(0x81) EP1 OUT=HID(0x01)
 *   EP2 OUT=CDC bulk(0x02) | EP3 IN=CDC 通知(0x83)
 *   EP4 IN=MSC bulk(0x84) | EP5 OUT=MSC bulk(0x05) EP5 IN=HID(0x85) */
#if RTOS_CONFIG_USE_USB
#ifndef CONFIG_USB_DWC2_CUSTOM_FIFO
#define CONFIG_USB_DWC2_CUSTOM_FIFO
#endif
#endif

/* ============================== USB 高速模式(可选) ============================== */
/* 若芯片支持 USB 高速(480Mbps), 在 rtos_config.h 或此处开启:
 *   #define CONFIG_USB_HS
 * 此时 CDC_MAX_MPS 等自动切换为 512 字节。 */

/* ============================== USB 主机栈配置(可选) ============================== */
#if RTOS_CONFIG_USB_HOST_MODE
#ifndef CONFIG_USBHOST_MAX_RHPORTS
#define CONFIG_USBHOST_MAX_RHPORTS 1
#endif
#ifndef CONFIG_USBHOST_MAX_EXTHUBS
#define CONFIG_USBHOST_MAX_EXTHUBS 1
#endif
#ifndef CONFIG_USBHOST_MAX_EHPORTS
#define CONFIG_USBHOST_MAX_EHPORTS 4
#endif
#ifndef CONFIG_USBHOST_MAX_INTERFACES
#define CONFIG_USBHOST_MAX_INTERFACES 8
#endif
#ifndef CONFIG_USBHOST_MAX_INTF_ALTSETTINGS
#define CONFIG_USBHOST_MAX_INTF_ALTSETTINGS 2
#endif
#ifndef CONFIG_USBHOST_MAX_ENDPOINTS
#define CONFIG_USBHOST_MAX_ENDPOINTS 4
#endif
#ifndef CONFIG_USBHOST_PSC_PRIO
#define CONFIG_USBHOST_PSC_PRIO (RTOS_CONFIG_USB_THREAD_PRIORITY)
#endif
#ifndef CONFIG_USBHOST_PSC_STACKSIZE
#define CONFIG_USBHOST_PSC_STACKSIZE (RTOS_CONFIG_USB_THREAD_STACK_SIZE * 4U)
#endif
#ifndef CONFIG_USBHOST_REQUEST_BUFFER_LEN
#define CONFIG_USBHOST_REQUEST_BUFFER_LEN 2048
#endif
#ifndef CONFIG_USBHOST_MAX_BUS
#define CONFIG_USBHOST_MAX_BUS 1
#endif
#endif /* RTOS_CONFIG_USB_HOST_MODE */

#ifdef __cplusplus
}
#endif
#endif /* USB_CONFIG_H_ */
