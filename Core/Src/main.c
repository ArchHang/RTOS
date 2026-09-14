/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usart.h"
#include "usb_otg.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "rtos.h"
#include "rtos_usb.h"
#include "asm_test.h"
#include "ai_app.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static uint8_t ft_dispatch(uint8_t byte);   /* 故障测试命令分发(定义在 ft_* 任务之后) */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
int _write(int file, char *ptr, int len)
{
    (void)file;

    HAL_UART_Transmit(&huart2, (uint8_t *)ptr, len, 1000);

    return len;
}

void itm_printf(char *fmt, ...)
{
    va_list args;
    char str[128];
    int len;

    va_start(args, fmt);
    len = vsprintf(str, fmt, args);
    for (int i = 0; i < len; i++) {
        ITM_SendChar(str[i]);
    }
    va_end(args);
}

static rtos_tcb_t      *task_tcb;

static void task_0(void *arg)
{
    uint32_t cnt = 0;
    (void)arg;

    while (1) {
        // printf("task 0 running\r\n");
        // cnt = asm_increment(cnt);
        // cnt = asm_pass_args(cnt, 1, 2, 3, 4);
        // printf("cnt = %d\r\n", cnt);
        // asm_printf("0123456789\r\n");
        (void)cnt;
        rtos_task_delay(1000);
    }
}

static rtos_tcb_t      *task1_tcb;

static void task_1(void *arg)
{
    (void)arg;
    while (1) {
        // printf("task 1 running\r\n");
        rtos_task_delay(500);
    }
}

static rtos_event_t     algo_task_event;
static rtos_tcb_t      *task_uart_tcb;
static uint8_t          uart2_queue_storage[4096];
static rtos_queue_t     uart2_rx_queue;
static uint8_t          audio_buf[4096];
static uint16_t         audio_cnt = 0U;
static void task_uart(void *arg)
{
    uint8_t byte;

    (void)arg;
    while (1) {
        /* 队列空则阻塞等待(WAIT_FOREVER), 收到一个字节 */
        if (rtos_queue_recv(&uart2_rx_queue, &byte, RTOS_WAIT_FOREVER) == RTOS_OK) {
            /* HardFault 测试调度: 数字命令通知对应测试任务, 不进音频缓冲 */
            if (ft_dispatch(byte)) {
                continue;
            }
            audio_buf[audio_cnt++] = byte;
            if (audio_cnt >= 2048) {
                audio_cnt = 0U;
                rtos_event_set(&algo_task_event, 0x01U);
            }
        }
    }
}

static rtos_tcb_t      *algo_task_tcb;


static void algo_task(void *arg)
{
    float logits[4];
    uint32_t class_id = 0;

    (void)arg;
    while (1) {
        rtos_event_wait(&algo_task_event, 0x01U, RTOS_EVENT_WAIT_ANY | RTOS_EVENT_CLEAR_ON_EXIT, RTOS_WAIT_FOREVER);
        printf("algo_task running\r\n");

        uint32_t tick = rtos_sched_get_tick_count();
        ai_forward_u8(audio_buf, logits, &class_id);
        switch (class_id) {
            case 0:
                printf("无人声\r\n");
                break;
            case 1:
                printf("其他人\r\n");
                break;
            case 2:
                printf("小原\r\n");
                break;
            case 3:
                printf("小芯\r\n");
                break;
            default:
                printf("未知错误\r\n");
                break;
        }
        printf("algo cost time = %u\r\n", (unsigned)(rtos_sched_get_tick_count() - tick));
    }
}
















