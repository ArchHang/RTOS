/*
 * Copyright (c) 2025, VeriSilicon Holdings Co., Ltd. All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file conv.c
 * @brief 可移植 CNN 张量算子库实现（fp32 与 int8 量化推理）
 *
 * 文件组织：
 *   1. fp32 算子：卷积+BN、LeakyReLU、Softmax、argmax、全连接、池化、Sigmoid
 *   2. int8/uint8 量化算子：量化、静态量化卷积、动态量化卷积、量化池化、
 *      量化全连接
 *
 * 数值约定：
 *   - int8 量化链在整数域（int32）完成累加，最后才反量化为 float，
 *     保证与 Python 端 int32 精确复现一致；
 *   - 量化舍入统一使用 lrintf（round-half-even）；
 *   - 卷积累加顺序固定为 channel -> kernel_row -> kernel_col，
 *     浮点路径与朴素参考实现保持逐位一致。
 *   - 动态量化（无需校准）的激活重量化在整数域完成：逐点只有
 *     int32 累加 + int64 乘加 + 移位，不产生逐点浮点转换；
 *     每通道仅一次浮点运算用于计算量化系数（见 DYNAMIC_QUANT_FRAC_BITS）。
 */

#include "conv.h"

#define BN_EPS (1e-5f)

/**
 * 动态量化定点重量化的二进制小数位宽。
 *
 * 第二遍量化时：
 *   q = round_half_even((acc * A_fix + B_fix) / 2^K)
 * 其中 A_fix/B_fix 为每通道一次计算好的定点系数：
 *   A_fix = round((mul / scale) * 2^K)
 *   B_fix = round((bias / scale) * 2^K)
 * 取 K=28 时，对本工程卷积层（acc 上界约 1.3e6、|bias|<=1、
 * scale 下限 2.55e-7）乘积不超过 int64 安全范围，且量化误差
 * 远小于一个量化码（实测全测试集 99.998% 与浮点参考逐位一致，
 * 预测类别 100% 一致）。
 */
#define DYNAMIC_QUANT_FRAC_BITS 28

/**
 * @brief int64 定点数做 round-half-even 右移。
 *
 * 与 lrintf 的舍入规则一致（四舍五入到偶数），用于替代逐点浮点除法。
 */
static inline int32_t round_half_even_shift64(int64_t v, int bits) {
    int64_t a = v < 0 ? -v : v;
    int64_t r = (a + (1LL << (bits - 1)) + ((a >> bits) & 1LL)) >> bits;
    return v < 0 ? -(int32_t)r : (int32_t)r;
}

/**
 * @brief 把浮点系数安全转换为 int64 定点系数（防止极端参数下溢出）。
 */
static inline int64_t fixed_coeff(double x) {
    const double limit = 9.0e18;
    if (x > limit) {
        return INT64_MAX;
    }
    if (x < -limit) {
        return INT64_MIN;
    }
    return (int64_t)llrint(x);
}

/**
 * @brief 计算卷积/池化输出长度（valid 输出）。
 *
 * 调用方需保证 raw_len + 2*pad_len >= filter_len，否则结果无意义。
 */
uint32_t cal_conv_out_len(uint32_t raw_len, uint32_t pad_len, uint32_t filter_len,
                          uint32_t stride) {
    return (raw_len + 2 * pad_len - filter_len) / stride + 1;
}

/* =============================== fp32 算子 =============================== */

/**
 * @brief fp32 卷积 + BN（无卷积 bias），支持 1D/2D、zero-padding、任意 stride。
 *
 * 内部把 BN 折叠为每通道 scale/shift：
 *   scale = gamma / sqrt(var + BN_EPS)
 *   shift = beta - mean * scale
 * 推理公式：y = acc * scale + shift。
 *
 * 性能路径：
 *   - 1D（row=1、核 row=1）：
 *       * pad=0、stride=1 时使用 FIR 散射形式（内层对整行输出做连续累加，
 *         读写连续，编译器可向量化为 AVX2 FMA）；
 *       * pad=0、stride>1 时使用输出驻留循环；
 *       * pad>0 时使用虚拟 padding（窗口完全在界内走无分支快速路径，
 *         边界像素逐 tap 判断越界输入按 0 参与累加）。
 *   - 2D：指针游走 + 内层连续读取，累加顺序与朴素实现一致。
 *
 * 注意：输入输出 buffer 不允许重叠。
 *
 * @param input_feat  [in]  输入特征图；1D 时 row=1
 * @param param       [in]  卷积配置
 * @param output_feat [out] 输出特征图；data 由调用方提供，容量至少
 *                          filter_num * out_row * out_col
 * @return CNN_NORMAL / CNN_POINTER_NULL / CNN_DATA_EXCEPTION
 */
