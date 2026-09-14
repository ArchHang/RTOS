#ifndef __AI_APP_H__
#define __AI_APP_H__

/*
 * 自动生成：ai 前向推理接口（int8 动态量化版）
 * 激活 scale 每帧在线计算，无需校准数据。
 */

#include <stdint.h>

#define AI_INPUT_SIZE  2044
#define AI_OUTPUT_SIZE 4
#define AI_INPUT_ROW   1
#define AI_INPUT_COL   2044
#define AI_INPUT_CH    1

#define ai_INPUT_SIZE  AI_INPUT_SIZE
#define ai_OUTPUT_SIZE AI_OUTPUT_SIZE

#ifdef __cplusplus
extern "C" {
#endif

/* 输入固定量化：float 输入按 1/255、zp=0 量化（图像/特征范围 [0,1]） */
int ai_forward_u8(const uint8_t *input, float *logits, uint32_t *class_id);
int ai_forward(const float *input, float *logits, uint32_t *class_id);

#ifdef __cplusplus
}
#endif

#endif
