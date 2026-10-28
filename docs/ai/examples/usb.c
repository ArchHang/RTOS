/**
 * @file usb.c
 * @brief CDC byte-count handling after target-specific device initialization.
 */
#include "ai_examples.h"

int32_t ai_example_cdc_write(uint8_t busid, const uint8_t *data, uint32_t bytes)
{
    if ((data == NULL) || (bytes == 0U) || (bytes > (uint32_t)INT32_MAX)) {
        return 0;
    }
#if RTOS_CONFIG_USE_USB && RTOS_CONFIG_USB_DEVICE_MODE
    /* Initialization, clocks, pins, IRQ priority and real host are prerequisites.
     * A short/zero result does not identify a unique failure cause. */
    return (int32_t)rtos_usb_cdc_write(busid, data, bytes, 20U);
#else
    (void)busid;
    return 0;
#endif
}