int conv2d_bn_no_bias(Conv2dData* input_feat, Conv2dConfig* param, Conv2dData* output_feat) {
    const BatchNorm2d* bn = NULL;
    const Conv2dFilter* filter = NULL;
    const float* restrict in = NULL;
    const float* restrict w = NULL;
    float* restrict out = NULL;
    uint32_t in_row = 0, in_col = 0;
    uint32_t f_row = 0, f_col = 0, f_chan = 0, f_num = 0;
    uint32_t out_row = 0, out_col = 0, out_chan = 0;
    uint32_t i = 0, j = 0, k = 0, ii = 0, jj = 0, kk = 0;

    // 指针与关键字段非空检查。
    if (!input_feat || !input_feat->data || !param || !param->bn || !param->bn->mean ||
        !param->bn->var || !param->bn->gamma || !param->bn->beta || !param->filter ||
        !param->filter->data || !output_feat || !output_feat->data) {
        return CNN_POINTER_NULL;
    }

    bn = param->bn;
    filter = param->filter;

    // 形状合法性检查：stride>=1、通道匹配、padding 不溢出、核不超界、BN 长度匹配。
    // 注意溢出保护：pad > (UINT32_MAX - row)/2 时 2*pad+row 可能溢出，需先判断。
    if (param->stride == 0 || input_feat->channel != filter->channel ||
        param->pad > (UINT32_MAX - input_feat->row) / 2 ||
        param->pad > (UINT32_MAX - input_feat->col) / 2 || filter->filter_num != bn->size ||
        filter->row > 2 * param->pad + input_feat->row ||
        filter->col > 2 * param->pad + input_feat->col) {
        return CNN_DATA_EXCEPTION;
    }

    in_row = input_feat->row;
    in_col = input_feat->col;
    f_row = filter->row;
    f_col = filter->col;
    f_chan = filter->channel;
    f_num = filter->filter_num;

    // 输出维度：1D 时行维恒为 1（与库内其它算子约定一致）。
    out_row = (in_row == 1) ? 1 : cal_conv_out_len(in_row, param->pad, f_row, param->stride);
    out_col = cal_conv_out_len(in_col, param->pad, f_col, param->stride);
    out_chan = f_num;

    in = input_feat->data;
    w = filter->data;
    out = output_feat->data;

    output_feat->row = out_row;
    output_feat->col = out_col;
    output_feat->channel = out_chan;

    // ------------------------- Fast path 1: 1D 卷积 -------------------------
    // 单行输入 + 单行核，任意 stride。
    // stride=1 时使用 FIR 散射：外层遍历 通道 x 卷积核抽头，内层对整行输出做
    // 连续累加。内层循环读写都连续、权重是标量，编译器可自动向量化（AVX2 FMA）。
    // 每个输出位置的累加顺序仍然是 channel(0..C-1) -> tap(0..K-1)，
    // 与参考实现完全一致（保证逐位一致）。
    if (in_row == 1 && f_row == 1) {
        for (i = 0; i < out_chan; i++) {
            const float* w_i = w + i * (f_chan * f_col);
            float* out_i = out + i * out_col;
            const float scale = bn->gamma[i] / (float)sqrt(bn->var[i] + BN_EPS);
            const float shift = bn->beta[i] - bn->mean[i] * scale;

            if (param->pad == 0) {
                // 无 padding：保持原有最优路径（FIR 散射 / 输出驻留），性能零损失。
                if (param->stride == 1) {
                    // FIR 散射形式：输出先清零，再按 (通道, tap) 顺序累加。
                    for (j = 0; j < out_col; j++) {
                        out_i[j] = 0.0f;
                    }
                    for (ii = 0; ii < f_chan; ii++) {
                        const float* in_ii = in + ii * in_col;
                        const float* w_ii = w_i + ii * f_col;
                        for (kk = 0; kk < f_col; kk++) {
                            const float wv = w_ii[kk];
                            const float* src = in_ii + kk;
                            float* dst = out_i;
                            for (j = 0; j < out_col; j++) {
                                dst[j] += wv * src[j];
                            }
                        }
                    }
                    // BN 折叠：最后统一乘 scale 加 shift。
                    for (j = 0; j < out_col; j++) {
                        out_i[j] = out_i[j] * scale + shift;
                    }
                } else {
                    // stride>1：输出驻留循环，每个输出点独立累加（无重叠写）。
                    for (j = 0; j < out_col; j++) {
                        float acc = 0.0f;
                        for (ii = 0; ii < f_chan; ii++) {
                            const float* in_ii = in + ii * in_col;
                            const float* w_ii = w_i + ii * f_col;
                            for (kk = 0; kk < f_col; kk++) {
                                acc += w_ii[kk] * in_ii[j * param->stride + kk];
                            }
                        }
                        out_i[j] = acc * scale + shift;
                    }
                }
            } else {
                // 虚拟 padding：输出驻留；窗口完全在界内走无分支快速路径，
                // 边界像素逐 tap 判断（越界输入按 0 参与累加）。
                for (j = 0; j < out_col; j++) {
                    const int s = (int)(j * param->stride) - (int)param->pad;
                    const int inside = (s >= 0 && (uint32_t)(s + f_col) <= in_col);
                    float acc = 0.0f;
                    for (ii = 0; ii < f_chan; ii++) {
                        const float* w_ii = w_i + ii * f_col;
                        if (inside) {
                            const float* in_ii = in + ii * in_col + (uint32_t)s;
                            for (kk = 0; kk < f_col; kk++) {
                                acc += w_ii[kk] * in_ii[kk];
                            }
                        } else {
                            const float* in_ii = in + ii * in_col;
                            for (kk = 0; kk < f_col; kk++) {
                                const int p = s + (int)kk;
                                if (p >= 0 && p < (int)in_col) {
                                    acc += w_ii[kk] * in_ii[p];
                                }
                            }
                        }
                    }
                    out_i[j] = acc * scale + shift;
                }
            }
        }

        return CNN_NORMAL;
    }

    // ------------------------- Fast path 2: 通用 2D 卷积 -------------------------
    // 循环顺序与累加顺序（channel -> kernel_row -> kernel_col）与参考实现一致，
    // 但全部改为指针游走：输出平面顺序写入，内层 kernel-col 循环读写连续 float，
    // 编译器可生成紧凑的 load-FMA 序列。
    for (i = 0; i < out_chan; i++) {
        const float* w_i = w + i * (f_chan * f_row * f_col);
        float* out_i = out + i * (out_row * out_col);
        const float scale = bn->gamma[i] / (float)sqrt(bn->var[i] + BN_EPS);
        const float shift = bn->beta[i] - bn->mean[i] * scale;

        if (param->pad == 0) {
            // 无 padding：指针游走，输出连续写入。
            for (j = 0; j < out_row; j++) {
                const float* row_base = in + j * param->stride * in_col;
                for (k = 0; k < out_col; k++) {
                    const float* col_base = row_base + k * param->stride;
                    float acc = 0.0f;
                    for (ii = 0; ii < f_chan; ii++) {
                        const float* in_c = col_base + ii * (in_row * in_col);
                        const float* w_c = w_i + ii * (f_row * f_col);
                        for (jj = 0; jj < f_row; jj++) {
                            const float* in_r = in_c + jj * in_col;
                            const float* w_r = w_c + jj * f_col;
                            for (kk = 0; kk < f_col; kk++) {
                                acc += w_r[kk] * in_r[kk];
                            }
                        }
                    }
                    *out_i++ = acc * scale + shift;
                }
            }
        } else {
            // 虚拟 padding：行/列窗口都完全在界内时走无分支快速路径，
            // 否则逐 tap 判断（越界输入按 0）。
            for (j = 0; j < out_row; j++) {
                const int r0 = (int)(j * param->stride) - (int)param->pad;
                const int r_in = (r0 >= 0 && (uint32_t)(r0 + f_row) <= in_row);
                for (k = 0; k < out_col; k++) {
                    const int c0 = (int)(k * param->stride) - (int)param->pad;
                    const int c_in = (c0 >= 0 && (uint32_t)(c0 + f_col) <= in_col);
                    float acc = 0.0f;
                    if (r_in && c_in) {
                        const float* col_base = in + (uint32_t)r0 * in_col + (uint32_t)c0;
                        for (ii = 0; ii < f_chan; ii++) {
                            const float* in_c = col_base + ii * (in_row * in_col);
                            const float* w_c = w_i + ii * (f_row * f_col);
                            for (jj = 0; jj < f_row; jj++) {
                                const float* in_r = in_c + jj * in_col;
                                const float* w_r = w_c + jj * f_col;
                                for (kk = 0; kk < f_col; kk++) {
                                    acc += w_r[kk] * in_r[kk];
                                }
                            }
                        }
                    } else {
                        // 边界窗口：逐元素检查越界（行越界整行跳过，列越界按 0 处理）。
                        for (ii = 0; ii < f_chan; ii++) {
                            const float* w_c = w_i + ii * (f_row * f_col);
                            for (jj = 0; jj < f_row; jj++) {
                                const int rr = r0 + (int)jj;
                                if (rr < 0 || rr >= (int)in_row) {
                                    continue;
                                }
                                const float* w_r = w_c + jj * f_col;
                                for (kk = 0; kk < f_col; kk++) {
                                    const int cc = c0 + (int)kk;
                                    if (cc >= 0 && cc < (int)in_col) {
                                        acc += w_r[kk] * in[ii * (in_row * in_col) +
                                                            (uint32_t)rr * in_col + (uint32_t)cc];
                                    }
                                }
                            }
                        }
                    }
                    *out_i++ = acc * scale + shift;
                }
            }
        }
    }

    return CNN_NORMAL;
}

