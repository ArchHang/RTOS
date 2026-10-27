/**
 * @file    rtos_usb_msc.c
 * @brief   CherryUSB MSC 静态 RAM 盘与扇区统计。
 *
 * @details 独立实现介质容量、读写和观测接口; 不依赖 CDC/HID 的设备静态状态。
 *          扇区大小为 512 字节, 介质容量由 RTOS_CONFIG_USB_MSC_RAM_DISK_SIZE 配置。
 */
#include "rtos_usb.h"

#if RTOS_CONFIG_USE_USB

#include <string.h>

#if RTOS_CONFIG_USB_USE_MSC
#include "usbd_core.h" /* 为 usbd_msc.h 提供 CherryUSB 的打包类型宏。 */
#include "usbd_msc.h"
#endif

/*============================================================================*/
/*====================  MSC 大容量存储封装(RAM 盘)  ==========================*/
/*============================================================================*/
/* CherryUSB 的 usbd_msc.c 通过以下三个弱约定回调访问存储介质:
 *   usbd_msc_get_cap       → 上报容量(扇区数/扇区大小)
 *   usbd_msc_sector_read   → 读扇区(运行于 usbd_msc 线程)
 *   usbd_msc_sector_write  → 写扇区(运行于 usbd_msc 线程)
 * 此处用静态 RAM 数组模拟 U 盘。用户接入 SPI Flash/SD 卡时, 只需将这三个
 * 函数内部的 memcpy 替换为实际介质的读写驱动即可。 */

#if RTOS_CONFIG_USB_USE_MSC

#define MSC_BLOCK_SIZE (512U)
#define MSC_BLOCK_NUM (RTOS_CONFIG_USB_MSC_RAM_DISK_SIZE / MSC_BLOCK_SIZE)

/* 编译期检查: RAM 盘大小必须是 512 的整数倍且非空 */
#if (MSC_BLOCK_NUM * MSC_BLOCK_SIZE) != RTOS_CONFIG_USB_MSC_RAM_DISK_SIZE
#error "RTOS_CONFIG_USB_MSC_RAM_DISK_SIZE must be a multiple of 512"
#endif

static uint8_t s_msc_ram_disk[RTOS_CONFIG_USB_MSC_RAM_DISK_SIZE]
    __attribute__((aligned(4)));

/* 扇区读写统计(观测用, MSC 任务周期性打印) */
static volatile uint32_t s_msc_read_sectors;
static volatile uint32_t s_msc_write_sectors;

void usbd_msc_get_cap(uint8_t busid, uint8_t lun, uint32_t *block_num, uint32_t *block_size)
{
    (void)busid;
    (void)lun;
    *block_num = MSC_BLOCK_NUM;
    *block_size = MSC_BLOCK_SIZE;
}

int usbd_msc_sector_read(uint8_t busid, uint8_t lun, uint32_t sector, uint8_t *buffer,
                         uint32_t length)
{
    (void)busid;
    (void)lun;
    if ((sector >= MSC_BLOCK_NUM) || (buffer == NULL) ||
        ((length % MSC_BLOCK_SIZE) != 0U) ||
        ((sector + length / MSC_BLOCK_SIZE) > MSC_BLOCK_NUM)) {
        return -1;
    }
    memcpy(buffer, &s_msc_ram_disk[sector * MSC_BLOCK_SIZE], length);
    s_msc_read_sectors += length / MSC_BLOCK_SIZE;
    return 0;
}

int usbd_msc_sector_write(uint8_t busid, uint8_t lun, uint32_t sector, uint8_t *buffer,
                          uint32_t length)
{
#if RTOS_CONFIG_USB_MSC_READONLY
    (void)busid;
    (void)lun;
    (void)sector;
    (void)buffer;
    (void)length;
    return -1;
#else
    (void)busid;
    (void)lun;
    if ((sector >= MSC_BLOCK_NUM) || (buffer == NULL) ||
        ((length % MSC_BLOCK_SIZE) != 0U) ||
        ((sector + length / MSC_BLOCK_SIZE) > MSC_BLOCK_NUM)) {
        return -1;
    }
    memcpy(&s_msc_ram_disk[sector * MSC_BLOCK_SIZE], buffer, length);
    s_msc_write_sectors += length / MSC_BLOCK_SIZE;
    return 0;
#endif
}

void rtos_usb_msc_get_stats(uint8_t busid, uint32_t *reads, uint32_t *writes)
{
    (void)busid;
    if (reads != NULL) {
        *reads = s_msc_read_sectors;
    }
    if (writes != NULL) {
        *writes = s_msc_write_sectors;
    }
}

#else /* !RTOS_CONFIG_USB_USE_MSC */

void rtos_usb_msc_get_stats(uint8_t busid, uint32_t *reads, uint32_t *writes)
{
    (void)busid;
    if (reads != NULL) {
        *reads = 0U;
    }
    if (writes != NULL) {
        *writes = 0U;
    }
}

#endif /* RTOS_CONFIG_USB_USE_MSC */

#endif /* RTOS_CONFIG_USE_USB */