/* ==================================================================== */
/*          HardFault 诊断验证: 故障触发测试任务                          */
/*                                                                      */
/* 通过 USART2 发送数字选择测试项(每项触发后系统停机, 需复位再做下一项):  */
/*   '1' → 精确总线故障  (BFSR.PRECISERR + BFARVALID + BFAR)             */
/*   '2' → 非法状态故障  (UFSR.INVSTATE, 栈帧 xPSR.T=0)                 */
/*   '3' → 除零故障      (UFSR.DIVBYZERO, 需 CCR.DIV_0_TRP)             */
/*   '4' → 栈边界演示    (margin 报告 + stack_magic 毁坏 + 总线故障)    */
/*   '5' → 未定义指令    (UFSR.UNDEFINSTR, 执行 UDF 编码)               */
/*   '6' → 非对齐访问    (UFSR.UNALIGNED, LDRD 永久禁止非对齐)           */
/*   '7' → 写越界故障    (BFSR, 写缓冲异步 → 常见 IMPRECISERR)          */
/*   '8' → 真实栈溢出    (SP 低于栈底, 专用小栈 + 献祭缓冲)             */
/*   '9' → ISR 内故障    (命令后发任意字节, 验证 Handler 模式帧/IPSR)    */
/* 1~8 各一个任务(同时验证任务归因), 9 在 USART2 ISR 内直接触发。        */
/* ==================================================================== */

static rtos_tcb_t *ft1_tcb;
static rtos_tcb_t *ft2_tcb;
static rtos_tcb_t *ft3_tcb;
static rtos_tcb_t *ft4_tcb;
static rtos_tcb_t *ft5_tcb;
static rtos_tcb_t *ft6_tcb;
static rtos_tcb_t *ft7_tcb;

/* 测试9: ISR 内故障的布防标志('9' 命令置位, 下一字节的 RX 中断触发) */
static volatile uint8_t g_isr_fault_arm = 0U;

/* 测试2 用到的正常函数(地址 bit0=1, 取反后模拟被破坏的函数指针) */
static void ft_noop(void)
{
}

/* ---- 测试1: 精确总线故障 ----
 * 读 RAM 末尾之外的保留地址(读操作是同步的 → PRECISERR, PC/BFAR 可信) */
static void ftask_bus(void *arg)
{
    (void)arg;
    uint32_t v;

    for (;;) {
        rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v);
        printf("[FT1] BusFault: read 0x20020000 (beyond RAM end)\r\n");
        v = *(volatile uint32_t *)0x20020000UL;   /* ← 故障点 */
        (void)v;
    }
}

/* ---- 测试2: 非法状态 ----
 * BLX 到 bit0=0 的地址(非 Thumb) → UFSR.INVSTATE, 栈帧 xPSR.T=0 */
static void ftask_invstate(void *arg)
{
    (void)arg;
    uint32_t v;

    for (;;) {
        rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v);
        printf("[FT2] UsageFault INVSTATE: call non-Thumb address\r\n");
        /* volatile 阻止编译器看穿掩码直接 bl ft_noop */
        volatile uint32_t target = ((uint32_t)ft_noop) & ~1UL;
        ((void (*)(void))target)();             /* ← 故障点 */
    }
}

/* ---- 测试3: 除零 ----
 * CCR.DIV_0_TRP=1 时 UDIV 除零 → UFSR.DIVBYZERO (UsageFault) */
static void ftask_div0(void *arg)
{
    (void)arg;
    uint32_t v;

    for (;;) {
        rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v);
        printf("[FT3] UsageFault DIVBYZERO\r\n");
        volatile uint32_t z = 0U;
        volatile uint32_t r = 100U / z;            /* ← 故障点 */
        (void)r;
    }
}

/* ---- 测试4: 栈边界演示 ----
 * noinline 保证递归每层真实消耗栈; 最深层破坏 TCB.stack_magic 模拟
 * "TCB 被越界写", 随后触发总线故障 → dump 报告 margin 与 magic 报警。 */
static uint32_t __attribute__((noinline)) ft_recurse(uint32_t depth)
{
    volatile char pad[256];                     /* 每层消耗 ~256B */
    pad[0]  = (char)depth;
    pad[255] = (char)(depth + 1U);

    if (depth > 0U) {
        return ft_recurse(depth - 1U) + (uint32_t)(uint8_t)pad[0];
    }

    /* 最深层: 此时任务栈已被消耗大半, dump 的 margin 将明显小于 size */
#if RTOS_CONFIG_CHECK_FOR_STACK_OVERFLOW
    ft4_tcb->stack_magic = 0x12345678U;           /* 模拟 TCB 被越界写破坏 */
#endif
    {
        volatile uint32_t v = *(volatile uint32_t *)0x20020000UL;  /* ← 故障点 */
        (void)v;
    }
    return (uint32_t)(uint8_t)pad[0];
}

