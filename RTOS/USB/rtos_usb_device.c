/**
 * @file    rtos_usb_device.c
 * @brief   CherryUSB 复合设备描述符、CDC/HID 传输与 DWC2 FIFO 配置。
 *
 * @details CDC/HID 共用设备连接状态与端点回调, 在同一模块维护会话和超时预算。
 *          对象池与 OSAL 适配由 rtos_usb_osal.c 提供, MSC 介质由 rtos_usb_msc.c 提供。
 *          FIFO 配置面向 F446 OTG_FS; 更换控制器时需重新核对端点与 FIFO 容量。
 */
#include "rtos_usb.h"

#if RTOS_CONFIG_USE_USB

#include <string.h>

#include "usbd_core.h"
#include "usbd_cdc_acm.h"
#if RTOS_CONFIG_USB_USE_HID
#include "usbd_hid.h"
#endif
#if RTOS_CONFIG_USB_USE_MSC
#include "usbd_msc.h"
#endif
#include "usb_dwc2_param.h"

#include "rtos_internal.h"
#include "rtos_sched.h"

/* ============================== DWC2 FIFO 自定义划分 ============================== */
/* F446 OTG_FS: 6 端点(EP0~EP5), FIFO 总深 320 字。默认 ST 参数只给 EP0~EP3
 * 分配 TX FIFO, 无法支撑复合设备的 4 个非控制 IN 端点。重新划分:
 *   RX FIFO          = 176 字 (704B, 所有 OUT 端点共享, 最小要求 47 字)
 *   TX FIFO[0](EP0)  = 16 字 (控制端点)
 *   TX FIFO[1](EP1)  = 64 字 (256B, CDC bulk IN 0x81, 全速 MPS=64B 余量充足)
 *   TX FIFO[2](EP2)  = 16 字 (保留, EP2 仅用作 OUT)
 *   TX FIFO[3](EP3)  = 16 字 (64B, CDC 通知 IN 0x83)
 *   TX FIFO[4](EP4)  = 16 字 (64B, MSC bulk IN 0x84, 全速 MPS=64B 恰好)
 *   TX FIFO[5](EP5)  = 16 字 (64B, HID 中断 IN 0x85, 与 MSC OUT 0x05 同号不同向)
 * 合计 176+144=320 字, 与 GHWCFG3 报告的 DFIFO 深度一致。
 * 由 usb_config.h 中的 CONFIG_USB_DWC2_CUSTOM_FIFO 启用。 */
void dwc2_get_user_fifo_config(uint32_t reg_base, struct usb_dwc2_user_fifo_config *config)
{
    (void)reg_base;
    config->device_rx_fifo_size = 176U;
    memset(config->device_tx_fifo_size, 0, sizeof(config->device_tx_fifo_size));
    config->device_tx_fifo_size[0] = 16U; /* EP0 控制 */
    config->device_tx_fifo_size[1] = 64U; /* EP1 IN: CDC bulk */
    config->device_tx_fifo_size[2] = 16U; /* EP2 保留 */
    config->device_tx_fifo_size[3] = 16U; /* EP3 IN: CDC 通知 */
    config->device_tx_fifo_size[4] = 16U; /* EP4 IN: MSC bulk */
    config->device_tx_fifo_size[5] = 16U; /* EP5 IN: HID 中断 */
}

/*============================================================================*/
/*====================  CDC ACM 用户封装(虚拟串口)  ==========================*/
/*============================================================================*/

/* 端点地址(端点号 <= 5, 受 F446 OTG_FS 6 端点限制; 同端点号 IN/OUT 互相独立) */
#define CDC_IN_EP 0x81 /* EP1 IN (CDC 批量 IN) */
#define CDC_OUT_EP 0x02 /* EP2 OUT (CDC 批量 OUT) */
#define CDC_INT_EP 0x83 /* EP3 IN (CDC 通知) */

#if RTOS_CONFIG_USB_USE_MSC
#define MSC_IN_EP 0x84 /* EP4 IN (MSC 批量 IN) */
#define MSC_OUT_EP 0x05 /* EP5 OUT (MSC 批量 OUT) */
#endif

#if RTOS_CONFIG_USB_USE_HID
#define HID_OUT_EP 0x01 /* EP1 OUT (HID 中断 OUT, 与 CDC_IN_EP 同端点号不同方向) */
#define HID_IN_EP 0x85 /* EP5 IN (HID 中断 IN, 与 MSC_OUT_EP 同端点号不同方向) */
#define HID_MPS 64U /* HID 报告大小(字节) */
#define HID_INTERVAL 1U /* 轮询间隔(ms) */
#endif

#define USBD_VID RTOS_CONFIG_USB_VID
#define USBD_PID RTOS_CONFIG_USB_PID
#define USBD_MAX_POWER 100 /* 电流(单位 2mA, 100=200mA) */
#define USBD_LANGID_STRING 1033 /* 英语 */

/* 复合设备接口数: CDC(IAD, 2 接口) + MSC(1) + HID(1) */
#define USBD_CONFIG_ITF_NUM \
    (0x02U + (RTOS_CONFIG_USB_USE_MSC ? 1U : 0U) + (RTOS_CONFIG_USB_USE_HID ? 1U : 0U))

#define USB_CONFIG_SIZE                                                                              \
    (9U + CDC_ACM_DESCRIPTOR_LEN + (RTOS_CONFIG_USB_USE_MSC ? MSC_DESCRIPTOR_LEN : 0U) +             \
     (RTOS_CONFIG_USB_USE_HID ? HID_CUSTOM_INOUT_DESCRIPTOR_LEN : 0U))

