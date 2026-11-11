/**
 * @file robustness_services.h
 * @brief Software service checks in the F446 robustness firmware.
 */
#ifndef HIL_FW_INC_ROBUSTNESS_SERVICES_H_
#define HIL_FW_INC_ROBUSTNESS_SERVICES_H_

/** @brief Run USB OSAL and enabled performance checks from the controller task. */
void robustness_services_run(void);

#endif /* HIL_FW_INC_ROBUSTNESS_SERVICES_H_ */