static void ftask_stack(void *arg)
{
    (void)arg;
    uint32_t v;

    for (;;) {
        rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v);
        printf("[FT4] stack margin + magic + BusFault\r\n");
        ft_recurse(2U);                         /* 递归 2 层再触发故障 */
    }
}

/* ---- 测试5: 未定义指令 ----
 * .short 0xDE00 = UDF #0 (ARMv7-M 永久未定义的指令编码),
 * 执行到该半字即触发 UsageFault: UFSR.UNDEFINSTR, PC 指向该指令。
 * 模拟场景: 函数指针跑飞落入数据区/代码被改写。 */
static void ftask_undef(void *arg)
{
    (void)arg;
    uint32_t v;

    for (;;) {
        rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v);
        printf("[FT5] UsageFault UNDEFINSTR: execute UDF halfword\r\n");
        __asm volatile(".short 0xDE00");        /* ← 故障点: UDF #0 */
    }
}

/* ---- 测试6: 非对齐访问 ----
 * LDRD 基址必须字对齐, 非 4 整数倍时无条件触发 UsageFault:
 * UFSR.UNALIGNED。与 CCR.UNALIGN_TRP 无关 —— 普通 ldr/str 的非对齐
 * 访问是架构合法操作(仅陷阱开启时才报), 而 ldrd/ldm/stm/ldrex
 * 永久禁止非对齐。 */
static uint8_t ft6_buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};

static void ftask_unaligned(void *arg)
{
    (void)arg;
    uint32_t v;
    uint32_t lo, hi;

    for (;;) {
        rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v);
        printf("[FT6] UsageFault UNALIGNED: ldrd from misaligned address\r\n");
        v = (uint32_t)&ft6_buf[1];              /* 地址 ≡1 (mod 4) 非对齐 */
        __asm volatile("ldrd %0, %1, [%2]" : "=&r"(lo), "=&r"(hi) : "r"(v)); /* ← 故障点 */
        (void)lo;
        (void)hi;
    }
}

/* ---- 测试7: 写越界(非精确总线故障) ----
 * 对 RAM 末尾之外的保留地址写入。写操作经写缓冲异步下发, 总线错误
 * 常在若干条指令之后才回报: BFSR.IMPRECISERR, 此时 PC/BFAR 均不可信
 * (dump 会明确提示"不可信"), 用于验证非精确路径的呈现。
 * 注: 部分总线实现可能报 PRECISERR, 亦属正常。 */
static void ftask_imprecise(void *arg)
{
    (void)arg;
    uint32_t v;

    for (;;) {
        rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v);
        printf("[FT7] BusFault write 0x20020000 (imprecise expected)\r\n");
        *(volatile uint32_t *)0x20020000UL = 0x12345678U;   /* ← 故障点 */
    }
}

/* ---- 测试8: 真实栈溢出 ----
 * 专用 512B 小栈: 递归 4 层 ×(160B 垫片+帧) ≈ 800B, SP 深入栈底之下
 * 数百字节, 再触发总线故障 → dump 报告 "*** SP已低于栈底: 栈溢出! ***"
 * 及真实(负)margin。献祭区声明在栈数组之前(典型 .bss 布局下位于其低
 * 地址侧), 吸收溢出写入以保护其他静态数据; 极端布局下输出可能失真,
 * 但故障本身仍会被捕获。 */
static rtos_tcb_t  ft8_tcb;
static uint32_t    ft8_guard[128];                /* 512B 献祭缓冲 */
static rtos_stack_t ft8_stack[128];               /* 512B 任务栈 */

static uint32_t __attribute__((noinline)) ft_recurse8(uint32_t depth)
{
    volatile char pad[160];                       /* 每层 ~160B + 帧开销 */

    pad[0]   = (char)depth;
    pad[159] = (char)(depth + 1U);

    if (depth > 0U) {
        return ft_recurse8(depth - 1U) + (uint32_t)(uint8_t)pad[0];
    }

    /* 最深层: SP 已低于 ft8_stack 栈底, 处于献祭区内 */
    {
        volatile uint32_t v = *(volatile uint32_t *)0x20020000UL;   /* ← 故障点 */
        (void)v;
    }
    return (uint32_t)(uint8_t)pad[0];
}