#ifdef CONFIG_USB_HS
#define CDC_MAX_MPS 512
#else
#define CDC_MAX_MPS 64
#endif

#if RTOS_CONFIG_USB_USE_MSC
#ifdef CONFIG_USB_HS
#define MSC_MAX_MPS 512
#else
#define MSC_MAX_MPS 64
#endif
#endif

/* 类被裁剪时提供长度回退值, 使上面的 USB_CONFIG_SIZE 宏始终可展开 */
#if !RTOS_CONFIG_USB_USE_MSC
#define MSC_DESCRIPTOR_LEN 0U
#endif
#if !RTOS_CONFIG_USB_USE_HID
#define HID_CUSTOM_INOUT_DESCRIPTOR_LEN 0U
#endif

/* A power-of-two capacity divides 2^32, so masking wrapping byte counters
 * preserves the physical cursor. Other capacities use positions modulo the
 * capacity directly. Both representations reserve one byte for full/empty. */
#define RTOS_USB_IS_POW2(x) (((x) != 0U) && (((x) & ((x) - 1U)) == 0U))

static inline uint32_t cdc_rx_position(uint32_t cursor)
{
#if RTOS_USB_IS_POW2(RTOS_CONFIG_USB_CDC_RX_BUF_SIZE)
    return cursor & (RTOS_CONFIG_USB_CDC_RX_BUF_SIZE - 1U);
#else
    return cursor;
#endif
}

static inline uint32_t cdc_rx_count(uint32_t head, uint32_t tail)
{
#if RTOS_USB_IS_POW2(RTOS_CONFIG_USB_CDC_RX_BUF_SIZE)
    return head - tail;
#else
    return (head >= tail) ? (head - tail) : (RTOS_CONFIG_USB_CDC_RX_BUF_SIZE - (tail - head));
#endif
}

static inline uint32_t cdc_rx_advance(uint32_t cursor, uint32_t count)
{
#if RTOS_USB_IS_POW2(RTOS_CONFIG_USB_CDC_RX_BUF_SIZE)
    return cursor + count;
#else
    /* count is at most the ring's usable capacity, so one wrap suffices. */
    const uint32_t until_end = RTOS_CONFIG_USB_CDC_RX_BUF_SIZE - cursor;
    return (count < until_end) ? (cursor + count) : (count - until_end);
#endif
}

#ifndef RTOS_USB_MIN
#define RTOS_USB_MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* ---- 描述符 ---- */

#if RTOS_CONFIG_USB_USE_HID
/* 自定义厂商定义 HID 报告描述符: 64 字节 Input 报告 + 64 字节 Output 报告。
 * 主机侧可用 hidapi 或 CherryUSB 的 tools/test_srcipts/test_hid_inout.py 测试。 */
static const uint8_t s_hid_report_descriptor[] = {
    0x06, 0x00, 0xFF, /* Usage Page (Vendor Defined 0xFF00) */
    0x09, 0x01,       /* Usage (0x01) */
    0xA1, 0x01,       /* Collection (Application) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255) */
    0x75, 0x08,       /*   Report Size (8 bits) */
    0x95, 0x40,       /*   Report Count (64) --- Input 报告 */
    0x09, 0x02,       /*   Usage (0x02) */
    0x81, 0x02,       /*   Input (Data, Var, Abs) */
    0x95, 0x40,       /*   Report Count (64) --- Output 报告 */
    0x09, 0x03,       /*   Usage (0x03) */
    0x91, 0x02,       /*   Output (Data, Var, Abs) */
    0xC0,             /* End Collection */
};
#endif

static const uint8_t s_device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0100, 0x01)};

static const uint8_t s_config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, USBD_CONFIG_ITF_NUM, 0x01, USB_CONFIG_BUS_POWERED,
                               USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, CDC_MAX_MPS, 0x02)
#if RTOS_CONFIG_USB_USE_MSC
        ,
    MSC_DESCRIPTOR_INIT(0x02, MSC_OUT_EP, MSC_IN_EP, MSC_MAX_MPS, 0x00)
#endif
#if RTOS_CONFIG_USB_USE_HID
        ,
    /* HID 接口号: CDC 占 0/1, MSC 启用时占 2, HID 紧随其后 */
    HID_CUSTOM_INOUT_DESCRIPTOR_INIT((0x02U + (RTOS_CONFIG_USB_USE_MSC ? 1U : 0U)), 0x00,
                                     sizeof(s_hid_report_descriptor), HID_OUT_EP, HID_IN_EP,
                                     HID_MPS, HID_INTERVAL)
#endif
};

static const uint8_t s_device_quality_descriptor[] = {
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
};

static const char *s_string_descriptors[] = {
    (const char[]){0x09, 0x04}, /* Langid */
    "RTOS", /* Manufacturer */
#if RTOS_CONFIG_USB_USE_HID
    "RTOS CDC MSC HID", /* Product (复合设备: 虚拟串口 + U盘 + HID) */
#elif RTOS_CONFIG_USB_USE_MSC
    "RTOS CDC MSC", /* Product (复合设备: 虚拟串口 + U盘) */
#else
    "RTOS CDC ACM", /* Product */
#endif
    "000000000001", /* Serial Number */
};

static const uint8_t *s_device_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return s_device_descriptor;
}

static const uint8_t *s_config_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return s_config_descriptor;
}

static const uint8_t *s_device_quality_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return s_device_quality_descriptor;
}

