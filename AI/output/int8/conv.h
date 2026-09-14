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
 * @file conv.h
 * @brief 可移植 CNN 张量算子库（fp32 与 int8 量化推理）
 *
 * 本库只实现张量算子：卷积、批归一化、激活、全连接、池化、量化等，
 * 不关心数据来源（音频、图像或任意张量），也不包含任何预处理。
 * 全部代码仅依赖 C 标准库，可直接交叉编译到嵌入式平台（如 RISC-V/DSP），
 * 无第三方依赖、无动态内存分配（调用方提供所有 buffer）。
 *
 * 内存布局约定（全库统一，NCHW 连续存储）：
 *   - 特征图:  [channel][row][col]，data 容量为 channel * row * col
 *   - 卷积核:  [filter_num][channel][k_row][k_col]
 *   - 全连接:  weight 为 [fea_size][inp_size]
 *   - uint8 特征图布局与 fp32 相同（[channel][row][col]）
 */

#ifndef CNN_C_CONV_H_
#define CNN_C_CONV_H_

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief CNN 推理库错误码（所有 int 返回值函数使用）
 *
 * 返回 CNN_NORMAL(0) 表示成功；其余为负值错误码。
 */
typedef enum CnnError {
    CNN_ERR_GENERIC = -1,   ///< 通用错误
    CNN_NORMAL = 0,         ///< 成功
    CNN_MALLOC_FAIL,        ///< 内存分配失败（本库不主动分配，保留兼容）
    CNN_DATA_NOT_ENOUGH,    ///< 数据长度不足
    CNN_DATA_EXCEPTION,     ///< 数据/参数异常（形状、步长、核大小等不合法）
    CNN_DATA_NULL,          ///< 数据指针为空
    CNN_DATA_INVALID,       ///< 数据内容非法
    CNN_DATA_TOO_MANY,      ///< 数据量过大
    CNN_ERR_LOCAL_FREE,     ///< 本地释放失败（保留兼容）
    CNN_POINTER_NULL,       ///< 必要指针为空（NULL 参数）
    CNN_DATA_QUALITY_POOR,  ///< 数据质量差
    CNN_IO_EXCEPTION        ///< IO 异常
} CnnError;

/**
 * @brief 卷积层输入/输出特征图
 *
 * data 指向连续内存，布局为 [channel][row][col]，
 * 容量至少 channel * row * col 个 float。
 * 1D 特征（如 2048 点 FFT 幅度谱）表示为 row=1、col=长度。
 */
typedef struct Conv2dData {
    uint32_t row;      ///< 高；1D 特征恒为 1
    uint32_t col;      ///< 宽（1D 特征的长度）
    uint32_t channel;  ///< 通道数
    float* data;       ///< 特征图数据，NCHW 连续存储
} Conv2dData;

/**
 * @brief 卷积层权重（卷积核）
 *
 * data 布局为 [filter_num][channel][row][col]，
 * 容量 filter_num * channel * row * col 个 float。
 * 1D 卷积表示为 row=1、col=kernel 宽度。
 */
typedef struct Conv2dFilter {
    uint32_t row;         ///< 核高（1D 为 1）
    uint32_t col;         ///< 核宽
    uint32_t channel;     ///< 输入通道数（必须等于特征图通道数）
    uint32_t filter_num;  ///< 输出通道数
    float* data;          ///< 权重数据
} Conv2dFilter;

/**
 * @brief 批归一化（BatchNorm）参数
 *
 * 推理公式（eps=1e-5）：
 *   y = gamma * (x - mean) / sqrt(var + eps) + beta
 *
 * 本库内部会将其折叠为每通道 scale/shift 以加速，且不改动调用方数据。
 * size 必须等于卷积输出通道数 filter_num。
 */
typedef struct BatchNorm2d {
    uint32_t size;  ///< 参数长度，必须等于 filter_num
    float* mean;    ///< running_mean，长度 size
    float* var;     ///< running_var，长度 size
    float* gamma;   ///< weight，长度 size
    float* beta;    ///< bias，长度 size
} BatchNorm2d;

/**
 * @brief 卷积层配置（卷积核 + BN + 步长 + padding）
 *
 * padding 为“虚拟 padding”：不分配临时缓冲、不搬运数据，
 * 卷积内部对越界输入按 0 处理，仅边界输出像素有额外判断，
 * 对嵌入式性能影响很小。
 */