static void ftask_overflow(void *arg)
{
    (void)arg;
    uint32_t v;

    /* 献祭区仅被动接收溢出写入, 无需显式访问; 引用一次消除未用警告 */
    (void)ft8_guard;

    for (;;) {
        rtos_task_notify_wait(RTOS_WAIT_FOREVER, &v);
        printf("[FT8] real stack overflow: recurse 4x160B in 512B stack\r\n");
        ft_recurse8(3U);                        /* 递归 4 层(3→0) */
    }
}

/* UART 数字命令 → 通知对应测试任务触发故障; 返回 1 表示命令已消费 */
static uint8_t ft_dispatch(uint8_t byte)
{
    switch (byte) {
        case '1':
            rtos_task_notify(ft1_tcb, 1U, RTOS_NOTIFY_VALUE);
            return 1U;
        case '2':
            rtos_task_notify(ft2_tcb, 1U, RTOS_NOTIFY_VALUE);
            return 1U;
        case '3':
            rtos_task_notify(ft3_tcb, 1U, RTOS_NOTIFY_VALUE);
            return 1U;
        case '4':
            rtos_task_notify(ft4_tcb, 1U, RTOS_NOTIFY_VALUE);
            return 1U;
        case '5':
            rtos_task_notify(ft5_tcb, 1U, RTOS_NOTIFY_VALUE);
            return 1U;
        case '6':
            rtos_task_notify(ft6_tcb, 1U, RTOS_NOTIFY_VALUE);
            return 1U;
        case '7':
            rtos_task_notify(ft7_tcb, 1U, RTOS_NOTIFY_VALUE);
            return 1U;
        case '8':
            rtos_task_notify(&ft8_tcb, 1U, RTOS_NOTIFY_VALUE);
            return 1U;
        case '9':
            g_isr_fault_arm = 1U;   /* 再发任意字节即在 ISR 内触发 */
            return 1U;
        default:
            return 0U;
    }
}

uint8_t g_uart2_rx;
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    /* HardFault 测试9: 命令 '9' 布防后, 在本回调(USART2 中断上下文)内
     * 触发总线故障。用于验证 Handler 模式故障路径:
     * EXC_RETURN bit3=0(帧压在 MSP)、dump 提示"错误发生在 ISR 内"、
     * 栈帧 IPSR 报出被打断的异常号(USART2 = 16+38 = 54)。 */
    if (g_isr_fault_arm != 0U) {
        g_isr_fault_arm = 0U;
        volatile uint32_t v = *(volatile uint32_t *)0x20020000UL;   /* ← ISR 内故障点 */
        (void)v;
    }

    HAL_UART_Receive_IT(&huart2, (uint8_t *)&g_uart2_rx, 1);
    /* ISR 中发送必须 RTOS_NO_WAIT; 队列按值拷贝单字节 */
    rtos_queue_send(&uart2_rx_queue, &g_uart2_rx, RTOS_NO_WAIT, RTOS_FALSE);
}

/* ==================================================================== */
/*          USB 任务: 虚拟串口(CDC) + 大容量存储(MSC)                     */
/*                                                                      */
/* 设备为复合设备: 插入主机后同时枚举出                                  */
/*   - 一个虚拟串口(CDC ACM, 可用串口助手收发)                           */
/*   - 一个 U 盘(MSC RAM 盘, 首次使用需在主机端格式化)                   */
/* 初始化在 main() 中完成(rtos_usb_init + rtos_usb_cdc_init),           */
/* MSC 的 SCSI 处理由 CherryUSB 内部线程(usbd_msc)完成。                */
/* ==================================================================== */

static rtos_tcb_t *usb_cdc_tcb;

