/** @file rtos_diag_internal.h
 * @brief Compile-time erased kernel observation points. Caller holds exclusion.
 */
#ifndef RTOS_DIAG_INTERNAL_H_
#define RTOS_DIAG_INTERNAL_H_

#include "rtos_diag.h"

#if RTOS_CONFIG_USE_DIAGNOSTICS
typedef struct rtos_diag_payload {
    const void *object;
    uint32_t value;
    uint32_t detail;
    rtos_status_t result;
    uint32_t peer_id;
} rtos_diag_payload_t;

/** @brief Emit under kernel exclusion; arguments must refer to live inputs.
 *  @param event [in] Transition being observed.
 *  @param operation [in] Explicit wait operation, or NONE for inherited context.
 *  @param task [in] Live subject TCB, or NULL for an unassigned marker.
 *  @param payload [in] Call-local values; copied before returning, never retained.
 *  @return No value. Frozen recording silently ignores new observations.
 */
void rtos_diag_emit_locked(rtos_diag_event_t event, rtos_diag_operation_t operation,
                           const rtos_tcb_t *task, const rtos_diag_payload_t *payload);
/** @brief Permanently freeze before task identities can become ambiguous.
 *  @return No value. Caller holds kernel exclusion; kernel IDs are unchanged.
 */
void rtos_diag_identity_exhausted_locked(void);
/** @brief Record a resumed API return under observation-only exclusion.
 *  @param operation [in] Operation which actually blocked.
 *  @param task [in] Live task whose C stack has resumed.
 *  @param value [in] Actual returned value, or zero for status/void APIs.
 *  @param result [in] Status captured before entering observation exclusion.
 *  @return No value. Does not inspect a historical IPC object.
 */
void rtos_internal_diag_return_record(rtos_diag_operation_t operation,
    const rtos_tcb_t *task, uint32_t value, rtos_status_t result);
/** @brief Record a blocking API's status and preserve that exact result.
 *  @param operation [in] Operation which actually blocked.
 *  @param task [in] Live resumed subject task.
 *  @param result [in] Captured status to record and return.
 *  @return The unchanged result argument.
 */
rtos_status_t rtos_internal_diag_return_status(rtos_diag_operation_t operation,
    const rtos_tcb_t *task, rtos_status_t result);
/** @brief Record a blocking event return and preserve its exact returned bits.
 *  @param operation [in] Operation which actually blocked.
 *  @param task [in] Live resumed subject task.
 *  @param value [in] Actual returned bits.
 *  @param result [in] Internal wake status, independent of nonzero partial bits.
 *  @return The unchanged value argument.
 */
uint32_t rtos_internal_diag_return_value(rtos_diag_operation_t operation,
    const rtos_tcb_t *task, uint32_t value, rtos_status_t result);
#define RTOS_DIAG_RECORD(event, operation, task, object, value, detail, result, peer_id) \
    rtos_diag_emit_locked((event), (operation), (task), \
        &(const rtos_diag_payload_t){(object), (value), (detail), (result), (peer_id)})
#define RTOS_DIAG_IDENTITY_EXHAUSTED() rtos_diag_identity_exhausted_locked()
#else
#define RTOS_DIAG_RECORD(event, operation, task, object, value, detail, result, peer_id) \
    ((void)0)
#define RTOS_DIAG_IDENTITY_EXHAUSTED() ((void)0)
#endif

#endif /* RTOS_DIAG_INTERNAL_H_ */
