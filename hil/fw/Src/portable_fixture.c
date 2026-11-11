/** @file portable_fixture.c
 * @brief Dispatch one portable test from the F446 audit task.
 */

#include "audit_regressions.h"

#include <stdio.h>

#include "rtos.h"

#define HIL_ENTRY_STRING_IMPL(symbol) #symbol
#define HIL_ENTRY_STRING(symbol) HIL_ENTRY_STRING_IMPL(symbol)

void HIL_PORTABLE_ENTRY(void *argument);
#if defined(HIL_PORTABLE_BEFORE_START)
void HIL_PORTABLE_BEFORE_START(void);
#endif

void audit_pre_start(void)
{
#if defined(HIL_PORTABLE_BEFORE_START)
    HIL_PORTABLE_BEFORE_START();
#endif
}

void audit_regressions_task(void *argument)
{
    if (rtos_task_set_priority(NULL, HIL_PORTABLE_PRIORITY) == RTOS_OK) {
        (void)printf("F4HIL|READY|entry=%s|priority=%u\r\n",
                     HIL_ENTRY_STRING(HIL_PORTABLE_ENTRY), (unsigned int)HIL_PORTABLE_PRIORITY);
        HIL_PORTABLE_ENTRY(argument);
    } else {
        (void)printf("F4HIL|FAIL|entry_priority\r\n");
    }
    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}