/* CDC 任务: 虚拟串口回显。主机打开串口并发送的数据会被原样发回。 */
static void task_usb_cdc(void *arg)
{
    uint8_t buf[64];
    bool banner_sent = false;

    (void)arg;
    while (1) {
        if (!rtos_usb_cdc_is_connected(0)) {
            banner_sent = false;
            rtos_task_delay(100);
            continue;
        }

        /* 每次连接后发一次欢迎信息 */
        if (!banner_sent) {
            static const char banner[] = "RTOS USB CDC ready\r\n";
            rtos_usb_cdc_write(0, (const uint8_t *)banner, sizeof(banner) - 1U, 1000);
            banner_sent = true;
        }

        /* 阻塞等待主机数据(1s 超时), 收到后原样回显 */
        int n = rtos_usb_cdc_read(0, buf, sizeof(buf), 1000);
        if (n > 0) {
            rtos_usb_cdc_write(0, buf, (uint32_t)n, 1000);
        }
    }
}

static rtos_tcb_t *usb_msc_tcb;

/* MSC 任务: 监视 U 盘扇区读写统计, 有变化时经 USART2 打印。
 * (实际 SCSI 读写由 CherryUSB 的 usbd_msc 内部线程处理,
 *  本任务仅做状态观测, 也可在此挂接文件系统等扩展逻辑) */
static void task_usb_msc(void *arg)
{
    uint32_t last_reads = 0, last_writes = 0;
    uint32_t reads, writes;

    (void)arg;
    while (1) {
        rtos_task_delay(2000);
        rtos_usb_msc_get_stats(0, &reads, &writes);
        if ((reads != last_reads) || (writes != last_writes)) {
            printf("[MSC] sectors read=%u write=%u\r\n", (unsigned)reads, (unsigned)writes);
            last_reads = reads;
            last_writes = writes;
        }
    }
}

static rtos_tcb_t *usb_hid_tcb;

/* HID 任务: 自定义厂商定义 HID 设备(64B IN/OUT 报告)。
 *  - 每秒发送一个 Input 报告: [0]=0x5A 魔术字, [1]=序号, [2..5]=系统 tick
 *  - 主机发来的 Output 报告原样回显, 并经 USART2 打印提示
 * 主机侧可用 hidapi 或 CherryUSB 的 test_hid_inout.py 测试。 */
