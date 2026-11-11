/**
 * @file usb_cdc_cursor_regression.c
 * @brief Verify CDC receive bytes across 32-bit and physical ring boundaries.
 */
#include "audit_regressions.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtos.h"
#include "rtos_usb.h"

static uint32_t g_passed;
static uint32_t g_failed;

void rtos_usb_cdc_cursor_test_seed(uint32_t total);
bool rtos_usb_cdc_cursor_test_push(const uint8_t *data, uint32_t count);
int __wrap_usbd_ep_start_read(uint8_t busid, uint8_t ep, uint8_t *data, uint32_t length);

int __wrap_usbd_ep_start_read(uint8_t busid, uint8_t ep, uint8_t *data, uint32_t length)
{
    (void)busid;
    (void)ep;
    (void)data;
    (void)length;
    return 0;
}

static void record_result(const char *name, bool passed)
{
    if (passed) {
        ++g_passed;
    } else {
        ++g_failed;
    }
    (void)printf("CDCWR|%s|%s\r\n", name, passed ? "PASS" : "FAIL");
}

static void check_split_reads(uint32_t initial, const char *name)
{
    static const uint8_t input[] = {0x41U, 0x52U, 0x63U, 0x74U, 0x85U, 0x96U, 0xA7U, 0xB8U};
    uint8_t output[sizeof(input)] = {0};
    rtos_usb_cdc_cursor_test_seed(initial);
    const bool pushed = rtos_usb_cdc_cursor_test_push(input, sizeof(input));
    const bool available = rtos_usb_cdc_available(0U) == sizeof(input);
    const int first = rtos_usb_cdc_read(0U, output, 4U, 0U);
    const int second = rtos_usb_cdc_read(0U, output + 4U, 4U, 0U);
    record_result(name, pushed && available && (first == 4) && (second == 4) &&
                  (memcmp(input, output, sizeof(input)) == 0) &&
                  (rtos_usb_cdc_available(0U) == 0U));
    (void)printf("CDCWR|BYTES|%s|second=%02X%02X%02X%02X\r\n", name,
                 (unsigned int)output[4], (unsigned int)output[5],
                 (unsigned int)output[6], (unsigned int)output[7]);
}

static bool fill_or_drain(bool fill)
{
    uint8_t chunk[64];
    const uint32_t capacity = RTOS_CONFIG_USB_CDC_RX_BUF_SIZE - 1U;
    uint32_t offset = 0U;

    while (offset < capacity) {
        const uint32_t left = capacity - offset;
        const uint32_t count = (left < sizeof(chunk)) ? left : sizeof(chunk);
        if (fill) {
            for (uint32_t i = 0U; i < count; ++i) {
                chunk[i] = (uint8_t)((offset + i) * 73U + 29U);
            }
            if (!rtos_usb_cdc_cursor_test_push(chunk, count)) {
                return false;
            }
        } else {
            if (rtos_usb_cdc_read(0U, chunk, count, 0U) != (int)count) {
                return false;
            }
            for (uint32_t i = 0U; i < count; ++i) {
                if (chunk[i] != (uint8_t)((offset + i) * 73U + 29U)) {
                    return false;
                }
            }
        }
        offset += count;
    }
    return true;
}

void audit_pre_start(void)
{
}

void audit_regressions_task(void *argument)
{
    (void)argument;
    const uint8_t extra = 0xEEU;
    const uint32_t capacity = RTOS_CONFIG_USB_CDC_RX_BUF_SIZE;

    (void)printf("CDCWR|BEGIN|capacity=%lu\r\n", (unsigned long)capacity);
    check_split_reads(UINT32_MAX - 3U, "uint32_wrap");
    check_split_reads(capacity - 3U, "array_wrap");
    rtos_usb_cdc_cursor_test_seed(capacity - 3U);
    const bool filled = fill_or_drain(true);
    const bool rejected = rtos_usb_cdc_cursor_test_push(&extra, 1U) &&
        (rtos_usb_cdc_available(0U) == capacity - 1U);
    record_result("full_and_ordered", filled && rejected && fill_or_drain(false) &&
                  (rtos_usb_cdc_available(0U) == 0U));
    const bool refilled = rtos_usb_cdc_cursor_test_push(&extra, 1U) &&
        (rtos_usb_cdc_available(0U) == 1U);
    rtos_usb_cdc_flush_rx(0U);
    record_result("flush_after_wrap", refilled && (rtos_usb_cdc_available(0U) == 0U));
    (void)printf("CDCWR|DONE|pass=%lu|fail=%lu\r\n", (unsigned long)g_passed,
                 (unsigned long)g_failed);
    for (;;) {
        rtos_task_delay(1000U);
    }
}