/**
 * @brief LeakyReLU 激活。
 *
 * y = x >= 0 ? x : x * neg_slope。
 * 支持 in-place（out == inp）与非 in-place 两种形式。
 *
 * @param neg_slope 负半轴斜率；传 0 即 ReLU
 * @param inp       [in]  输入数据
 * @param inp_size  [in]  输入长度
 * @param out       [out] 输出数据
 * @return CNN_NORMAL / CNN_POINTER_NULL
 */
int leaky_relu(float neg_slope, float* inp, uint32_t inp_size, float* out) {
    uint32_t i = 0;

    if (!inp || !out) {
        return CNN_POINTER_NULL;
    }

    if (inp == out) {
        // 原地版本：直接读写同一 buffer。
        for (; i < inp_size; i++) {
            const float v = inp[i];
            out[i] = (v < 0.0f) ? v * neg_slope : v;
        }
    } else {
        // 非原地版本：用 restrict 指针帮助编译器向量化。
        const float* restrict x = inp;
        float* restrict o = out;
        for (; i < inp_size; i++) {
            const float v = x[i];
            o[i] = (v < 0.0f) ? v * neg_slope : v;
        }
    }

    return CNN_NORMAL;
}

/**
 * @brief Softmax 激活（数值稳定）。
 *
 * 先减最大值再 exp，避免 exp 溢出为 inf/nan，最后归一化使和为 1。
 * 支持 in-place（out == inp）。
 *
 * @param inp  [in]  输入数据
 * @param size [in]  输入长度（>0）
 * @param out  [out] 输出概率
 * @return CNN_NORMAL / CNN_POINTER_NULL
 */
int softmax(const float* inp, uint32_t size, float* out) {
    float maxv, sum;
    uint32_t i;

    if (!inp || !out || size == 0) {
        return CNN_POINTER_NULL;
    }

    // 第一遍找最大值，用于数值稳定。
    maxv = inp[0];
    for (i = 1; i < size; i++) {
        if (inp[i] > maxv) {
            maxv = inp[i];
        }
    }

    // 第二遍计算 exp(x - max) 并求和。
    sum = 0.0f;
    for (i = 0; i < size; i++) {
        out[i] = expf(inp[i] - maxv);
        sum += out[i];
    }

    // 归一化。
    for (i = 0; i < size; i++) {
        out[i] /= sum;
    }

    return CNN_NORMAL;
}

/**
 * @brief argmax：返回最大值下标（并列时返回第一个）。
 *
 * @param inp     [in]  输入数据
 * @param size    [in]  输入长度
 * @param max_val [out] 可选；返回最大值，传 NULL 忽略
 * @return 最大值下标；参数非法时返回 0
 */
uint32_t argmax(const float* inp, uint32_t size, float* max_val) {
    uint32_t i, idx = 0;
    float m;

    if (!inp || size == 0) {
        if (max_val) {
            *max_val = 0.0f;
        }
        return 0;
    }

    m = inp[0];
    for (i = 1; i < size; i++) {
        if (inp[i] > m) {
            m = inp[i];
            idx = i;
        }
    }
    if (max_val) {
        *max_val = m;
    }
    return idx;
}

/* ============================= 量化算子 ============================= */

/**
 * @brief float -> uint8 非对称量化。
 *
 * q = clamp(round(x / scale) + zp, 0, 255)
 *
 * 使用 lrintf（round-half-even）与 C 端其余量化算子保持一致；
 * 该舍入规则与 Python numpy round 默认行为一致，便于跨端对拍。
 * 参数非法（空指针 / n==0 / scale<=0）时直接返回，不写 out。
 *
 * @param in    [in]  输入 float 数组
 * @param n     [in]  元素个数
 * @param scale [in]  量化步长（>0）
 * @param zp    [in]  零点
 * @param out   [out] 输出 uint8 数组
 */