static const char *s_string_descriptor_callback(uint8_t speed, uint8_t index)
{
    (void)speed;
    if (index >= (sizeof(s_string_descriptors) / sizeof(char *))) {
        return NULL;
    }
    return s_string_descriptors[index];
}

static const struct usb_descriptor s_cdc_descriptor = {
    .device_descriptor_callback = s_device_descriptor_callback,
    .config_descriptor_callback = s_config_descriptor_callback,
    .device_quality_descriptor_callback = s_device_quality_descriptor_callback,
    .string_descriptor_callback = s_string_descriptor_callback,
};

/* ---- CDC 设备上下文 ---- */

typedef struct {
    volatile bool connected; /* 主机已配置 */
    volatile bool tx_busy; /* TX DMA 传输进行中(回调清除) */
    volatile bool tx_has_failed; /* IN 回调报告当前块的终止错误 */
    volatile uint32_t tx_done_seq; /* 当前块成功或失败都结束等待，连接事件只更新 session */
    volatile uint32_t session_seq; /* RESET/断开/重新配置使在途操作失效 */
    rtos_usb_sem_t *rx_sem; /* RX 数据可用信号量 */
    rtos_usb_sem_t *tx_sem; /* TX 完成信号量 */
    rtos_usb_mutex_t *tx_mutex; /* TX 串行化互斥锁 */
    /* RX 环形缓冲(ISR 写, 任务读) */
    uint8_t rx_buf[RTOS_CONFIG_USB_CDC_RX_BUF_SIZE];
    volatile uint32_t rx_head; /* 写入位置(ISR 更新) */
    volatile uint32_t rx_tail; /* 读取位置(任务更新) */
    volatile uint32_t rx_dropped; /* RX 缓冲满丢弃的字节数(调试观测) */
    /* OUT 端点接收缓冲(DMA 用, 非缓存+对齐) */
    USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t ep_out_buf[RTOS_CONFIG_USB_CDC_EP_OUT_BUF_SIZE];
    /* IN 端点发送缓冲(DMA 用, 非缓存+对齐)。
     * 用户数据先拷贝到此缓冲再 DMA, 保证:
     *   1. 超时返回后 DMA 不会读到已复用的用户缓冲
     *   2. Cortex-M7 D-cache 一致性(非缓存段) */
    USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t ep_in_buf[RTOS_CONFIG_USB_CDC_TX_BUF_SIZE];
    /* CherryUSB 接口/端点对象 */
    struct usbd_interface intf0; /* CDC 通信接口 */
    struct usbd_interface intf1; /* CDC 数据接口 */
    struct usbd_endpoint cdc_out_ep; /* OUT 端点 */
    struct usbd_endpoint cdc_in_ep; /* IN 端点 */
#if RTOS_CONFIG_USB_USE_MSC
    struct usbd_interface msc_intf; /* MSC 接口(端点由 usbd_msc 类内部管理) */
#endif
#if RTOS_CONFIG_USB_USE_HID
    struct usbd_interface hid_intf; /* HID 接口 */
    struct usbd_endpoint hid_out_ep; /* HID 中断 OUT 端点 */
    struct usbd_endpoint hid_in_ep; /* HID 中断 IN 端点 */
    rtos_usb_sem_t *hid_tx_sem; /* HID IN 发送完成信号量 */
    rtos_usb_sem_t *hid_rx_sem; /* HID OUT 报告到达信号量 */
    rtos_usb_mutex_t *hid_tx_mutex; /* HID 写入串行化互斥锁(多任务并发保护) */
    volatile bool hid_tx_busy; /* HID IN 发送进行中 */
    volatile uint32_t hid_tx_done_seq;
    volatile uint32_t hid_rx_len; /* 待读 OUT 报告长度(0=无) */
    uint8_t hid_rx_buf[HID_MPS]; /* OUT 报告单槽缓冲(读走前新报告被丢弃) */
    /* IN/OUT 端点 DMA 缓冲(非缓存+对齐) */
    USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t hid_ep_in_buf[HID_MPS];
    USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t hid_ep_out_buf[HID_MPS];
#endif
    bool initialized; /* 已初始化标志 */
} rtos_usb_cdc_dev_t;

static rtos_usb_cdc_dev_t s_cdc_dev[CONFIG_USBDEV_MAX_BUS];

/* ---- 事件处理器 ---- */

static void usb_change_session(rtos_usb_cdc_dev_t *dev, uint8_t event)
{
    const bool is_configured = event == USBD_EVENT_CONFIGURED;

    /* Invalidate the old session and detach its readers in one critical section. */
    RTOS_PORT_ENTER_CRITICAL();
    ++dev->session_seq;
    dev->connected = is_configured;
    dev->tx_busy = false;
    if (event != USBD_EVENT_DISCONNECTED) {
        dev->rx_head = 0U;
        dev->rx_tail = 0U;
    }
    if (is_configured) {
        dev->rx_dropped = 0U;
    }
    if (dev->rx_sem != NULL) {
        rtos_internal_sem_cancel_waiters(&dev->rx_sem->sem);
    }
    if (dev->tx_sem != NULL) {
        /* A pending token already wakes the serialized writer. */
        (void)rtos_usb_sem_give(dev->tx_sem);
    }
#if RTOS_CONFIG_USB_USE_HID
    dev->hid_tx_busy = false;
    dev->hid_rx_len = 0U;
    if (dev->hid_rx_sem != NULL) {
        rtos_internal_sem_cancel_waiters(&dev->hid_rx_sem->sem);
    }
    if (dev->hid_tx_sem != NULL) {
        (void)rtos_usb_sem_give(dev->hid_tx_sem);
    }
#endif
    RTOS_PORT_EXIT_CRITICAL();
}

