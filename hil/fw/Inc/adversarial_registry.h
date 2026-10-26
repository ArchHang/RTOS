#ifndef ADVERSARIAL_REGISTRY_H_
#define ADVERSARIAL_REGISTRY_H_

#include <stdint.h>

/** @brief Observe the existing internal mutex registry for HIL assertions.
 *  @return Number of initialized mutexes; caller holds kernel exclusion.
 *  @note This test declaration does not add a production API or implementation. */
uint32_t rtos_internal_mutex_registry_count(void);

#endif /* ADVERSARIAL_REGISTRY_H_ */