typedef struct Conv2dConfig {
    uint32_t stride;       ///< 步长，>=1
    uint32_t pad;          ///< 每侧 zero-padding 宽度（行列相同，>=0）
    Conv2dFilter* filter;  ///< 卷积核
    BatchNorm2d* bn;       ///< BN 参数
} Conv2dConfig;

/**
 * @brief 全连接层（线性层）配置
 *
 * weight 布局为 [fea_size][inp_size]，bias 长度为 fea_size。
 */
typedef struct LinearParam {
    uint32_t inp_size;  ///< 输入维度
    uint32_t fea_size;  ///< 输出维度
    float* weight;      ///< 权重，[fea_size][inp_size]
    float* bias;        ///< 偏置，长度 fea_size
} LinearParam;

/**
 * @brief 最大池化配置
 *
 * 当前仅支持 pad=0（valid 池化）。
 * 1D 池化表示为 row=1、col=核宽。
 */
typedef struct MaxPoolConfig {
    uint32_t row;     ///< 池化核高
    uint32_t col;     ///< 池化核宽
    uint32_t stride;  ///< 步长，>=1
    uint32_t pad;     ///< padding，当前仅支持 0
} MaxPoolConfig;

/**
 * @brief 计算卷积/池化输出长度（valid 输出）
 *
 * 公式：out = (raw_len + 2*pad_len - filter_len) / stride + 1
 *
 * @param raw_len    输入长度
 * @param pad_len    每侧 padding 宽度
 * @param filter_len 核大小
 * @param stride     步长
 * @return 输出长度
 */
uint32_t cal_conv_out_len(uint32_t raw_len, uint32_t pad_len, uint32_t filter_len, uint32_t stride);

/**
 * @brief fp32 卷积 + BN（无卷积 bias，BN 的 beta 充当 bias）
 *
 * 支持 1D/2D、zero-padding、任意 stride>=1。
 * 内部把 BN 折叠为 per-channel scale/shift，纯 C 实现。
 *
 * 累加顺序（保证与朴素参考实现一致）：
 *   channel(0..C-1) -> kernel_row -> kernel_col
 *
 * @param input_feat  [in]  输入特征图；1D 时 row=1
 * @param param       [in]  卷积配置（stride/pad/filter/bn）
 * @param output_feat [out] 输出特征图；data 由调用方提供，
 *                           容量至少 filter_num * out_row * out_col
 * @return CNN_NORMAL / CNN_POINTER_NULL / CNN_DATA_EXCEPTION
 */
int conv2d_bn_no_bias(Conv2dData* input_feat, Conv2dConfig* param, Conv2dData* output_feat);

/**
 * @brief LeakyReLU 激活函数
 *
 * y = x >= 0 ? x : x * neg_slope
 *
 * @param neg_slope 负半轴斜率；传 0 即 ReLU
 * @param inp       [in]  输入数据
 * @param inp_size  [in]  输入长度
 * @param out       [out] 输出数据；支持 in-place（out == inp）
 * @return CNN_NORMAL / CNN_POINTER_NULL
 */
int leaky_relu(float neg_slope, float* inp, uint32_t inp_size, float* out);

/**
 * @brief Softmax 激活（数值稳定：先减最大值再 exp，避免溢出）
 *
 * @param inp  [in]  输入数据，长度 size
 * @param size [in]  输入长度，必须 >0
 * @param out  [out] 输出概率，和为 1；支持 in-place（out == inp）
 * @return CNN_NORMAL / CNN_POINTER_NULL
 */
int softmax(const float* inp, uint32_t size, float* out);

/**
 * @brief 返回最大值所在下标（argmax）
 *
 * 并列时返回第一个最大值下标。
 *
 * @param inp     [in]  输入数据
 * @param size    [in]  输入长度，必须 >0
 * @param max_val [out] 可选；返回最大值，传 NULL 可忽略
 * @return 最大值下标；参数非法时返回 0
 */
uint32_t argmax(const float* inp, uint32_t size, float* max_val);

/* ============================ int8 量化算子 ============================ */

/**
 * @brief float -> uint8 非对称量化
 *
 * 公式：
 *   q = clamp(round(x / scale) + zp, 0, 255)
 *
 * 舍入规则与 C 端其余量化算子一致：lrintf（round-half-even）。
 * 注意：本函数为 void，参数非法时直接返回（不写 out）。
 *
 * @param in    [in]  输入 float 数组，长度 n
 * @param n     [in]  元素个数，必须 >0
 * @param scale [in]  量化步长，必须 >0
 * @param zp    [in]  零点（量化域中对应浮点 0 的值）
 * @param out   [out] 输出 uint8 数组，长度 n
 */