static void rtos_usb_cdc_event_handler(uint8_t busid, uint8_t event)
{
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return;
    }
    RTOS_ASSERT_ISR_OK();
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    switch (event) {
        case USBD_EVENT_CONFIGURED:
            usb_change_session(dev, event);
            /* 启动首次 OUT 读取(异步, 收到数据后回调中继续读) */
            usbd_ep_start_read(busid, CDC_OUT_EP, dev->ep_out_buf,
                               RTOS_CONFIG_USB_CDC_EP_OUT_BUF_SIZE);
#if RTOS_CONFIG_USB_USE_HID
            /* 启动首次 HID OUT 读取 */
            usbd_ep_start_read(busid, HID_OUT_EP, dev->hid_ep_out_buf, HID_MPS);
#endif
            break;

        case USBD_EVENT_DISCONNECTED:
        case USBD_EVENT_RESET:
            usb_change_session(dev, event);
            break;

        default:
            break;
    }
}

/* ---- OUT 端点回调(数据到达) ---- */
/* 性能优化: 用 memcpy 两段拷贝替代逐字节循环, ISR 耗时减少 5~10 倍。
 * 环形缓冲可能跨越末尾, 需要分两段拷贝:
 *   第一段: head -> 缓冲末尾
 *   第二段: 缓冲起始 -> 剩余空间 */

static void rtos_usb_cdc_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return;
    }
    RTOS_ASSERT_ISR_OK();
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    if (nbytes > 0U) {
        RTOS_PORT_ENTER_CRITICAL();
        const uint32_t head = dev->rx_head;
        const uint32_t tail = dev->rx_tail;
        const uint32_t space = RTOS_CONFIG_USB_CDC_RX_BUF_SIZE - 1U - cdc_rx_count(head, tail);
        /* 防御: nbytes 不应超过 ep_out_buf 大小(硬件契约), 但防御性截断 */
        uint32_t bounded_nbytes = RTOS_USB_MIN(nbytes, RTOS_CONFIG_USB_CDC_EP_OUT_BUF_SIZE);
        uint32_t to_copy = RTOS_USB_MIN(bounded_nbytes, space);

        if (to_copy > 0U) {
            const uint32_t offset = cdc_rx_position(head);
            const uint32_t first_chunk =
                RTOS_USB_MIN(to_copy, RTOS_CONFIG_USB_CDC_RX_BUF_SIZE - offset);
            memcpy(&dev->rx_buf[offset], &dev->ep_out_buf[0], first_chunk);

            /* 第二段: 缓冲起始到剩余 */
            uint32_t second_chunk = to_copy - first_chunk;
            if (second_chunk > 0U) {
                memcpy(&dev->rx_buf[0], &dev->ep_out_buf[first_chunk], second_chunk);
            }
            dev->rx_head = cdc_rx_advance(head, to_copy);
        }
        /* 统计丢弃字节(调试观测, 不影响功能) */
        if (nbytes > to_copy) {
            dev->rx_dropped += (nbytes - to_copy);
        }
        RTOS_PORT_EXIT_CRITICAL();

        /* 通知读线程有数据可用 */
        if (dev->rx_sem != NULL) {
            rtos_usb_sem_give(dev->rx_sem);
        }
    }

    /* 启动下一次 OUT 读取 */
    usbd_ep_start_read(busid, CDC_OUT_EP, dev->ep_out_buf, RTOS_CONFIG_USB_CDC_EP_OUT_BUF_SIZE);
}

/* ---- IN 端点回调(发送完成) ---- */
static void cdc_handle_in_completion(uint8_t busid, uint32_t nbytes, uint16_t mps)
{
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];
    if (!dev->tx_busy) {
        return;
    }

    bool is_zlp_required = (nbytes != 0U) && (mps != 0U) && ((nbytes % mps) == 0U);
    if (is_zlp_required && (usbd_ep_start_write(busid, CDC_IN_EP, NULL, 0U) == 0)) {
        return; /* The accepted ZLP will complete through this callback again. */
    }

    /* A rejected ZLP cannot produce another completion, so finish this block here. */
    dev->tx_has_failed = is_zlp_required;
    dev->tx_busy = false;
    dev->tx_done_seq++;
    if (dev->tx_sem != NULL) {
        /* A full semaphore already contains a wakeup. The sequence carries the result. */
        (void)rtos_usb_sem_give(dev->tx_sem);
    }
}

static void rtos_usb_cdc_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return;
    }
    RTOS_ASSERT_ISR_OK();
    cdc_handle_in_completion(busid, nbytes, usbd_get_ep_mps(busid, ep));
}

/* ---- HID IN 端点回调(报告发送完成) ---- */
/* 中断端点无 ZLP 语义(仅批量/控制端点需要), 直接通知即可。 */
#if RTOS_CONFIG_USB_USE_HID
static void rtos_usb_hid_in_done(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;
    (void)nbytes;
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return;
    }
    RTOS_ASSERT_ISR_OK();
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    if (dev->hid_tx_busy) {
        dev->hid_tx_busy = false;
        dev->hid_tx_done_seq++;
        if (dev->hid_tx_sem != NULL) {
            rtos_usb_sem_give(dev->hid_tx_sem);
        }
    }
}

