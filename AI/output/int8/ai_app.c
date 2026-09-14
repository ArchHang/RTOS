#include "ai_app.h"
#include "ai_param.h"
#include "conv.h"
#include <string.h>

/* 自动生成：int8 动态量化模型结构与运行代码 */

static uint8_t q_in[2044];
static uint8_t cbuf_0[4 * 2040];
static uint8_t cbuf_1[8 * 1016];
static uint8_t cbuf_2[16 * 504];
static float fbuf_0[32];
static uint8_t qbuf_0[32];
static float logits_buf[4];

static int ai_core(const uint8_t *q_in, float *logits, uint32_t *class_id)
{
  uint32_t i;
  int ret;
  float act_scale;
  float fmin, fmax, fscale;
  int fzp;

  if (!q_in) return CNN_POINTER_NULL;

  /* 层 0: Conv1d(1 -> 4, k=5) + BN（动态量化） */
  ret = conv2d_int8_dynamic(q_in, 1, 2044, 1, ai_l0_weight, 1, 5, 1, 0, 4,
                      ai_l0_wscale, ai_l0_bias,
                      0.00392156863f, 0, &act_scale, cbuf_0);
  if (ret != CNN_NORMAL) return ret;

  /* 层 3: MaxPool(k=2, s=2)（uint8 域取 max） */
  for (i = 0; i < 4; i++) {
    maxpool_u8(cbuf_0 + i * 2040, 1, 2040, cbuf_0 + i * 1020, 1, 2, 2);
  }

  /* 层 4: Conv1d(4 -> 8, k=5) + BN（动态量化） */
  ret = conv2d_int8_dynamic(cbuf_0, 1, 1020, 4, ai_l1_weight, 1, 5, 1, 0, 8,
                      ai_l1_wscale, ai_l1_bias,
                      act_scale, 0, &act_scale, cbuf_1);
  if (ret != CNN_NORMAL) return ret;

  /* 层 7: MaxPool(k=2, s=2)（uint8 域取 max） */
  for (i = 0; i < 8; i++) {
    maxpool_u8(cbuf_1 + i * 1016, 1, 1016, cbuf_1 + i * 508, 1, 2, 2);
  }

  /* 层 8: Conv1d(8 -> 16, k=5) + BN（动态量化） */
  ret = conv2d_int8_dynamic(cbuf_1, 1, 508, 8, ai_l2_weight, 1, 5, 1, 0, 16,
                      ai_l2_wscale, ai_l2_bias,
                      act_scale, 0, &act_scale, cbuf_2);
  if (ret != CNN_NORMAL) return ret;

  /* 层 11: MaxPool(k=2, s=2)（uint8 域取 max） */
  for (i = 0; i < 16; i++) {
    maxpool_u8(cbuf_2 + i * 504, 1, 504, cbuf_2 + i * 252, 1, 2, 2);
  }

  /* 层 12: Flatten（缓冲区连续，无需复制） */

  /* 层 13: Linear(4032 -> 32)（动态非对称量化） */
  ret = linear_int8(cbuf_2, 4032, ai_l3_weight, ai_l3_wscale, ai_l3_bias, 32,
                     act_scale, 0, fbuf_0);
  if (ret != CNN_NORMAL) return ret;
  fmin = fbuf_0[0]; fmax = fbuf_0[0];
  for (i = 1; i < 32; i++) {
    if (fbuf_0[i] < fmin) fmin = fbuf_0[i];
    if (fbuf_0[i] > fmax) fmax = fbuf_0[i];
  }
  fscale = (fmax - fmin) / 255.0f;
  if (fscale < 1e-9f) fscale = 1e-9f;
  fzp = (int)lrintf(-fmin / fscale);
  fzp = fzp < 0 ? 0 : (fzp > 255 ? 255 : fzp);
  quantize_u8(fbuf_0, 32, fscale, fzp, qbuf_0);

  /* 层 14: Linear(32 -> 4)（输出 logits） */
  ret = linear_int8(qbuf_0, 32, ai_l4_weight, ai_l4_wscale, ai_l4_bias, 4,
                     fscale, fzp, logits_buf);
  if (ret != CNN_NORMAL) return ret;

  /* 输出 */
  if (logits) memcpy(logits, logits_buf, 4 * sizeof(float));
  if (class_id) *class_id = argmax(logits_buf, 4, NULL);
  return CNN_NORMAL;
}

int ai_forward_u8(const uint8_t *input, float *logits, uint32_t *class_id)
{
    if (!input) return CNN_POINTER_NULL;
    return ai_core(input, logits, class_id);
}

int ai_forward(const float *input, float *logits, uint32_t *class_id)
{
    if (!input) return CNN_POINTER_NULL;
    quantize_u8(input, AI_INPUT_SIZE, 0.00392156863f, 0, q_in);
    return ai_core(q_in, logits, class_id);
}