void quantize_u8(const float* in, uint32_t n, float scale, int zp, uint8_t* out);

/**
 * @brief int8 1D 卷积 + ReLU + 输出量化（静态量化）
 *
 * 计算流程（与 C 端实现严格一致）：
 *   1) 整数域累加：acc += w_q * (x_q - zp_in)，int32 累加
 *   2) 反量化：y = acc * (w_scale * in_act_scale) + bias
 *   3) ReLU：y = max(y, 0)
 *   4) 输出量化：q_out = clamp(round(y / out_act_scale) + out_act_zp, 0, 255)
 *
 * 输入 uint8，布局 [in_chan][in_col]；
 * 权重 int8 per-channel，布局 [out_chan][in_chan][k]；
 * bias 为 BN 折叠后的 float 偏置，长度 out_chan。
 *
 * @param in             [in]  输入 uint8 特征，长度 in_chan * in_col
 * @param in_col         [in]  输入宽度
 * @param in_chan        [in]  输入通道数，>0
 * @param w              [in]  int8 权重
 * @param k              [in]  核宽，>0
 * @param stride         [in]  步长，>=1（支持任意值，如 2/3）
 * @param pad            [in]  每侧 zero-padding 宽度（虚拟 padding）
 * @param out_chan       [in]  输出通道数，>0
 * @param w_scale        [in]  per-channel 权重步长，长度 out_chan
 * @param bias           [in]  折叠后的偏置，长度 out_chan
 * @param in_act_scale   [in]  输入激活步长
 * @param in_act_zp      [in]  输入激活零点
 * @param out_act_scale  [in]  输出激活步长
 * @param out_act_zp     [in]  输出激活零点
 * @param out            [out] 输出 uint8，容量 out_chan *
 *                             ((in_col + 2*pad - k) / stride + 1)
 * @return CNN_NORMAL / CNN_POINTER_NULL / CNN_DATA_EXCEPTION
 */
int conv1d_int8(const uint8_t* in, uint32_t in_col, uint32_t in_chan, const int8_t* w, uint32_t k,
                uint32_t stride, uint32_t pad, uint32_t out_chan, const float* w_scale,
                const float* bias, float in_act_scale, int in_act_zp, float out_act_scale,
                int out_act_zp, uint8_t* out);

/**
 * @brief int8 卷积统一入口 + ReLU + 输出量化（1D/2D 自动分派）
 *
 * 当 in_row==1 且 k_row==1 时自动分派到 1D 优化算法（等价于 conv1d_int8），
 * 否则走 2D 通用算法。计算与量化规则同 conv1d_int8。
 *
 * 输入 uint8，布局 [in_chan][in_row][in_col]；
 * 权重 int8，布局 [out_chan][in_chan][k_row][k_col]。
 *
 * @param in             [in]  输入 uint8 特征
 * @param in_row         [in]  输入行数（1D 时为 1）
 * @param in_col         [in]  输入列数
 * @param in_chan        [in]  输入通道数
 * @param w              [in]  int8 权重
 * @param k_row          [in]  核高（1D 时为 1）
 * @param k_col          [in]  核宽
 * @param stride         [in]  行列共用步长，>=1
 * @param pad            [in]  每侧 zero-padding 宽度（行列相同，虚拟 padding）
 * @param out_chan       [in]  输出通道数
 * @param w_scale        [in]  per-channel 权重步长，长度 out_chan
 * @param bias           [in]  折叠后的偏置，长度 out_chan
 * @param in_act_scale   [in]  输入激活步长
 * @param in_act_zp      [in]  输入激活零点
 * @param out_act_scale  [in]  输出激活步长
 * @param out_act_zp     [in]  输出激活零点
 * @param out            [out] 输出 uint8，容量
 *   out_chan * ((in_row + 2*pad - k_row) / stride + 1) *
 *              ((in_col + 2*pad - k_col) / stride + 1)
 * @return CNN_NORMAL / CNN_POINTER_NULL / CNN_DATA_EXCEPTION
 */
int conv2d_int8(const uint8_t* in, uint32_t in_row, uint32_t in_col, uint32_t in_chan,
                const int8_t* w, uint32_t k_row, uint32_t k_col, uint32_t stride, uint32_t pad,
                uint32_t out_chan, const float* w_scale, const float* bias, float in_act_scale,
                int in_act_zp, float out_act_scale, int out_act_zp, uint8_t* out);