/* ---- HID OUT 端点回调(主机发来 Output 报告) ---- */
/* 单槽缓冲: 上一个报告未被读走时, 新报告直接丢弃(避免撕裂)。 */
static void rtos_usb_hid_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return;
    }
    RTOS_ASSERT_ISR_OK();
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    if (nbytes > HID_MPS) {
        nbytes = HID_MPS;
    }
    if (nbytes > 0U) {
        RTOS_PORT_ENTER_CRITICAL();
        if (dev->hid_rx_len == 0U) {
            memcpy(dev->hid_rx_buf, dev->hid_ep_out_buf, nbytes);
            dev->hid_rx_len = nbytes;
        }
        RTOS_PORT_EXIT_CRITICAL();

        if (dev->hid_rx_sem != NULL) {
            rtos_usb_sem_give(dev->hid_rx_sem);
        }
    }

    /* 启动下一次 HID OUT 读取 */
    usbd_ep_start_read(busid, HID_OUT_EP, dev->hid_ep_out_buf, HID_MPS);
}
#endif /* RTOS_CONFIG_USB_USE_HID */

/* ---- CDC ACM 公共 API ---- */

static void release_device_ipc(rtos_usb_cdc_dev_t *dev)
{
    rtos_usb_sem_delete(dev->rx_sem);
    rtos_usb_sem_delete(dev->tx_sem);
    rtos_usb_mutex_delete(dev->tx_mutex);
    dev->rx_sem = NULL;
    dev->tx_sem = NULL;
    dev->tx_mutex = NULL;
#if RTOS_CONFIG_USB_USE_HID
    rtos_usb_sem_delete(dev->hid_tx_sem);
    rtos_usb_sem_delete(dev->hid_rx_sem);
    rtos_usb_mutex_delete(dev->hid_tx_mutex);
    dev->hid_tx_sem = NULL;
    dev->hid_rx_sem = NULL;
    dev->hid_tx_mutex = NULL;
#endif
}

rtos_status_t rtos_usb_cdc_init(uint8_t busid, uintptr_t reg_base)
{
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return RTOS_ERR_PARAM;
    }
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    if (dev->initialized) {
        return RTOS_ERR_ALREADY;
    }

    /* 初始化上下文 */
    memset(dev, 0, sizeof(*dev));

    /* 创建同步对象 */
    dev->rx_sem = rtos_usb_sem_create(0U, RTOS_CONFIG_USB_CDC_RX_BUF_SIZE);
    if (dev->rx_sem == NULL) {
        return RTOS_ERR_NO_MEM;
    }
    dev->tx_sem = rtos_usb_sem_create(0U, 1U);
    if (dev->tx_sem == NULL) {
        rtos_usb_sem_delete(dev->rx_sem);
        dev->rx_sem = NULL;
        return RTOS_ERR_NO_MEM;
    }
    dev->tx_mutex = rtos_usb_mutex_create();
    if (dev->tx_mutex == NULL) {
        rtos_usb_sem_delete(dev->rx_sem);
        rtos_usb_sem_delete(dev->tx_sem);
        dev->rx_sem = NULL;
        dev->tx_sem = NULL;
        return RTOS_ERR_NO_MEM;
    }
#if RTOS_CONFIG_USB_USE_HID
    dev->hid_tx_sem = rtos_usb_sem_create(0U, 1U);
    if (dev->hid_tx_sem == NULL) {
        rtos_usb_sem_delete(dev->rx_sem);
        rtos_usb_sem_delete(dev->tx_sem);
        rtos_usb_mutex_delete(dev->tx_mutex);
        dev->rx_sem = NULL;
        dev->tx_sem = NULL;
        dev->tx_mutex = NULL;
        return RTOS_ERR_NO_MEM;
    }
    dev->hid_rx_sem = rtos_usb_sem_create(0U, 1U);
    if (dev->hid_rx_sem == NULL) {
        rtos_usb_sem_delete(dev->rx_sem);
        rtos_usb_sem_delete(dev->tx_sem);
        rtos_usb_mutex_delete(dev->tx_mutex);
        rtos_usb_sem_delete(dev->hid_tx_sem);
        dev->rx_sem = NULL;
        dev->tx_sem = NULL;
        dev->tx_mutex = NULL;
        dev->hid_tx_sem = NULL;
        return RTOS_ERR_NO_MEM;
    }
    dev->hid_tx_mutex = rtos_usb_mutex_create();
    if (dev->hid_tx_mutex == NULL) {
        rtos_usb_sem_delete(dev->rx_sem);
        rtos_usb_sem_delete(dev->tx_sem);
        rtos_usb_mutex_delete(dev->tx_mutex);
        rtos_usb_sem_delete(dev->hid_tx_sem);
        rtos_usb_sem_delete(dev->hid_rx_sem);
        dev->rx_sem = NULL;
        dev->tx_sem = NULL;
        dev->tx_mutex = NULL;
        dev->hid_tx_sem = NULL;
        dev->hid_rx_sem = NULL;
        return RTOS_ERR_NO_MEM;
    }
#endif

    /* 注册端点回调 */
    dev->cdc_out_ep.ep_addr = CDC_OUT_EP;
    dev->cdc_out_ep.ep_cb = rtos_usb_cdc_bulk_out;
    dev->cdc_in_ep.ep_addr = CDC_IN_EP;
    dev->cdc_in_ep.ep_cb = rtos_usb_cdc_bulk_in;
#if RTOS_CONFIG_USB_USE_HID
    dev->hid_out_ep.ep_addr = HID_OUT_EP;
    dev->hid_out_ep.ep_cb = rtos_usb_hid_out;
    dev->hid_in_ep.ep_addr = HID_IN_EP;
    dev->hid_in_ep.ep_cb = rtos_usb_hid_in_done;
