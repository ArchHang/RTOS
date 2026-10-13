/**
 * @file production_stress.h
 * @brief Bounded industrial acquisition and control workload for board validation.
 *
 * All sensor values are synthetic. Packets are copied by value; a source advances
 * its zero-based sequence only after a successful queue send. No heap allocation
 * is performed by the workload. The caller creates the controller task.
 */
#ifndef F446_RTOS_HIL_FW_INC_PRODUCTION_STRESS_H_
#define F446_RTOS_HIL_FW_INC_PRODUCTION_STRESS_H_

#include <stdint.h>

typedef struct production_packet {
    uint32_t source;
    uint32_t sequence;
    uint32_t payload;
    uint32_t checksum;
} production_packet_t;

/**
 * @brief Compute the deterministic synthetic value used by tasks and ISRs.
 * @param source [in] Source identifier in the range 0 through 5.
 * @param sequence [in] Zero-based successful-send sequence.
 * @param seed [in] Reproducible workload seed.
 * @return Synthetic value; unsigned arithmetic has defined modulo semantics.
 */
uint32_t production_stress_payload(uint32_t source, uint32_t sequence, uint32_t seed);

/**
 * @brief Compute the packet integrity check without reading its checksum field.
 * @param packet [in] Non-NULL packet with source, sequence and payload populated.
 * @return Integrity check, or zero when packet is NULL.
 */
uint32_t production_stress_checksum(const production_packet_t *packet);

/**
 * @brief Run a bounded three-phase, sixteen-worker RTOS stress scenario.
 * @param arg [in] Unused; caller may pass NULL. Requires a running scheduler.
 * @return Does not return: prints STRESS|DONE and parks the controller task.
 */
void production_stress_task(void *arg);

#endif /* F446_RTOS_HIL_FW_INC_PRODUCTION_STRESS_H_ */