/**
 * @brief int8 1D 卷积 + 动态量化（无需校准数据）
 *
 * 权重仍为离线静态 per-channel int8（w_scale 提前算好）；
 * 激活量化参数在推理时在线计算：
 *   - 第一遍在整数域找每通道最大累加值 acc_max，再由每通道一次的
 *     浮点运算得到全局 ymax = max(0, acc_max*mul + bias)；
 *   - scale = ymax / 255，zp = 0（ReLU 后非负）；
 *   - 第二遍在整数域定点重量化：
 *       q = clamp(round_half_even((acc*A_fix + B_fix) / 2^K), 0, 255)
 *
 * 实现为两遍计算、零额外内存，且逐点不产生任何浮点类型转换
 * （每通道仅一次浮点运算用于计算定点系数）。
 * 本层动态 scale 通过 out_act_scale 返回，供下一层反量化使用。
 *
 * @param in             [in]  输入 uint8 特征
 * @param in_col         [in]  输入宽度
 * @param in_chan        [in]  输入通道数
 * @param w              [in]  int8 权重
 * @param k              [in]  核宽
 * @param stride         [in]  步长
 * @param pad            [in]  每侧 zero-padding 宽度（虚拟 padding）
 * @param out_chan       [in]  输出通道数
 * @param w_scale        [in]  per-channel 权重步长
 * @param bias           [in]  折叠后的偏置
 * @param in_act_scale   [in]  输入激活步长（上一层返回的动态 scale）
 * @param in_act_zp      [in]  输入激活零点（动态量化链通常为 0）
 * @param out_act_scale  [out] 本层动态 scale（= max/255）
 * @param out            [out] 输出 uint8，容量 out_chan * out_col
 * @return CNN_NORMAL / CNN_POINTER_NULL / CNN_DATA_EXCEPTION
 */
int conv1d_int8_dynamic(const uint8_t* in, uint32_t in_col, uint32_t in_chan, const int8_t* w,
                        uint32_t k, uint32_t stride, uint32_t pad, uint32_t out_chan,
                        const float* w_scale, const float* bias, float in_act_scale, int in_act_zp,
                        float* out_act_scale, uint8_t* out);

/**
 * @brief int8 卷积统一入口 + 动态量化（1D/2D 自动分派，无需校准数据）
 *
 * 同 conv2d_int8 的 1D/2D 自动分派规则；量化方式与 conv1d_int8_dynamic
 * 相同（整数域找 max + 定点重量化），返回 out_act_scale 供下一层使用。
 *
 * @param in             [in]  输入 uint8 特征
 * @param in_row         [in]  输入行数（1D 时为 1）
 * @param in_col         [in]  输入列数
 * @param in_chan        [in]  输入通道数
 * @param w              [in]  int8 权重
 * @param k_row          [in]  核高（1D 时为 1）
 * @param k_col          [in]  核宽
 * @param stride         [in]  步长
 * @param pad            [in]  每侧 zero-padding 宽度（虚拟 padding）
 * @param out_chan       [in]  输出通道数
 * @param w_scale        [in]  per-channel 权重步长
 * @param bias           [in]  折叠后的偏置
 * @param in_act_scale   [in]  输入激活步长
 * @param in_act_zp      [in]  输入激活零点
 * @param out_act_scale  [out] 本层动态 scale
 * @param out            [out] 输出 uint8
 * @return CNN_NORMAL / CNN_POINTER_NULL / CNN_DATA_EXCEPTION
 */
int conv2d_int8_dynamic(const uint8_t* in, uint32_t in_row, uint32_t in_col, uint32_t in_chan,
                        const int8_t* w, uint32_t k_row, uint32_t k_col, uint32_t stride,
                        uint32_t pad, uint32_t out_chan, const float* w_scale, const float* bias,
                        float in_act_scale, int in_act_zp, float* out_act_scale, uint8_t* out);

/**
 * @brief uint8 1D 最大池化（量化域直接取 max，与浮点池化等价）
 *
 * 输出长度：out_n = (n - k) / stride + 1（valid 池化）。
 *
 * @param in     [in]  输入 uint8，长度 n
 * @param n      [in]  输入长度
 * @param out    [out] 输出 uint8，长度 out_n
 * @param k      [in]  核宽
 * @param stride [in]  步长
 */
void maxpool1d_u8(const uint8_t* in, uint32_t n, uint8_t* out, uint32_t k, uint32_t stride);