#endif

    /* CherryUSB 初始化序列 */
    usbd_desc_register(busid, &s_cdc_descriptor);
    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &dev->intf0));
    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &dev->intf1));
    usbd_add_endpoint(busid, &dev->cdc_out_ep);
    usbd_add_endpoint(busid, &dev->cdc_in_ep);
#if RTOS_CONFIG_USB_USE_MSC
    struct usbd_interface *msc_interface =
        usbd_msc_init_intf(busid, &dev->msc_intf, MSC_OUT_EP, MSC_IN_EP);
    if (msc_interface == NULL) {
        release_device_ipc(dev);
        return RTOS_ERR_NO_MEM;
    }
    usbd_add_interface(busid, msc_interface);
    usbd_msc_set_readonly(busid, RTOS_CONFIG_USB_MSC_READONLY ? true : false);
#endif
#if RTOS_CONFIG_USB_USE_HID
    /* HID 接口(报告描述符经 usbd_hid_init_intf 注册, GET_DESCRIPTOR 由类内部处理) */
    usbd_add_interface(busid, usbd_hid_init_intf(busid, &dev->hid_intf, s_hid_report_descriptor,
                                                 sizeof(s_hid_report_descriptor)));
    usbd_add_endpoint(busid, &dev->hid_out_ep);
    usbd_add_endpoint(busid, &dev->hid_in_ep);
#endif

    int ret = usbd_initialize(busid, reg_base, rtos_usb_cdc_event_handler);
    if (ret != 0) {
        /* Stop the controller and class worker before releasing device IPC. */
        (void)usbd_deinitialize(busid);
        release_device_ipc(dev);
        return RTOS_ERR_PARAM;
    }

    dev->initialized = true;
    return RTOS_OK;
}

void rtos_usb_cdc_isr(uint8_t busid)
{
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return;
    }
    RTOS_ASSERT_ISR_OK();
    USBD_IRQHandler(busid);
}

bool rtos_usb_cdc_is_connected(uint8_t busid)
{
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return false;
    }
    return s_cdc_dev[busid].connected;
}

/* One budget covers lock contention, old transfers, and all chunks. Subtracting
 * elapsed ticks also works when tick_count wraps during an operation. */
static rtos_tick_t usb_timeout_remaining(rtos_tick_t started, rtos_tick_t budget)
{
    if (budget == RTOS_WAIT_FOREVER) {
        return RTOS_WAIT_FOREVER;
    }
    rtos_tick_t elapsed = rtos_sched_get_tick_count() - started;
    return elapsed >= budget ? 0U : budget - elapsed;
}

int rtos_usb_cdc_write(uint8_t busid, const uint8_t *data, uint32_t len, uint32_t timeout_ms)
{
    if ((busid >= CONFIG_USBDEV_MAX_BUS) || (data == NULL) || (len == 0U) ||
        RTOS_PORT_IN_ISR()) {
        return 0;
    }
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];
    const uint32_t session = dev->session_seq;

    if (!dev->connected) {
        return 0;
    }

    rtos_tick_t started = rtos_sched_get_tick_count();
    rtos_tick_t budget = rtos_usb_timeout_ticks(timeout_ms);
    uint32_t total_sent = 0U;
    if (rtos_mutex_take(&dev->tx_mutex->mutex, usb_timeout_remaining(started, budget)) != RTOS_OK) {
        return 0;
    }
    while ((total_sent < len) && dev->connected && (session == dev->session_seq)) {
        while (dev->tx_busy) {
            if (rtos_usb_sem_take(dev->tx_sem, usb_timeout_remaining(started, budget)) != RTOS_OK) {
                goto done;
            }
            if (!dev->connected || (session != dev->session_seq)) {
                goto done;
            }
        }
        if ((total_sent != 0U) && (usb_timeout_remaining(started, budget) == 0U)) {
            break;
        }
        uint32_t chunk = len - total_sent;
        if (chunk > RTOS_CONFIG_USB_CDC_TX_BUF_SIZE) {
            chunk = RTOS_CONFIG_USB_CDC_TX_BUF_SIZE;
        }

        memcpy(dev->ep_in_buf, &data[total_sent], chunk);
        RTOS_PORT_ENTER_CRITICAL();
        if (!dev->connected || (session != dev->session_seq)) {
            RTOS_PORT_EXIT_CRITICAL();
            break;
        }
        rtos_usb_sem_reset(dev->tx_sem);
        dev->tx_has_failed = false;
        dev->tx_busy = true;
        uint32_t seq = dev->tx_done_seq;

        int ret = usbd_ep_start_write(busid, CDC_IN_EP, dev->ep_in_buf, chunk);
        if (ret != 0) {
            dev->tx_busy = false;
        }
        RTOS_PORT_EXIT_CRITICAL();
        if (ret != 0) {
            break;
        }
        while (dev->tx_done_seq == seq) {
            if (!dev->connected || (session != dev->session_seq) ||
                (rtos_usb_sem_take(dev->tx_sem, usb_timeout_remaining(started, budget)) != RTOS_OK)) {
                goto done;
            }
        }
        if ((session != dev->session_seq) || dev->tx_has_failed) {
            break;
        }
        total_sent += chunk;
    }
done:
    rtos_usb_mutex_give(dev->tx_mutex);
    return (int)total_sent;
}

