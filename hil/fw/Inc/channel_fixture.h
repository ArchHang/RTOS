/**
 * @file channel_fixture.h
 * @brief Shared, test-only channel result and payload helpers.
 */
#ifndef HIL_FW_CHANNEL_FIXTURE_H_
#define HIL_FW_CHANNEL_FIXTURE_H_

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx_hal.h"
#include "rtos.h"
#include "rtos_channel.h"

/**
 * @brief Record one named test predicate; logging is task-context only.
 * @param name [in] Stable machine-readable check name.
 * @param condition [in] Nonzero means pass.
 */
void ch_expect(const char *name, uint32_t condition);
/**
 * @brief Print the final oracle and park the audit controller indefinitely.
 * @param mode [in] Fixture mode name; this function never returns.
 */
void ch_finish(const char *mode);
/**
 * @brief Generate the deterministic byte at offset in a seeded message.
 * @param seed [in] Full 32-bit source/sequence seed.
 * @param offset [in] Byte position in the message.
 * @return Deterministic pattern byte.
 */
uint8_t ch_pattern(uint32_t seed, uint32_t offset);
/**
 * @brief Fill caller-owned storage in place without allocating or copying a payload.
 * @param buffer [out] Writable range of at least length bytes.
 * @param length [in] Number of bytes to generate.
 * @param seed [in] Pattern seed.
 */
void ch_fill(void *buffer, uint32_t length, uint32_t seed);
/**
 * @brief Verify every byte in place and retain its checksum for the final oracle.
 * @param buffer [in] Readable range of at least length bytes.
 * @param length [in] Number of bytes to verify.
 * @param seed [in] Expected pattern seed.
 * @return 1 for an exact match, otherwise 0.
 */
uint32_t ch_verify(const void *buffer, uint32_t length, uint32_t seed);
/**
 * @brief Check that every slot is accounted for in exactly one state count.
 * @param stats [in] Successful channel statistics snapshot.
 * @return 1 if state counts sum to capacity, otherwise 0.
 */
uint32_t ch_conserved(const rtos_channel_stats_t *stats);
/**
 * @brief Wait at most 100 ticks for a worker to block on a specified object.
 * @param task [in] Live worker control block; its scheduler fields are read under a lock.
 * @param object [in] Expected blocked_on object identity.
 * @return 1 if the worker was observed blocked, otherwise 0.
 */
uint32_t ch_wait_blocked(rtos_tcb_t *task, const void *object);

#endif /* HIL_FW_CHANNEL_FIXTURE_H_ */
