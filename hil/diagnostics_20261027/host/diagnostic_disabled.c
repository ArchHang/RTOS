/** @file diagnostic_disabled.c
 * @brief Disabled public API remains inert and never touches output arguments.
 */
#include "host_port.h"

#include <stdio.h>
#include <string.h>

#include "../../../RTOS/Services/rtos_diag.c"

static uint32_t checks;
static uint32_t failures;

static void expect(const char *name, uint32_t condition)
{
    checks++;
    if (condition == 0U) {
        failures++;
        (void)printf("DISABLED|FAIL|id=%lu name=%s\n", (unsigned long)checks, name);
    }
}

int main(void)
{
    rtos_diag_info_t info;
    rtos_diag_info_t before_info;
    rtos_diag_record_t record;
    rtos_diag_record_t before_record;
    rtos_diag_explanation_t explanation;
    rtos_diag_explanation_t before_explanation;

    memset(&info, 0xA5, sizeof(info));
    before_info = info;
    memset(&record, 0x5A, sizeof(record));
    before_record = record;
    memset(&explanation, 0x3C, sizeof(explanation));
    before_explanation = explanation;
    expect("reset", rtos_diag_reset() == RTOS_ERR_DISABLED);
    expect("freeze", rtos_diag_freeze(1U) == RTOS_ERR_DISABLED);
    expect("mark", rtos_diag_mark(1U, 2U) == RTOS_ERR_DISABLED);
    expect("info", rtos_diag_get_info(&info) == RTOS_ERR_DISABLED);
    expect("read", rtos_diag_read(1U, 1U, &record) == RTOS_ERR_DISABLED);
    expect("explain", rtos_diag_explain(1U, 1U, &explanation) == RTOS_ERR_DISABLED);
    expect("info_unchanged", memcmp(&info, &before_info, sizeof(info)) == 0);
    expect("record_unchanged", memcmp(&record, &before_record, sizeof(record)) == 0);
    expect("explanation_unchanged", memcmp(&explanation, &before_explanation,
        sizeof(explanation)) == 0);
    expect("info_null", rtos_diag_get_info(NULL) == RTOS_ERR_DISABLED);
    expect("read_null", rtos_diag_read(0U, 0U, NULL) == RTOS_ERR_DISABLED);
    expect("explain_null", rtos_diag_explain(0U, 0U, NULL) == RTOS_ERR_DISABLED);
    /* Deliberately invalid integer addresses are never dereferenced. */
    rtos_diag_fault_freeze(UINT32_MAX, (uintptr_t)1U);
    expect("fault_stub_safe", 1U);
    (void)printf("DISABLED|SUMMARY|checks=%lu failures=%lu sanitizer=none\n",
        (unsigned long)checks, (unsigned long)failures);
    (void)printf("DISABLED|DONE|status=%lu\n", failures == 0U ? 0UL : 1UL);
    return failures == 0U ? 0 : 1;
}