int rtos_usb_cdc_read(uint8_t busid, uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    if ((busid >= CONFIG_USBDEV_MAX_BUS) || (buf == NULL) || (len == 0U)) {
        return 0;
    }
    RTOS_ASSERT_ISR_OK();
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];
    const uint32_t session = dev->session_seq;
    rtos_usb_sem_t *const signal = dev->rx_sem;
    rtos_tick_t timeout = rtos_usb_timeout_ticks(timeout_ms);
    rtos_tick_t started = rtos_sched_get_tick_count();

    for (;;) {
        /* 块拷贝读取: 用 memcpy 两段拷贝替代逐字节循环 */
        RTOS_PORT_ENTER_CRITICAL();
        if (session != dev->session_seq) {
            RTOS_PORT_EXIT_CRITICAL();
            return 0;
        }
        const uint32_t head = dev->rx_head;
        const uint32_t tail = dev->rx_tail;
        const uint32_t avail = cdc_rx_count(head, tail);
        uint32_t to_read = RTOS_USB_MIN(len, avail);

        if (to_read > 0U) {
            const uint32_t offset = cdc_rx_position(tail);
            const uint32_t first_chunk =
                RTOS_USB_MIN(to_read, RTOS_CONFIG_USB_CDC_RX_BUF_SIZE - offset);
            memcpy(&buf[0], &dev->rx_buf[offset], first_chunk);

            /* 第二段: 缓冲起始到剩余 */
            uint32_t second_chunk = to_read - first_chunk;
            if (second_chunk > 0U) {
                memcpy(&buf[first_chunk], &dev->rx_buf[0], second_chunk);
            }
            dev->rx_tail = cdc_rx_advance(tail, to_read);
        }
        const bool is_connected = dev->connected;
        RTOS_PORT_EXIT_CRITICAL();

        if (to_read > 0U) {
            return (int)to_read;
        }

        /* Keep the session check and wait registration atomic inside the semaphore. */
        if (!is_connected || signal == NULL ||
            rtos_internal_sem_take_guarded(&signal->sem, usb_timeout_remaining(started, timeout),
                                           &dev->session_seq, session) != RTOS_OK) {
            return 0;
        }
    }
}

uint32_t rtos_usb_cdc_available(uint8_t busid)
{
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return 0U;
    }
    RTOS_ASSERT_ISR_OK();
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    RTOS_PORT_ENTER_CRITICAL();
    uint32_t head = dev->rx_head;
    uint32_t tail = dev->rx_tail;
    RTOS_PORT_EXIT_CRITICAL();

    return cdc_rx_count(head, tail);
}

void rtos_usb_cdc_flush_rx(uint8_t busid)
{
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return;
    }
    RTOS_ASSERT_ISR_OK();
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    RTOS_PORT_ENTER_CRITICAL();
    dev->rx_tail = dev->rx_head;
    RTOS_PORT_EXIT_CRITICAL();
}

/*============================================================================*/
/*====================  HID 用户封装(自定义厂商报告)  ========================*/
/*============================================================================*/

#if RTOS_CONFIG_USB_USE_HID

int rtos_usb_hid_write(uint8_t busid, const uint8_t *data, uint32_t len, uint32_t timeout_ms)
{
    if ((busid >= CONFIG_USBDEV_MAX_BUS) || (data == NULL) || (len == 0U) ||
        RTOS_PORT_IN_ISR()) {
        return 0;
    }
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];
    const uint32_t session = dev->session_seq;

    if (!dev->connected) {
        return 0;
    }
    if (len > HID_MPS) {
        len = HID_MPS; /* 单报告最大 64B */
    }
    rtos_tick_t timeout = rtos_usb_timeout_ticks(timeout_ms);
    rtos_tick_t started = rtos_sched_get_tick_count();
    int sent = 0;
    if (rtos_mutex_take(&dev->hid_tx_mutex->mutex,
                        usb_timeout_remaining(started, timeout)) != RTOS_OK) {
        return 0;
    }
    while (dev->hid_tx_busy) {
        if (!dev->connected || (session != dev->session_seq) ||
            (rtos_usb_sem_take(dev->hid_tx_sem, usb_timeout_remaining(started, timeout)) != RTOS_OK)) {
            goto done;
        }
    }
    memcpy(dev->hid_ep_in_buf, data, len);
    RTOS_PORT_ENTER_CRITICAL();
    if (!dev->connected || (session != dev->session_seq)) {
        RTOS_PORT_EXIT_CRITICAL();
        goto done;
    }
    rtos_usb_sem_reset(dev->hid_tx_sem);
    dev->hid_tx_busy = true;
    uint32_t seq = dev->hid_tx_done_seq;
    int ret = usbd_ep_start_write(busid, HID_IN_EP, dev->hid_ep_in_buf, len);
    if (ret != 0) {
        dev->hid_tx_busy = false;
    }
    RTOS_PORT_EXIT_CRITICAL();
    if (ret != 0) {
        goto done;
    }
    while (dev->hid_tx_done_seq == seq) {
        if (!dev->connected || (session != dev->session_seq) ||
            (rtos_usb_sem_take(dev->hid_tx_sem, usb_timeout_remaining(started, timeout)) != RTOS_OK)) {
            goto done;
        }
    }
    if (session == dev->session_seq) {
        sent = (int)len;
    }
done:
    rtos_usb_mutex_give(dev->hid_tx_mutex);
    return sent;
}

