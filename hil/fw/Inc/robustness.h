#ifndef F446_HIL_FW_ROBUSTNESS_H_
#define F446_HIL_FW_ROBUSTNESS_H_

#include <stdbool.h>

/**
 * @brief 记录一条具名回归结果。
 * @param[in] name 在本次执行中唯一的 ASCII 用例名。
 * @param[in] is_ok 用例是否通过。
 */
void robustness_check(const char *name, bool is_ok);

#endif /* F446_HIL_FW_ROBUSTNESS_H_ */