void quantize_u8(const float* in, uint32_t n, float scale, int zp, uint8_t* out) {
    uint32_t i;
    if (!in || !out || n == 0 || scale <= 0.0f) {
        return;
    }
    for (i = 0; i < n; i++) {
        int q = (int)lrintf(in[i] / scale) + zp;
        out[i] = (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
    }
}

/**
 * @brief int8 1D 卷积 + ReLU + 输出量化（静态量化）。
 *
 * 整数域累加（int32）-> 反量化 -> ReLU -> 量化，与 Python int32 复现一致。
 * 溢出上界：单点累加项 127*255，fc 最长 4032 项约 1.31e8 < INT32_MAX，安全。
 *
 * @param in             [in]  输入 uint8 特征，布局 [in_chan][in_col]
 * @param in_col         [in]  输入宽度
 * @param in_chan        [in]  输入通道数
 * @param w              [in]  int8 权重，布局 [out_chan][in_chan][k]
 * @param k              [in]  核宽
 * @param stride         [in]  步长
 * @param pad            [in]  每侧 zero-padding 宽度（虚拟 padding）
 * @param out_chan       [in]  输出通道数
 * @param w_scale        [in]  per-channel 权重步长
 * @param bias           [in]  BN 折叠后的 float 偏置
 * @param in_act_scale   [in]  输入激活步长
 * @param in_act_zp      [in]  输入激活零点（非对称量化时需先减零点）
 * @param out_act_scale  [in]  输出激活步长
 * @param out_act_zp     [in]  输出激活零点
 * @param out            [out] 输出 uint8，容量 out_chan * out_col
 * @return CNN_NORMAL / CNN_POINTER_NULL / CNN_DATA_EXCEPTION
 */
int conv1d_int8(const uint8_t* in, uint32_t in_col, uint32_t in_chan, const int8_t* w, uint32_t k,
                uint32_t stride, uint32_t pad, uint32_t out_chan, const float* w_scale,
                const float* bias, float in_act_scale, int in_act_zp, float out_act_scale,
                int out_act_zp, uint8_t* out) {
    uint32_t out_col, oc, j, ic, kk;

    if (!in || !w || !w_scale || !bias || !out) {
        return CNN_POINTER_NULL;
    }
    if (k == 0 || stride == 0 || pad > (UINT32_MAX - in_col) / 2 || k > in_col + 2 * pad ||
        in_chan == 0 || out_chan == 0) {
        return CNN_DATA_EXCEPTION;
    }
    out_col = (in_col + 2 * pad - k) / stride + 1;

    for (oc = 0; oc < out_chan; oc++) {
        const int8_t* wrow = w + oc * (in_chan * k);
        // 权重步长与输入激活步长合并为一个乘法因子。
        const float mul = w_scale[oc] * in_act_scale;
        const float b = bias[oc];
        uint8_t* o = out + oc * out_col;

        for (j = 0; j < out_col; j++) {
            const int s = (int)(j * stride) - (int)pad;
            const int inside = (s >= 0 && (uint32_t)(s + k) <= in_col);
            int32_t acc = 0;
            for (ic = 0; ic < in_chan; ic++) {
                const int8_t* wp = wrow + ic * k;
                if (inside) {
                    // 窗口完全在界内：无分支快速路径。
                    const uint8_t* ip = in + ic * in_col + (uint32_t)s;
                    for (kk = 0; kk < k; kk++) {
                        acc += (int32_t)wp[kk] * ((int32_t)ip[kk] - in_act_zp);
                    }
                } else {
                    // 边界窗口：逐 tap 判断，越界输入按 0 处理。
                    const uint8_t* ip = in + ic * in_col;
                    for (kk = 0; kk < k; kk++) {
                        const int p = s + (int)kk;
                        if (p >= 0 && p < (int)in_col) {
                            acc += (int32_t)wp[kk] * ((int32_t)ip[p] - in_act_zp);
                        }
                    }
                }
            }
            {
                // 反量化 -> ReLU -> 输出量化。
                float y = (float)acc * mul + b;
                int q;
                if (y < 0.0f) {
                    y = 0.0f;
                }
                q = (int)lrintf(y / out_act_scale) + out_act_zp;
                o[j] = (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
            }
        }
    }
    return CNN_NORMAL;
}

/**
 * @brief int8 卷积统一入口 + ReLU + 输出量化（1D/2D 自动分派）。
 *
 * @param in             [in]  输入 uint8 特征
 * @param in_row         [in]  输入行数（1D 时为 1）
 * @param in_col         [in]  输入列数
 * @param in_chan        [in]  输入通道数
 * @param w              [in]  int8 权重
 * @param k_row          [in]  核高（1D 时为 1）
 * @param k_col          [in]  核宽
 * @param stride         [in]  行列共用步长
 * @param pad            [in]  每侧 zero-padding 宽度
 * @param out_chan       [in]  输出通道数
 * @param w_scale        [in]  per-channel 权重步长
 * @param bias           [in]  BN 折叠后的 float 偏置
 * @param in_act_scale   [in]  输入激活步长
 * @param in_act_zp      [in]  输入激活零点
 * @param out_act_scale  [in]  输出激活步长
 * @param out_act_zp     [in]  输出激活零点
 * @param out            [out] 输出 uint8
 * @return CNN_NORMAL / CNN_POINTER_NULL / CNN_DATA_EXCEPTION
 */
int conv2d_int8(const uint8_t* in, uint32_t in_row, uint32_t in_col, uint32_t in_chan,
                const int8_t* w, uint32_t k_row, uint32_t k_col, uint32_t stride, uint32_t pad,
                uint32_t out_chan, const float* w_scale, const float* bias, float in_act_scale,
                int in_act_zp, float out_act_scale, int out_act_zp, uint8_t* out) {
    uint32_t out_row, out_col, oc, j, k, ic, jj, kk;

    if (!in || !w || !w_scale || !bias || !out) {
        return CNN_POINTER_NULL;
    }
    if (k_row == 0 || k_col == 0 || stride == 0 || in_chan == 0 || out_chan == 0 ||
        pad > (UINT32_MAX - in_row) / 2 || pad > (UINT32_MAX - in_col) / 2 ||
        k_row > in_row + 2 * pad || k_col > in_col + 2 * pad) {
        return CNN_DATA_EXCEPTION;
    }

    // 统一入口：纯 1D（单行输入 + 单行核）自动分派到 1D 优化算法，
    // 与 fp32 的 conv2d_bn_no_bias 行为一致；其余情况走 2D 通用算法。
    if (in_row == 1 && k_row == 1) {
        return conv1d_int8(in, in_col, in_chan, w, k_col, stride, pad, out_chan, w_scale, bias,
                           in_act_scale, in_act_zp, out_act_scale, out_act_zp, out);
    }
    out_row = (in_row + 2 * pad - k_row) / stride + 1;
    out_col = (in_col + 2 * pad - k_col) / stride + 1;

    for (oc = 0; oc < out_chan; oc++) {
        const int8_t* woc = w + oc * (in_chan * k_row * k_col);
        const float mul = w_scale[oc] * in_act_scale;
        const float b = bias[oc];
        uint8_t* o = out + oc * (out_row * out_col);

        for (j = 0; j < out_row; j++) {
            const int r0 = (int)(j * stride) - (int)pad;
            const int r_in = (r0 >= 0 && (uint32_t)(r0 + k_row) <= in_row);
            for (k = 0; k < out_col; k++) {
                const int c0 = (int)(k * stride) - (int)pad;
                const int c_in = (c0 >= 0 && (uint32_t)(c0 + k_col) <= in_col);
                int32_t acc = 0;
                if (r_in && c_in) {
                    // 窗口完全在界内：无分支快速路径。
                    for (ic = 0; ic < in_chan; ic++) {
                        const uint8_t* inp =
                            in + ic * (in_row * in_col) + (uint32_t)r0 * in_col + (uint32_t)c0;
                        const int8_t* wp = woc + ic * (k_row * k_col);
                        for (jj = 0; jj < k_row; jj++) {
                            for (kk = 0; kk < k_col; kk++) {
                                acc += (int32_t)wp[jj * k_col + kk] *
                                       ((int32_t)inp[jj * in_col + kk] - in_act_zp);
                            }
                        }
                    }
                } else {
                    // 边界窗口：逐元素检查，越界输入按 0 处理。
                    for (ic = 0; ic < in_chan; ic++) {
                        const int8_t* wp = woc + ic * (k_row * k_col);
                        for (jj = 0; jj < k_row; jj++) {
                            const int rr = r0 + (int)jj;
                            if (rr < 0 || rr >= (int)in_row) {
                                continue;
                            }
                            for (kk = 0; kk < k_col; kk++) {
                                const int cc = c0 + (int)kk;
                                if (cc >= 0 && cc < (int)in_col) {
                                    acc += (int32_t)wp[jj * k_col + kk] *
                                           ((int32_t)in[ic * (in_row * in_col) +
                                                        (uint32_t)rr * in_col + (uint32_t)cc] -
                                            in_act_zp);
                                }
                            }
                        }
                    }
                }
                {
                    // 反量化 -> ReLU -> 输出量化。
                    float y = (float)acc * mul + b;
                    int q;
                    if (y < 0.0f) {
                        y = 0.0f;
                    }
                    q = (int)lrintf(y / out_act_scale) + out_act_zp;
                    o[j * out_col + k] = (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
                }
            }
        }
    }
    return CNN_NORMAL;
}

/* --------------------- 动态量化（无需校准数据） --------------------- */

/**
 * @brief 计算单个 1D 输出点的 int32 累加值（不含 bias/ReLU）。
 *
 * 动态量化两遍法复用：第一遍用整数累加找每通道最大值，第二遍用整数
 * 累加 + 定点重量化。static inline 保证在任意优化级别下内联展开，
 * 无函数调用 prologue/epilogue 开销。
 *
 * @param in        输入 uint8 特征
 * @param in_col    输入宽度
 * @param in_chan   输入通道数
 * @param wrow      当前输出通道的权重行
 * @param k         核宽
 * @param s         输出位置对应的输入窗口起点（可能为负，表示 padding）
 * @param in_act_zp 输入激活零点
 * @return acc = sum(w_q * (x_q - zp))
 */
static inline int32_t conv1d_int8_acc(const uint8_t* in, uint32_t in_col, uint32_t in_chan,
                                      const int8_t* wrow, uint32_t k, int s, int in_act_zp) {
    int32_t acc = 0;
    uint32_t ic, kk;
    const int inside = (s >= 0 && (uint32_t)(s + k) <= in_col);
    for (ic = 0; ic < in_chan; ic++) {
        const int8_t* wp = wrow + ic * k;
        if (inside) {
            const uint8_t* ip = in + ic * in_col + (uint32_t)s;
            for (kk = 0; kk < k; kk++) {
                acc += (int32_t)wp[kk] * ((int32_t)ip[kk] - in_act_zp);
            }
        } else {
            const uint8_t* ip = in + ic * in_col;
            for (kk = 0; kk < k; kk++) {
                const int p = s + (int)kk;
                if (p >= 0 && p < (int)in_col) {
                    acc += (int32_t)wp[kk] * ((int32_t)ip[p] - in_act_zp);
                }
            }
        }
    }
    return acc;
}

/**
 * @brief 计算单个 2D 输出点的 int32 累加值（不含 bias/ReLU）。
 *
 * 语义与 conv1d_int8_acc 相同，仅多出行维处理；同样 static inline。
 * window_inside 由调用方在行/列循环外计算后传入，避免每个输出点重复
 * 计算行/列边界标志（行标志只与 r0 有关、列标志只与 c0 有关）。
 *
 * @param in            输入 uint8 特征
 * @param in_row        输入行数
 * @param in_col        输入列数
 * @param in_chan       输入通道数
 * @param woc           当前输出通道的权重
 * @param k_row         核高
 * @param k_col         核宽
 * @param r0            窗口起始行（可能为负，表示 padding）
 * @param c0            窗口起始列（可能为负，表示 padding）
 * @param window_inside 窗口是否完全在输入内（0/1）
 * @param in_act_zp     输入激活零点
 * @return acc = sum(w_q * (x_q - zp))
 */
static inline int32_t conv2d_int8_acc(const uint8_t* in, uint32_t in_row, uint32_t in_col,
                                      uint32_t in_chan, const int8_t* woc, uint32_t k_row,
                                      uint32_t k_col, int r0, int c0, int window_inside,
                                      int in_act_zp) {
    int32_t acc = 0;
    uint32_t ic, jj, kk;
    if (window_inside) {
        for (ic = 0; ic < in_chan; ic++) {
            const uint8_t* inp = in + ic * (in_row * in_col) + (uint32_t)r0 * in_col + (uint32_t)c0;
            const int8_t* wp = woc + ic * (k_row * k_col);
            for (jj = 0; jj < k_row; jj++) {
                for (kk = 0; kk < k_col; kk++) {
                    acc +=
                        (int32_t)wp[jj * k_col + kk] * ((int32_t)inp[jj * in_col + kk] - in_act_zp);
                }
            }
        }
    } else {
        for (ic = 0; ic < in_chan; ic++) {
            const int8_t* wp = woc + ic * (k_row * k_col);
            for (jj = 0; jj < k_row; jj++) {
                const int rr = r0 + (int)jj;
                if (rr < 0 || rr >= (int)in_row) {
                    continue;
                }
                for (kk = 0; kk < k_col; kk++) {
                    const int cc = c0 + (int)kk;
                    if (cc >= 0 && cc < (int)in_col) {
                        acc +=
                            (int32_t)wp[jj * k_col + kk] *
                            ((int32_t)
                                 in[ic * (in_row * in_col) + (uint32_t)rr * in_col + (uint32_t)cc] -
                             in_act_zp);
                    }
                }
            }
        }
    }
    return acc;
}

/**
 * @brief int8 1D 卷积 + 动态量化（无需校准数据）。
 *
 * 激活量化参数在线计算，分两遍、逐点零浮点转换：
 *   第一遍：整数域累加找每通道最大 acc，再用每通道一次的浮点运算得到
 *           全局 ymax = max(0, acc_max * mul + bias)，scale = ymax / 255；
 *   第二遍：整数域定点重量化
 *           q = round_half_even((acc * A_fix + B_fix) / 2^K)
 *           其中 A_fix / B_fix 为每通道一次计算好的定点系数。
 *
 * 由于 mul = w_scale * in_act_scale > 0，y = acc * mul + bias 随 acc 单调，
 * 全局最大值必然出现在某通道 acc 最大处，因此第一遍只需整数 max 即可，
 * 与逐点浮点求 max 数学等价（实测逐位一致）。
 * 激活 ReLU 后非负，故 zp=0。零额外内存：第一遍不写输出 buffer。
 * 全零输出（ymax==0）时直接写 0，避免定点系数除零。
 *
 * @param in             [in]  输入 uint8 特征
 * @param in_col         [in]  输入宽度
 * @param in_chan        [in]  输入通道数
 * @param w              [in]  int8 权重
 * @param k              [in]  核宽
 * @param stride         [in]  步长
 * @param pad            [in]  每侧 zero-padding 宽度
 * @param out_chan       [in]  输出通道数
 * @param w_scale        [in]  per-channel 权重步长
 * @param bias           [in]  BN 折叠后的偏置
 * @param in_act_scale   [in]  输入激活步长（上一层动态 scale）
 * @param in_act_zp      [in]  输入激活零点
 * @param out_act_scale  [out] 本层动态 scale（= max/255）
 * @param out            [out] 输出 uint8
 * @return CNN_NORMAL / CNN_POINTER_NULL / CNN_DATA_EXCEPTION
 */
int conv1d_int8_dynamic(const uint8_t* in, uint32_t in_col, uint32_t in_chan, const int8_t* w,
                        uint32_t k, uint32_t stride, uint32_t pad, uint32_t out_chan,
                        const float* w_scale, const float* bias, float in_act_scale, int in_act_zp,
                        float* out_act_scale, uint8_t* out) {
    uint32_t out_col, oc, j;
    float ymax, scale;
    const double k2 = (double)(1LL << DYNAMIC_QUANT_FRAC_BITS);

    if (!in || !w || !w_scale || !bias || !out || !out_act_scale) {
        return CNN_POINTER_NULL;
    }
    if (k == 0 || stride == 0 || pad > (UINT32_MAX - in_col) / 2 || k > in_col + 2 * pad ||
        in_chan == 0 || out_chan == 0) {
        return CNN_DATA_EXCEPTION;
    }
    out_col = (in_col + 2 * pad - k) / stride + 1;

    // ---- 第一遍：整数域找每通道 acc 最大值，再求全局 ymax ----
    ymax = 0.0f;
    for (oc = 0; oc < out_chan; oc++) {
        const float mul = w_scale[oc] * in_act_scale;
        const float b = bias[oc];
        const int8_t* wrow = w + oc * (in_chan * k);
        int32_t acc_max = INT32_MIN;
        for (j = 0; j < out_col; j++) {
            const int s = (int)(j * stride) - (int)pad;
            const int32_t acc = conv1d_int8_acc(in, in_col, in_chan, wrow, k, s, in_act_zp);
            if (acc > acc_max) {
                acc_max = acc;
            }
        }
        // 每通道仅一次浮点运算（含 ReLU），与逐点求 max 数学等价。
        const float y_ch = (float)acc_max * mul + b;
        const float y_relu = y_ch > 0.0f ? y_ch : 0.0f;
        if (y_relu > ymax) {
            ymax = y_relu;
        }
    }
    scale = ymax / 255.0f;
    if (scale < 1e-9f) {
        scale = 1e-9f;  // 全零输出保护（与旧实现一致）
    }
    *out_act_scale = scale;

    // 全零输出：全部码字为 0，直接返回（与浮点参考一致）。
    if (ymax <= 0.0f) {
        memset(out, 0, (size_t)out_chan * out_col);
        return CNN_NORMAL;
    }

    // ---- 第二遍：整数域定点重量化，逐点仅 int32 累加 + int64 乘加 ----
    for (oc = 0; oc < out_chan; oc++) {
        const float mul = w_scale[oc] * in_act_scale;
        const float b = bias[oc];
        const int8_t* wrow = w + oc * (in_chan * k);
        const int64_t a_fix = fixed_coeff((double)mul / (double)scale * k2);
        const int64_t b_fix = fixed_coeff((double)b / (double)scale * k2);
        uint8_t* o = out + oc * out_col;
        for (j = 0; j < out_col; j++) {
            const int s = (int)(j * stride) - (int)pad;
            const int32_t acc = conv1d_int8_acc(in, in_col, in_chan, wrow, k, s, in_act_zp);
            const int64_t v = (int64_t)acc * a_fix + b_fix;
            const int32_t q = round_half_even_shift64(v, DYNAMIC_QUANT_FRAC_BITS);
            o[j] = (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
        }
    }
    return CNN_NORMAL;
}

/**
 * @brief int8 卷积统一入口 + 动态量化（1D/2D 自动分派，无需校准数据）。
 *
 * 1D 时等价于 conv1d_int8_dynamic；2D 时第一遍按行/列找每通道最大 acc，
 * 第二遍用同一套整数域定点重量化规则。
 *
 * @param in             [in]  输入 uint8 特征
 * @param in_row         [in]  输入行数（1D 时为 1）
 * @param in_col         [in]  输入列数
 * @param in_chan        [in]  输入通道数
 * @param w              [in]  int8 权重
 * @param k_row          [in]  核高（1D 时为 1）
 * @param k_col          [in]  核宽
 * @param stride         [in]  行列共用步长
 * @param pad            [in]  每侧 zero-padding 宽度
 * @param out_chan       [in]  输出通道数
 * @param w_scale        [in]  per-channel 权重步长
 * @param bias           [in]  BN 折叠后的偏置
 * @param in_act_scale   [in]  输入激活步长
 * @param in_act_zp      [in]  输入激活零点
 * @param out_act_scale  [out] 本层动态 scale
 * @param out            [out] 输出 uint8
 * @return CNN_NORMAL / CNN_POINTER_NULL / CNN_DATA_EXCEPTION
 */
int conv2d_int8_dynamic(const uint8_t* in, uint32_t in_row, uint32_t in_col, uint32_t in_chan,
                        const int8_t* w, uint32_t k_row, uint32_t k_col, uint32_t stride,
                        uint32_t pad, uint32_t out_chan, const float* w_scale, const float* bias,
                        float in_act_scale, int in_act_zp, float* out_act_scale, uint8_t* out) {
    uint32_t out_row, out_col, oc, j, k;
    float ymax, scale;
    const double k2 = (double)(1LL << DYNAMIC_QUANT_FRAC_BITS);

    if (!in || !w || !w_scale || !bias || !out || !out_act_scale) {
        return CNN_POINTER_NULL;
    }
    if (k_row == 0 || k_col == 0 || stride == 0 || in_chan == 0 || out_chan == 0 ||
        pad > (UINT32_MAX - in_row) / 2 || pad > (UINT32_MAX - in_col) / 2 ||
        k_row > in_row + 2 * pad || k_col > in_col + 2 * pad) {
        return CNN_DATA_EXCEPTION;
    }

    // 1D 自动分派。
    if (in_row == 1 && k_row == 1) {
        return conv1d_int8_dynamic(in, in_col, in_chan, w, k_col, stride, pad, out_chan, w_scale,
                                   bias, in_act_scale, in_act_zp, out_act_scale, out);
    }
    out_row = (in_row + 2 * pad - k_row) / stride + 1;
    out_col = (in_col + 2 * pad - k_col) / stride + 1;

    // ---- 第一遍：整数域找每通道 acc 最大值，再求全局 ymax ----
    ymax = 0.0f;
    for (oc = 0; oc < out_chan; oc++) {
        const float mul = w_scale[oc] * in_act_scale;
        const float b = bias[oc];
        const int8_t* woc = w + oc * (in_chan * k_row * k_col);
        int32_t acc_max = INT32_MIN;
        for (j = 0; j < out_row; j++) {
            const int r0 = (int)(j * stride) - (int)pad;
            // 行边界标志只与 j 有关，提升到行循环外。
            const int r_in = (r0 >= 0 && (uint32_t)(r0 + k_row) <= in_row);
            for (k = 0; k < out_col; k++) {
                const int c0 = (int)(k * stride) - (int)pad;
                // 列边界标志只与 k 有关，与行标志合并后传给累加 helper。
                const int window_inside = r_in && (c0 >= 0 && (uint32_t)(c0 + k_col) <= in_col);
                const int32_t acc = conv2d_int8_acc(in, in_row, in_col, in_chan, woc, k_row, k_col,
                                                    r0, c0, window_inside, in_act_zp);
                if (acc > acc_max) {
                    acc_max = acc;
                }
            }
        }
        const float y_ch = (float)acc_max * mul + b;
        const float y_relu = y_ch > 0.0f ? y_ch : 0.0f;
        if (y_relu > ymax) {
            ymax = y_relu;
        }
    }
    scale = ymax / 255.0f;
    if (scale < 1e-9f) {
        scale = 1e-9f;  // 全零输出保护
    }
    *out_act_scale = scale;

    if (ymax <= 0.0f) {
        memset(out, 0, (size_t)out_chan * out_row * out_col);
        return CNN_NORMAL;
    }

    // ---- 第二遍：整数域定点重量化 ----
    for (oc = 0; oc < out_chan; oc++) {
        const float mul = w_scale[oc] * in_act_scale;
        const float b = bias[oc];
        const int8_t* woc = w + oc * (in_chan * k_row * k_col);
        const int64_t a_fix = fixed_coeff((double)mul / (double)scale * k2);
        const int64_t b_fix = fixed_coeff((double)b / (double)scale * k2);
        uint8_t* o = out + oc * (out_row * out_col);
        for (j = 0; j < out_row; j++) {
            const int r0 = (int)(j * stride) - (int)pad;
            const int r_in = (r0 >= 0 && (uint32_t)(r0 + k_row) <= in_row);
            for (k = 0; k < out_col; k++) {
                const int c0 = (int)(k * stride) - (int)pad;
                const int window_inside = r_in && (c0 >= 0 && (uint32_t)(c0 + k_col) <= in_col);
                const int32_t acc = conv2d_int8_acc(in, in_row, in_col, in_chan, woc, k_row, k_col,
                                                    r0, c0, window_inside, in_act_zp);
                const int64_t v = (int64_t)acc * a_fix + b_fix;
                const int32_t q = round_half_even_shift64(v, DYNAMIC_QUANT_FRAC_BITS);
                o[j * out_col + k] = (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
            }
        }
    }
    return CNN_NORMAL;
}

/**
 * @brief uint8 1D 最大池化（量化域直接取 max）。
 *
 * 输出长度 = (n - k) / stride + 1（valid 池化，与 fp32 maxpool 一致）。
 * 量化函数单调，uint8 直接取 max 与浮点池化等价。
 *
 * @param in     [in]  输入 uint8
 * @param n      [in]  输入长度
 * @param out    [out] 输出 uint8
 * @param k      [in]  核宽
 * @param stride [in]  步长
 */
void maxpool1d_u8(const uint8_t* in, uint32_t n, uint8_t* out, uint32_t k, uint32_t stride) {
    uint32_t out_n;
    uint32_t i, j;
    if (!in || !out || k == 0 || stride == 0 || k > n) {
        return;
    }
    out_n = (n - k) / stride + 1;
    for (i = 0; i < out_n; i++) {
        uint8_t m = in[i * stride];
        for (j = 1; j < k; j++) {
            if (in[i * stride + j] > m) {
                m = in[i * stride + j];
            }
        }
        out[i] = m;
    }
}

/**
 * @brief uint8 2D 最大池化（量化域直接取 max）。
 *
 * @param in     [in]  输入 uint8，布局 [in_row][in_col]
 * @param in_row [in]  输入行数
 * @param in_col [in]  输入列数
 * @param out    [out] 输出 uint8
 * @param k_row  [in]  核高
 * @param k_col  [in]  核宽
 * @param stride [in]  步长
 */
void maxpool2d_u8(const uint8_t* in, uint32_t in_row, uint32_t in_col, uint8_t* out, uint32_t k_row,
                  uint32_t k_col, uint32_t stride) {
    uint32_t out_row, out_col;
    uint32_t j, k, jj, kk;

    // 参数校验：核大于输入时 (in - k) 会 uint32 下溢导致越界写，必须先检查。
    if (!in || !out || k_row == 0 || k_col == 0 || stride == 0 || k_row > in_row ||
        k_col > in_col) {
        return;
    }
    out_row = (in_row - k_row) / stride + 1;
    out_col = (in_col - k_col) / stride + 1;

    for (j = 0; j < out_row; j++) {
        for (k = 0; k < out_col; k++) {
            uint8_t max_num = in[(j * stride) * in_col + k * stride];
            for (jj = 0; jj < k_row; jj++) {
                for (kk = 0; kk < k_col; kk++) {
                    uint8_t v = in[(j * stride + jj) * in_col + k * stride + kk];
                    if (v > max_num) {
                        max_num = v;
                    }
                }
            }
            out[j * out_col + k] = max_num;
        }
    }
}

/**
 * @brief uint8 最大池化统一入口（1D/2D 自动分派）。
 *
 * @param in     [in]  输入 uint8，单通道平面
 * @param in_row [in]  输入行数（1D 时为 1）
 * @param in_col [in]  输入列数
 * @param out    [out] 输出 uint8，单通道平面
 * @param k_row  [in]  核高（1D 时为 1）
 * @param k_col  [in]  核宽
 * @param stride [in]  步长
 */
void maxpool_u8(const uint8_t* in, uint32_t in_row, uint32_t in_col, uint8_t* out, uint32_t k_row,
                uint32_t k_col, uint32_t stride) {
    // 统一入口：纯 1D（单行输入 + 单行核）走 1D 优化算法，否则 2D。
    if (in_row == 1 && k_row == 1) {
        maxpool1d_u8(in, in_col, out, k_col, stride);
    } else {
        maxpool2d_u8(in, in_row, in_col, out, k_row, k_col, stride);
    }
}

/**
 * @brief int8 全连接层：uint8 输入 -> fp32 输出。
 *
 * 整数域累加（int32）-> 反量化。最长 4032 项时上界约 1.31e8 < INT32_MAX，安全。
 * 输出行做 4 行分块（4 个独立累加器，打破乘加依赖链、便于向量化）；
 * 每行内的累加顺序仍为 j 从小到大，与 Python int32 复现一致。
 * 输入零点为 0 时走免减法快路径。
 *
 * @param in           [in]  输入 uint8，长度 in_size
 * @param in_size      [in]  输入维度
 * @param w            [in]  int8 权重，布局 [out_size][in_size]
 * @param w_scale      [in]  per-output 权重步长
 * @param bias         [in]  float 偏置
 * @param out_size     [in]  输出维度
 * @param in_act_scale [in]  输入激活步长
 * @param in_act_zp    [in]  输入激活零点
 * @param out          [out] 输出 fp32 logits
 * @return CNN_NORMAL / CNN_POINTER_NULL / CNN_DATA_EXCEPTION
 */
int linear_int8(const uint8_t* in, uint32_t in_size, const int8_t* w, const float* w_scale,
                const float* bias, uint32_t out_size, float in_act_scale, int in_act_zp,
                float* out) {
    uint32_t i, j;
    if (!in || !w || !w_scale || !bias || !out) {
        return CNN_POINTER_NULL;
    }
    if (in_size == 0 || out_size == 0) {
        return CNN_DATA_EXCEPTION;
    }

    if (in_act_zp == 0) {
        // 零点为 0 的快路径：省去每个输入元素的减法，且输入可零扩展直接相乘。
        for (i = 0; i + 4 <= out_size; i += 4) {
            int32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
            const int8_t* w0 = w + i * in_size;
            const int8_t* w1 = w0 + in_size;
            const int8_t* w2 = w1 + in_size;
            const int8_t* w3 = w2 + in_size;
            for (j = 0; j < in_size; j++) {
                const int32_t xv = (int32_t)in[j];
                a0 += (int32_t)w0[j] * xv;
                a1 += (int32_t)w1[j] * xv;
                a2 += (int32_t)w2[j] * xv;
                a3 += (int32_t)w3[j] * xv;
            }
            out[i] = (float)a0 * (w_scale[i] * in_act_scale) + bias[i];
            out[i + 1] = (float)a1 * (w_scale[i + 1] * in_act_scale) + bias[i + 1];
            out[i + 2] = (float)a2 * (w_scale[i + 2] * in_act_scale) + bias[i + 2];
            out[i + 3] = (float)a3 * (w_scale[i + 3] * in_act_scale) + bias[i + 3];
        }
        for (; i < out_size; i++) {
            const int8_t* wr = w + i * in_size;
            int32_t acc = 0;
            for (j = 0; j < in_size; j++) {
                acc += (int32_t)wr[j] * (int32_t)in[j];
            }
            out[i] = (float)acc * (w_scale[i] * in_act_scale) + bias[i];
        }
    } else {
        // 通用路径：每个输入元素先减零点，4 行共享该值。
        for (i = 0; i + 4 <= out_size; i += 4) {
            int32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
            const int8_t* w0 = w + i * in_size;
            const int8_t* w1 = w0 + in_size;
            const int8_t* w2 = w1 + in_size;
            const int8_t* w3 = w2 + in_size;
            for (j = 0; j < in_size; j++) {
                const int32_t xv = (int32_t)in[j] - in_act_zp;
                a0 += (int32_t)w0[j] * xv;
                a1 += (int32_t)w1[j] * xv;
                a2 += (int32_t)w2[j] * xv;
                a3 += (int32_t)w3[j] * xv;
            }
            out[i] = (float)a0 * (w_scale[i] * in_act_scale) + bias[i];
            out[i + 1] = (float)a1 * (w_scale[i + 1] * in_act_scale) + bias[i + 1];
            out[i + 2] = (float)a2 * (w_scale[i + 2] * in_act_scale) + bias[i + 2];
            out[i + 3] = (float)a3 * (w_scale[i + 3] * in_act_scale) + bias[i + 3];
        }
        for (; i < out_size; i++) {
            const int8_t* wr = w + i * in_size;
            int32_t acc = 0;
            for (j = 0; j < in_size; j++) {
                acc += (int32_t)wr[j] * ((int32_t)in[j] - in_act_zp);
            }
            out[i] = (float)acc * (w_scale[i] * in_act_scale) + bias[i];
        }
    }
    return CNN_NORMAL;
}

/**
 * @brief fp32 全连接层（线性层）。
 *
 * 对输出行做 4 行分块：输入向量 x 一次加载被 4 行权重复用（缓存友好），
 * 4 个独立累加器消除乘加链相关性，编译器可向量化为 FMA。
 * 累加顺序（每行内 j 从小到大）与朴素实现一致。
 *
 * @param inp           [in]  输入 float，长度 inp_size
 * @param linear_config [in]  全连接配置
 * @param out           [out] 输出 float，长度 fea_size
 * @return CNN_NORMAL / CNN_POINTER_NULL
 */
int linear_layer(float* inp, LinearParam* linear_config, float* out) {
    uint32_t i, j, fea_size, inp_size;
    const float* restrict w;
    const float* restrict x;
    float* restrict o;

    if (!inp || !linear_config || !linear_config->weight || !linear_config->bias || !out) {
        return CNN_POINTER_NULL;
    }

    fea_size = linear_config->fea_size;
    inp_size = linear_config->inp_size;
    w = linear_config->weight;
    x = inp;
    o = out;

    // 4 行同时累加：a0..a3 为 4 个独立累加器。
    for (i = 0; i + 4 <= fea_size; i += 4) {
        float a0 = linear_config->bias[i];
        float a1 = linear_config->bias[i + 1];
        float a2 = linear_config->bias[i + 2];
        float a3 = linear_config->bias[i + 3];
        const float* w0 = w + i * inp_size;
        const float* w1 = w0 + inp_size;
        const float* w2 = w1 + inp_size;
        const float* w3 = w2 + inp_size;

        for (j = 0; j < inp_size; j++) {
            const float xv = x[j];
            a0 += w0[j] * xv;
            a1 += w1[j] * xv;
            a2 += w2[j] * xv;
            a3 += w3[j] * xv;
        }

        o[i] = a0;
        o[i + 1] = a1;
        o[i + 2] = a2;
        o[i + 3] = a3;
    }

    // 尾部（fea_size 不是 4 的倍数时）。
    for (; i < fea_size; i++) {
        float acc = linear_config->bias[i];
        const float* wrow = w + i * inp_size;
        for (j = 0; j < inp_size; j++) {
            acc += x[j] * wrow[j];
        }
        o[i] = acc;
    }

    return CNN_NORMAL;
}

/**
 * @brief fp32 最大池化（通用 1D/2D，任意核大小与步长）。
 *
 * 当前仅支持 pad=0。支持 in-place（input_feat->data == output_feat->data）：
 * 输出按升序写入，且每个输出窗口先完全读入再写，任意 stride>=1 均安全。
 *
 * @param input_feat  [in]  输入特征图
 * @param param       [in]  池化配置
 * @param output_feat [out] 输出特征图
 * @return CNN_NORMAL / CNN_POINTER_NULL / CNN_DATA_EXCEPTION
 */
int maxpool(Conv2dData* input_feat, MaxPoolConfig* param, Conv2dData* output_feat) {
    uint32_t out_row = 0, out_col = 0, out_chan = 0;
    uint32_t in_row = 0, in_col = 0, in_chan = 0;
    uint32_t i = 0, j = 0, k = 0, jj = 0, kk = 0;

    if (!input_feat || !input_feat->data || !param || !output_feat || !output_feat->data) {
        return CNN_POINTER_NULL;
    }
    if (param->row == 0 || param->col == 0 || param->stride == 0 || param->pad != 0) {
        return CNN_DATA_EXCEPTION;
    }

    in_row = input_feat->row;
    in_col = input_feat->col;
    in_chan = input_feat->channel;

    if (param->row > in_row || param->col > in_col) {
        return CNN_DATA_EXCEPTION;
    }

    out_row = (in_row == 1) ? 1 : cal_conv_out_len(in_row, param->pad, param->row, param->stride);
    out_col = cal_conv_out_len(input_feat->col, param->pad, param->col, param->stride);
    out_chan = in_chan;

    // 通用 1D 路径：input row=1、核 row=1，任意核宽与步长（如 stride=2/3）。
    if (in_row == 1 && param->row == 1) {
        for (i = 0; i < out_chan; i++) {
            const float* s = input_feat->data + i * in_col;
            float* d = output_feat->data + i * out_col;
            for (k = 0; k < out_col; k++) {
                const float* w = s + k * param->stride;
                float max_num = w[0];
                for (kk = 1; kk < param->col; kk++) {
                    if (w[kk] > max_num) {
                        max_num = w[kk];
                    }
                }
                d[k] = max_num;
            }
        }
    } else {
        // 通用 2D 路径：任意核大小与步长。
        for (i = 0; i < out_chan; i++) {
            const float* in_c = input_feat->data + i * (in_row * in_col);
            float* out_c = output_feat->data + i * (out_row * out_col);

            for (j = 0; j < out_row; j++) {
                const float* row_base = in_c + j * param->stride * in_col;
                for (k = 0; k < out_col; k++) {
                    const float* col_base = row_base + k * param->stride;
                    float max_num = col_base[0];

                    // 第一行从列 1 开始（max_num 已初始化为列 0），其余行从列 0 开始。
                    for (jj = 0; jj < param->row; jj++) {
                        const float* prow = col_base + jj * in_col;
                        for (kk = (jj == 0) ? 1 : 0; kk < param->col; kk++) {
                            if (prow[kk] > max_num) {
                                max_num = prow[kk];
                            }
                        }
                    }

                    *out_c++ = max_num;
                }
            }
        }
    }

    output_feat->row = out_row;
    output_feat->col = out_col;
    output_feat->channel = out_chan;

    return CNN_NORMAL;
}

/**
 * @brief 线性层 DSP 加速入口。
 *
 * PC 版不再依赖 RISC-V DSP 库，统一走优化后的普通线性层实现。
 */
int linear_layer_dsp(float* inp, LinearParam* linear_config, float* out) {
    return linear_layer(inp, linear_config, out);
}

/**
 * @brief Sigmoid 激活（原地计算）。
 *
 * y = 1 / (1 + exp(-x))
 *
 * @param x    [in,out] 输入/输出数据
 * @param size [in]     长度
 */
void sigmoid(float* x, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        *(x + i) = (1 / (1 + exp(-*(x + i))));
    }
}