int rtos_usb_hid_read(uint8_t busid, uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    if ((busid >= CONFIG_USBDEV_MAX_BUS) || (buf == NULL) || (len == 0U)) {
        return 0;
    }
    RTOS_ASSERT_ISR_OK();
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];
    const uint32_t session = dev->session_seq;
    rtos_usb_sem_t *const signal = dev->hid_rx_sem;
    rtos_tick_t timeout = rtos_usb_timeout_ticks(timeout_ms);
    rtos_tick_t started = rtos_sched_get_tick_count();

    for (;;) {
        /* 单槽缓冲: 有待读报告则拷贝返回 */
        RTOS_PORT_ENTER_CRITICAL();
        if (session != dev->session_seq) {
            RTOS_PORT_EXIT_CRITICAL();
            return 0;
        }
        uint32_t n = dev->hid_rx_len;
        if (n > 0U) {
            if (n > len) {
                n = len;
            }
            memcpy(buf, dev->hid_rx_buf, n);
            dev->hid_rx_len = 0U;
        }
        const bool is_connected = dev->connected;
        RTOS_PORT_EXIT_CRITICAL();

        if (n > 0U) {
            return (int)n;
        }

        if (!is_connected || signal == NULL ||
            rtos_internal_sem_take_guarded(&signal->sem, usb_timeout_remaining(started, timeout),
                                           &dev->session_seq, session) != RTOS_OK) {
            return 0;
        }
    }
}

#else /* !RTOS_CONFIG_USB_USE_HID */

int rtos_usb_hid_write(uint8_t busid, const uint8_t *data, uint32_t len, uint32_t timeout_ms)
{
    (void)busid;
    (void)data;
    (void)len;
    (void)timeout_ms;
    return 0;
}

int rtos_usb_hid_read(uint8_t busid, uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    (void)busid;
    (void)buf;
    (void)len;
    (void)timeout_ms;
    return 0;
}

#endif /* RTOS_CONFIG_USB_USE_HID */

#if defined(RTOS_HIL_AUDIT_PENDSV)
/* Audit-only transport seam: tests run real CDC/HID APIs and callbacks, while
 * linker wrappers replace endpoint register access. No USB device is opened. */
int rtos_usb_audit_begin(void)
{
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[0];
    memset(dev, 0, sizeof(*dev));
    dev->rx_sem = rtos_usb_sem_create(0U, RTOS_CONFIG_USB_CDC_RX_BUF_SIZE);
    dev->tx_sem = rtos_usb_sem_create(0U, 1U);
    dev->tx_mutex = rtos_usb_mutex_create();
#if RTOS_CONFIG_USB_USE_HID
    dev->hid_rx_sem = rtos_usb_sem_create(0U, 1U);
    dev->hid_tx_sem = rtos_usb_sem_create(0U, 1U);
    dev->hid_tx_mutex = rtos_usb_mutex_create();
    if (!dev->hid_rx_sem || !dev->hid_tx_sem || !dev->hid_tx_mutex) {
        return -1;
    }
#endif
    dev->connected = true;
    return (dev->rx_sem && dev->tx_sem && dev->tx_mutex) ? 0 : -1;
}

void rtos_usb_audit_event(uint8_t event)
{
    rtos_usb_cdc_event_handler(0U, event);
}

/** @brief Inject a CDC endpoint completion with a test-transport packet size. */
void rtos_usb_audit_cdc_in(uint32_t nbytes, uint16_t mps)
{
    RTOS_ASSERT_ISR_OK();
    cdc_handle_in_completion(0U, nbytes, mps);
}

void rtos_usb_audit_hid_rx(const uint8_t *data, uint32_t length)
{
#if RTOS_CONFIG_USB_USE_HID
    length = RTOS_USB_MIN(length, HID_MPS);
    memcpy(s_cdc_dev[0].hid_ep_out_buf, data, length);
    rtos_usb_hid_out(0U, HID_OUT_EP, length);
#else
    (void)data;
    (void)length;
#endif
}

void rtos_usb_audit_end(void)
{
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[0];
    rtos_usb_sem_delete(dev->rx_sem);
    rtos_usb_sem_delete(dev->tx_sem);
    rtos_usb_mutex_delete(dev->tx_mutex);
#if RTOS_CONFIG_USB_USE_HID
    rtos_usb_sem_delete(dev->hid_rx_sem);
    rtos_usb_sem_delete(dev->hid_tx_sem);
    rtos_usb_mutex_delete(dev->hid_tx_mutex);
#endif
    memset(dev, 0, sizeof(*dev));
}
#endif

#if defined(RTOS_HIL_USB_CDC_CURSOR_TEST)
/* Seed an empty test ring and inject bytes through the real receive callback. */
void rtos_usb_cdc_cursor_test_seed(uint32_t total)
{
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[0];
    memset(dev->rx_buf, 0, sizeof(dev->rx_buf));
#if !RTOS_USB_IS_POW2(RTOS_CONFIG_USB_CDC_RX_BUF_SIZE)
    total %= RTOS_CONFIG_USB_CDC_RX_BUF_SIZE;
#endif
    dev->rx_head = total;
    dev->rx_tail = total;
}

bool rtos_usb_cdc_cursor_test_push(const uint8_t *data, uint32_t count)
{
    if ((data == NULL) || (count > RTOS_CONFIG_USB_CDC_EP_OUT_BUF_SIZE)) {
        return false;
    }
    memcpy(s_cdc_dev[0].ep_out_buf, data, count);
    rtos_usb_cdc_bulk_out(0U, CDC_OUT_EP, count);
    return true;
}
#endif

#endif /* RTOS_CONFIG_USE_USB */