static void task_usb_hid(void *arg)
{
    uint8_t in_rep[64];
    uint8_t out_rep[64];
    uint8_t seq = 0U;

    (void)arg;
    while (1) {
        rtos_task_delay(1000);
        if (!rtos_usb_cdc_is_connected(0)) {
            continue; /* 复合设备未完成枚举(同一设备的 CDC 连接标志) */
        }

        /* 非阻塞检查主机是否发来 Output 报告, 有则回显 + 打印 */
        int n = rtos_usb_hid_read(0, out_rep, sizeof(out_rep), 0);
        if (n > 0) {
            rtos_usb_hid_write(0, out_rep, (uint32_t)n, 1000);
            printf("[HID] out report recv %d bytes, echoed\r\n", n);
        }

        /* 周期性 Input 报告 */
        memset(in_rep, 0, sizeof(in_rep));
        in_rep[0] = 0x5AU;
        in_rep[1] = seq++;
        uint32_t tick = rtos_sched_get_tick_count();
        memcpy(&in_rep[2], &tick, sizeof(tick));
        rtos_usb_hid_write(0, in_rep, sizeof(in_rep), 1000);
    }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  /* HardFault 诊断前置(设计文档: docs/hardfault_debug.md):
   * 1. 使能三类可配置故障 —— 否则所有错误升级为 HardFault 且 CFSR 不留记录,
   *    错误分别进入 UsageFault/BusFault/MemManage Handler(已接入诊断模块)。
   * 2. DIV_0_TRP: 把静默除零(默认返回0)变成可定位故障(测试3需要)。
   * 3. ★ 不开启 UNALIGN_TRP: 普通 ldr/str 的非对齐访问在 M3/M4/M7 上是
   *    架构支持的合法操作(GCC 的 store-merging 会主动生成之), 开启陷阱
   *    会误伤正常代码甚至故障处理自身(2026-08 真机事故); 需要
   *    UFSR.UNALIGNED 时用 ldrd/ldm 类永久禁止非对齐的指令触发(测试6)。 */
  SCB->SHCSR |= (7UL << 16);                     /* USAGEFAULTENA | BUSFAULTENA | MEMFAULTENA */
  SCB->CCR   |= SCB_CCR_DIV_0_TRP_Msk;

  /* 注意: 不要调用 MX_USB_OTG_FS_PCD_Init()。USB 内核寄存器由 CherryUSB
   * DWC2 驱动(usb_dc_dwc2.c)接管, HAL PCD 只复用其 MspInit 做 48MHz 时钟/
   * GPIO/中断初始化(由 rtos_usb_cdc_init 内部的 usb_dc_init 触发)。
   * 若调用 HAL_PCD_Init 会与 DWC2 驱动抢占同一组 USB 寄存器。 */
  rtos_queue_init(&uart2_rx_queue, uart2_queue_storage, 1, sizeof(uart2_queue_storage));
  HAL_UART_Receive_IT(&huart2, (uint8_t *)&g_uart2_rx, 1);

  rtos_event_init(&algo_task_event);
  printf("System init done\r\n");
  printf("[FAULT-TEST] send '1'..'9' via USART2 (each test halts, reset between):\r\n");
  printf("  1: BusFault    precise read  @0x20020000\r\n");
  printf("  2: INVSTATE    call non-Thumb address\r\n");
  printf("  3: DIVBYZERO   (DIV_0_TRP enabled)\r\n");
  printf("  4: stack demo  (margin + magic + BusFault)\r\n");
  printf("  5: UNDEFINSTR  execute UDF halfword\r\n");
  printf("  6: UNALIGNED ldrd misaligned (always faults)\r\n");
  printf("  7: BusFault    write @0x20020000 (imprecise expected)\r\n");
  printf("  8: real stack overflow (SP below base)\r\n");
  printf("  9: fault inside USART2 ISR (send '9', then any byte)\r\n");


  rtos_init();
  /* USB 子系统: 初始化对象池/USB 堆, 再初始化 USB 设备(OTG_FS)。
   * RTOS_CONFIG_USB_USE_MSC=1 时为复合设备: 虚拟串口 + U盘(RAM 盘)。
   * 需在 rtos_start() 之前调用。 */
  rtos_usb_init();
  rtos_usb_cdc_init(0, (uintptr_t)USB_OTG_FS);

  rtos_task_create(&task_tcb, 256, task_0, NULL, 6, "task0");
  rtos_task_create(&task1_tcb, 256, task_1, NULL, 5, "task1");
  rtos_task_create(&task_uart_tcb, 256, task_uart, NULL, 1, "uart");
  rtos_task_create(&algo_task_tcb, 256, algo_task, NULL, 10, "algo");

  /* HardFault 诊断验证任务: 阻塞等待 UART 数字命令后触发对应故障 */
  rtos_task_create(&ft1_tcb, 256, ftask_bus,       NULL, 7, "ft1_bus");
  rtos_task_create(&ft2_tcb, 256, ftask_invstate,  NULL, 7, "ft2_invst");
  rtos_task_create(&ft3_tcb, 256, ftask_div0,      NULL, 7, "ft3_div0");
  rtos_task_create(&ft4_tcb, 256, ftask_stack,     NULL, 7, "ft4_stk");
  rtos_task_create(&ft5_tcb, 256, ftask_undef,     NULL, 7, "ft5_undef");
  rtos_task_create(&ft6_tcb, 256, ftask_unaligned, NULL, 7, "ft6_align");
  rtos_task_create(&ft7_tcb, 256, ftask_imprecise, NULL, 7, "ft7_impr");
  /* 测试8 用静态小栈(512B)+献祭缓冲, 不能用动态大栈 */
  rtos_task_create_static(&ft8_tcb, ft8_stack, 128, ftask_overflow, NULL, 7, "ft8_ovf");
  /* 测试9 无需任务: 命令布防后在 USART2 ISR 内直接触发 */

  /* USB 应用任务: CDC 回显 + MSC 状态监视 + HID 报告收发
   * (优先级 8, 低于 USB 协议栈线程 16) */
  rtos_task_create(&usb_cdc_tcb, 256, task_usb_cdc, NULL, 8, "usb_cdc");
  rtos_task_create(&usb_msc_tcb, 256, task_usb_msc, NULL, 8, "usb_msc");
  rtos_task_create(&usb_hid_tcb, 256, task_usb_hid, NULL, 8, "usb_hid");

  rtos_start();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
