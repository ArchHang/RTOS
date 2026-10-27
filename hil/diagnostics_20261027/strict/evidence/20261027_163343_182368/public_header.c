#include "rtos_diag.h"
int diagnostic_public_probe(void)
{
    rtos_diag_info_t info;
    rtos_diag_record_t record;
    rtos_diag_explanation_t explanation;
    (void)rtos_diag_reset();
    (void)rtos_diag_mark(1U, 2U);
    (void)rtos_diag_freeze(RTOS_DIAG_TRIGGER_USER);
    (void)rtos_diag_get_info(&info);
    (void)rtos_diag_read(1U, 1U, &record);
    (void)rtos_diag_explain(1U, 1U, &explanation);
    rtos_diag_fault_freeze(1U, (uintptr_t)0U);
    return 0;
}