/**
 * @brief uint8 2D 最大池化（量化域直接取 max）
 *
 * 输入布局 [in_row][in_col]（单通道），输出
 * [((in_row-k_row)/stride+1)] x [((in_col-k_col)/stride+1)]。
 * 多通道由调用方按通道平面循环调用。
 *
 * @param in     [in]  输入 uint8，长度 in_row * in_col
 * @param in_row [in]  输入行数
 * @param in_col [in]  输入列数
 * @param out    [out] 输出 uint8
 * @param k_row  [in]  核高
 * @param k_col  [in]  核宽
 * @param stride [in]  步长
 */
void maxpool2d_u8(const uint8_t* in, uint32_t in_row, uint32_t in_col, uint8_t* out, uint32_t k_row,
                  uint32_t k_col, uint32_t stride);

/**
 * @brief uint8 最大池化统一入口（1D/2D 自动分派）
 *
 * 当 in_row==1 且 k_row==1 时走 1D 优化算法，否则走 2D 通用算法。
 * 量化函数单调，max(quantize(a), quantize(b)) == quantize(max(a,b))，
 * 因此 uint8 直接取 max 与浮点池化等价。
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
                uint32_t k_col, uint32_t stride);

/**
 * @brief int8 全连接层：uint8 输入 -> fp32 输出（不做输出量化）
 *
 * 计算流程：
 *   1) 整数域累加：acc += w_q * (in_q - zp)，int32 累加
 *   2) 反量化：out = acc * (w_scale * in_act_scale) + bias
 *
 * @param in           [in]  输入 uint8，长度 in_size
 * @param in_size      [in]  输入维度，>0
 * @param w            [in]  int8 权重，布局 [out_size][in_size]
 * @param w_scale      [in]  per-output 权重步长，长度 out_size
 * @param bias         [in]  float 偏置，长度 out_size
 * @param out_size     [in]  输出维度，>0
 * @param in_act_scale [in]  输入激活步长
 * @param in_act_zp    [in]  输入激活零点
 * @param out          [out] 输出 fp32 logits，长度 out_size
 * @return CNN_NORMAL / CNN_POINTER_NULL / CNN_DATA_EXCEPTION
 */
int linear_int8(const uint8_t* in, uint32_t in_size, const int8_t* w, const float* w_scale,
                const float* bias, uint32_t out_size, float in_act_scale, int in_act_zp,
                float* out);

/**
 * @brief fp32 全连接层（线性层）
 *
 * out = W * in + bias。内部对输出行做 4 行分块累加（多独立累加器），
 * 以打破乘加链依赖、便于编译器向量化；累加顺序与朴素实现一致。
 *
 * @param inp           [in]  输入 float，长度 inp_size
 * @param linear_config [in]  全连接配置
 * @param out           [out] 输出 float，长度 fea_size
 * @return CNN_NORMAL / CNN_POINTER_NULL
 */
int linear_layer(float* inp, LinearParam* linear_config, float* out);

/**
 * @brief fp32 最大池化（通用 1D/2D，任意核大小与步长）
 *
 * 1D：input row=1 且 param->row=1，按列滑动；
 * 2D：按行列滑动。当前仅支持 pad=0。
 *
 * 支持 in-place（input_feat->data == output_feat->data）：
 * 实现按输出升序写入，且先读完整个输入窗口再写输出位置，
 * 任意 stride>=1 均安全。
 *
 * @param input_feat  [in]  输入特征图
 * @param param       [in]  池化配置（row/col/stride/pad=0）
 * @param output_feat [out] 输出特征图，data 由调用方提供
 * @return CNN_NORMAL / CNN_POINTER_NULL / CNN_DATA_EXCEPTION
 */
int maxpool(Conv2dData* input_feat, MaxPoolConfig* param, Conv2dData* output_feat);

/**
 * @brief 线性层 DSP 加速入口
 *
 * PC 版不再依赖 RISC-V DSP 库，统一走优化后的普通线性层实现。
 *
 * @param inp           [in]  输入 float
 * @param linear_config [in]  全连接配置
 * @param out           [out] 输出 float
 * @return error code（与 linear_layer 相同）
 */
int linear_layer_dsp(float* inp, LinearParam* linear_config, float* out);

/**
 * @brief Sigmoid 激活（原地计算）
 *
 * y = 1 / (1 + exp(-x))
 *
 * @param x    [in,out] 输入/输出数据
 * @param size [in]     长度
 */
void sigmoid(float* x, uint32_t size);

#endif  // CNN_C_CONV_H_
