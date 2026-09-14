"""
cnn2c：模型 -> CNN 库 C 文件转换脚本（通用版）

不固定网络结构：递归遍历 PyTorch 模型，按实际遇到的层生成代码。
支持的层：Conv1d/Conv2d、BatchNorm1d/2d、ReLU/LeakyReLU、
MaxPool1d/2d、Flatten、Dropout（跳过）、Linear。

输出到 output 文件夹，命名规范：
  <模型名>_param.c / <模型名>_param.h   —— 模型参数数组
  <模型名>_app.c   / <模型名>_app.h     —— 模型结构与运行代码

用户只需包含 <模型名>_app.h 并调用：
  int <模型名>_forward(const float *input, float *logits, uint32_t *class_id);

用法:
  python cnn2c.py --checkpoint model.pth --name model1
  python cnn2c.py --checkpoint model.pth --name model1 --quant int8
  python cnn2c.py --checkpoint model.pth --name model1 --quant int8 --calib calib.npz
"""

import argparse
import os
import re
import sys

import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import DataLoader

DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")


class _Block(nn.Module):
    """Conv(+DW/PW) + BN + ReLU + MaxPool（实验框架 SpeakerNet 的块结构）"""

    def __init__(self, in_ch, out_ch, kernel=5, pool=2, dw=False):
        super().__init__()
        if dw:
            self.conv = nn.Sequential(
                nn.Conv1d(in_ch, in_ch, kernel, padding=0, groups=in_ch, bias=False),
                nn.Conv1d(in_ch, out_ch, 1, bias=False),
            )
        else:
            self.conv = nn.Conv1d(in_ch, out_ch, kernel, bias=False)
        self.bn = nn.BatchNorm1d(out_ch)
        self.relu = nn.ReLU(inplace=True)
        self.pool_layer = nn.MaxPool1d(pool) if pool > 1 else None

    def forward(self, x):
        x = self.conv(x)
        x = self.bn(x)
        x = self.relu(x)
        if self.pool_layer is not None:
            x = self.pool_layer(x)
        return x


class SpeakerNet(nn.Module):
    """实验框架 SpeakerNet（内嵌定义，脚本可独立运行，不依赖工程的 common.py/train.py）"""

    def __init__(self, channels=(16, 32, 32), head="gap", fc=(32, 4), kernel=5, pool=2,
                 dw=False, dropout=0.0, in_len=2048):
        super().__init__()
        self.blocks = nn.ModuleList()
        prev = 1
        for ch in channels:
            self.blocks.append(_Block(prev, ch, kernel, pool, dw))
            prev = ch
        self.head = head
        self.gap = nn.AdaptiveAvgPool1d(1 if head == "gap" else int(head)) if head != "flat" else None
        self.fcs = nn.ModuleList()
        with torch.no_grad():
            dummy = torch.zeros(1, 1, in_len)
            for b in self.blocks:
                dummy = b(dummy)
            flat_len = dummy.numel() if head == "flat" else dummy.shape[1] * self.gap.output_size
        fc_in = flat_len
        for out in fc:
            self.fcs.append(nn.Linear(fc_in, out))
            fc_in = out
        self.dropout = dropout

    def forward(self, x):
        for b in self.blocks:
            x = b(x)
        if self.head != "flat":
            x = self.gap(x).reshape(x.size(0), -1)
        else:
            x = x.flatten(1)
        for i, fc in enumerate(self.fcs):
            x = fc(x)
            if self.dropout > 0 and i < len(self.fcs) - 1 and self.training:
                x = torch.nn.functional.dropout(x, self.dropout, training=True)
        return x


class CNNVoiceClassifier(nn.Module):
    """旧说话人模型（内嵌定义，用于加载旧格式整模型 pickle）。
    3 层 Conv1d + 全连接，结构与原训练脚本完全一致。"""

    def __init__(self):
        super().__init__()
        self.model = nn.Sequential(
            nn.Conv1d(1, 4, kernel_size=5, padding=0, bias=False),
            nn.BatchNorm1d(num_features=4),
            nn.ReLU(inplace=True),
            nn.MaxPool1d(2),
            nn.Conv1d(4, 8, kernel_size=5, padding=0, bias=False),
            nn.BatchNorm1d(num_features=8),
            nn.ReLU(inplace=True),
            nn.MaxPool1d(2),
            nn.Conv1d(8, 16, kernel_size=5, padding=0, bias=False),
            nn.BatchNorm1d(num_features=16),
            nn.ReLU(inplace=True),
            nn.MaxPool1d(2),
            nn.Flatten())
        self.dropout = nn.Dropout(p=0.4)
        self.fc1 = nn.Linear(4032, 32)
        self.fc2 = nn.Linear(32, 4)

    def forward(self, x):
        x = self.model(x)
        x = x.view(x.size(0), -1)
        x = self.fc1(x)
        x = self.dropout(x)
        x = self.fc2(x)
        return x


def model_name_from_path(path):
    base = os.path.splitext(os.path.basename(path))[0]
    base = re.sub(r"[^0-9A-Za-z_]", "_", base)
    if base and base[0].isdigit():
        base = "m_" + base
    return base


def _conv_dim(mod):
    """返回卷积核维度：1 表示 Conv1d，2 表示 Conv2d"""
    return mod.weight.dim() - 2


def _reverse_conv_dim(c, k, s, p):
    """反推卷积单维输入长度候选：L_out=(L_in+2p-k)/s+1 的多解"""
    out = []
    for t in range(int(s)):
        v = (int(c) - 1) * int(s) - 2 * int(p) + int(k) + t
        if v > 0:
            out.append(v)
    return out


def _reverse_pool(c, k, s):
    """反推 pool 的一维输入长度候选：L_out=(L_in-k)/s+1 的多解"""
    out = []
    for t in range(int(s)):
        v = (int(c) - 1) * int(s) + int(k) + t
        if v > 0:
            out.append(v)
    return out


def infer_input_shape(model, max_candidates=48):
    """从模型结构反推输入形状（尽力而为，不保证唯一）。

    原理：最后一个 Linear 的 in_features ÷ 最后一层卷积输出通道 = 特征空间点数，
    沿卷积/池化链反向推导输入空间尺寸的候选集合，再用随机前向验证。
    1D 模型直接得到长度；2D 模型得到面积，按接近方形拆分行列候选。
    返回 (row, col, ch) 或 None（无法推断）。
    """
    import itertools
    layers = collect_layers(model)
    lins = [l for l in layers if l[0] == "linear"]
    convs = [l for l in layers if l[0] == "conv"]
    if not lins or not convs:
        return None
    fc_in = int(lins[0][2])
    last_conv = convs[-1][1]
    out_ch = int(last_conv.out_channels)
    if fc_in % out_ch != 0:
        return None
    space = fc_in // out_ch  # 最后特征图的空间点数（1D 长度或 2D 面积）

    # 收集第一个 conv 之后、linear 之前的 conv/pool 链（用于反推）
    chain = []
    started = False
    for l in layers:
        if l[0] == "conv":
            started = True
            chain.append(l)
        elif l[0] == "pool":
            if started:
                chain.append(l)
        elif l[0] == "linear":
            break
    if not chain:
        return None

    def reverse_1d_chain(c, dim_index):
        """沿链反推单个维度的输入长度候选集合。
        dim_index: 1D 模型恒为 1（列）；2D 模型 0=行、1=列。"""
        cands = {c}
        for l in reversed(chain):
            new = set()
            if l[0] == "conv":
                mod = l[1]
                if _conv_dim(mod) == 1:
                    if dim_index == 1:
                        for v in cands:
                            new.update(_reverse_conv_dim(v, mod.kernel_size[0],
                                                          mod.stride[0], mod.padding[0]))
                    else:
                        new = cands
                else:
                    for v in cands:
                        new.update(_reverse_conv_dim(v, mod.kernel_size[dim_index],
                                                      mod.stride[dim_index],
                                                      mod.padding[dim_index]))
            else:  # pool
                k = l[2]
                s = l[3]
                if len(k) == 1:
                    # 1D 池化：核作用于列（MaxPool1d 的 k=(k,)，行维恒 1）
                    if dim_index == 1:
                        for v in cands:
                            new.update(_reverse_pool(v, k[0], s[0]))
                    else:
                        new = cands
                else:
                    kr = k[0]
                    kc = k[-1]
                    sr = s[0]
                    sc = s[-1]
                    if dim_index == 0:
                        if kr > 1:
                            for v in cands:
                                new.update(_reverse_pool(v, kr, sr))
                        else:
                            new = cands
                    else:
                        if kc > 1:
                            for v in cands:
                                new.update(_reverse_pool(v, kc, sc))
                        else:
                            new = cands
            cands = new
            if len(cands) > max_candidates:
                cands = set(sorted(cands, key=lambda v: abs(v - c))[:max_candidates])
        return cands

    dim = _conv_dim(convs[0][1])
    in_ch = int(convs[0][1].in_channels)
    if dim == 1:
        valid = []
        for L in sorted(reverse_1d_chain(space, 1)):
            if _try_forward(model, (1, L, in_ch)):
                valid.append(L)
        if valid:
            if len(valid) > 1:
                print(f"提示: 反推得到多个可行输入长度 {sorted(valid)}，"
                      f"选用第一个；如需精确匹配训练尺寸请用 --input-shape")
            return (1, valid[0], in_ch)
        return None

    # 2D：特征空间面积分解为 (h_out, w_out)，行列分别沿链反推后再验证
    pairs = []
    for h in range(1, int(space ** 0.5) + 1):
        if space % h == 0:
            pairs.append((h, space // h))
    pairs.sort(key=lambda p: abs(p[0] / p[1] - 1.0))
    for h_out, w_out in pairs:
        h_cands = sorted(reverse_1d_chain(h_out, 0))
        w_cands = sorted(reverse_1d_chain(w_out, 1))
        for h in h_cands:
            for w in w_cands:
                if _try_forward(model, (h, w, in_ch)):
                    print(f"提示: 反推的输入行列可能不唯一（特征面积 {space}），"
                          f"已选用 {h}x{w}；如需精确匹配请用 --input-shape")
                    return (h, w, in_ch)
    return None


def _try_forward(model, shape):
    """用随机输入试跑一次前向，成功返回 True"""
    row, col, ch = shape
    try:
        if row == 1:
            x = torch.rand(1, ch, col)
        else:
            x = torch.rand(1, ch, row, col)
        model.eval()
        with torch.no_grad():
            model(x)
        return True
    except Exception:
        return False


def build_calib_loader(calib_dir, batch=64):
    """扫描 wav 目录生成校准数据（FFT 幅度特征，与工程预处理一致）。
    不依赖 torchaudio/工程文件，便于脚本在任意位置独立运行。"""
    import numpy as np
    try:
        from scipy.io import wavfile
    except ImportError:
        raise SystemExit(
            "--calib-dir 需要 scipy（pip install scipy）。"
            "也可以用 --calib-loader 提供 DataLoader，或 --calib 加载预计算量化参数。")
    from torch.utils.data import Dataset, DataLoader

    class _CalibDS(Dataset):
        def __init__(self, files):
            self.feats = []
            for f in files:
                sr, data = wavfile.read(f)
                if data.ndim > 1:
                    data = data[:, 0]
                w = data.astype(np.float32) / 32768.0
                if sr != 8000:
                    if sr % 8000 == 0:
                        w = w[:: sr // 8000]
                    else:
                        continue
                n = len(w) // 4096
                if n == 0:
                    continue
                segs = w[:n * 4096].reshape(n, 4096)
                mag = np.abs(np.fft.rfft(segs, n=4096))[:, :2048]
                mag = mag / (mag.max(axis=1, keepdims=True) + 1e-9)
                for i in range(n):
                    self.feats.append(torch.from_numpy(mag[i].astype(np.float32)).unsqueeze(0))
            if not self.feats:
                raise SystemExit(f"校准目录 {calib_dir} 中没有可用的 8k/16k/48k wav")

        def __len__(self):
            return len(self.feats)

        def __getitem__(self, i):
            return self.feats[i], 0

    files = []
    for root, _, fs in os.walk(calib_dir):
        for f in sorted(fs):
            if f.lower().endswith(".wav"):
                files.append(os.path.join(root, f))
    if not files:
        raise SystemExit(f"校准目录没有 wav 文件: {calib_dir}")
    return DataLoader(_CalibDS(files), batch_size=batch, shuffle=True, num_workers=0)


def build_random_calib_loader(input_shape, batch=64, n_batches=8):
    """随机输入校准（零依赖回退）：input_shape 为 (row, col, ch)。
    row==1 视为 1D 输入（通道 ch、长度 col），否则 2D（通道 ch、高 row、宽 col）。"""
    from torch.utils.data import DataLoader, TensorDataset
    row, col, ch = input_shape
    if row == 1:
        x = torch.rand(n_batches * batch, ch, col)
    else:
        x = torch.rand(n_batches * batch, ch, row, col)
    y = torch.zeros(n_batches * batch, dtype=torch.long)
    return DataLoader(TensorDataset(x, y), batch_size=batch, shuffle=True)


def load_calib_npz(path):
    """加载预计算量化参数（npz：a0_scale/a0_zp, a1_scale/a1_zp, ...），跳过现场校准。"""
    import numpy as np
    d = np.load(path)
    pts = []
    i = 0
    while f"a{i}_scale" in d:
        pts.append((float(d[f"a{i}_scale"]), int(round(float(d[f"a{i}_zp"])))))
        i += 1
    if not pts:
        raise SystemExit(f"{path} 中没有 a*_scale / a*_zp 量化参数")
    print(f"已加载量化参数: {path}（{len(pts)} 个激活量化点）")
    return pts


def calibrate_any(net, loader, n_batches=400):
    """通用激活校准：输入 + 每个 Conv 输出 + 非最后 Linear 输出。
    量化点顺序与 generate_int8 的 n_act = 1 + n_conv + max(n_lin-1, 0) 对齐，
    适用于任意顺序结构的 CNN（Conv/BN/ReLU/MaxPool/Linear）。"""
    layers = collect_layers(net)
    conv_lin = [l for l in layers if l[0] in ("conv", "linear")]
    n_conv = sum(1 for l in conv_lin if l[0] == "conv")
    n_lin = sum(1 for l in conv_lin if l[0] == "linear")
    n_pts = 1 + n_conv + max(n_lin - 1, 0)
    mins = [1e30] * n_pts
    maxs = [-1e30] * n_pts
    stats = {}
    hooks = []

    def make_hook(mod_id):
        def hook(mod, inp, out):
            o = out[0] if isinstance(out, tuple) else out
            s = stats[mod_id]
            s[0] = min(s[0], float(o.min()))
            s[1] = max(s[1], float(o.max()))
        return hook

    for l in conv_lin:
        m = l[1]
        stats[id(m)] = [1e30, -1e30]
        hooks.append(m.register_forward_hook(make_hook(id(m))))
    net.eval()
    cnt = 0
    with torch.no_grad():
        for x, _ in loader:
            x = x.to(DEVICE)
            mins[0] = min(mins[0], float(x.min()))
            maxs[0] = max(maxs[0], float(x.max()))
            net(x)
            cnt += 1
            if cnt >= n_batches:
                break
    for h in hooks:
        h.remove()

    ai = 1
    for i, l in enumerate(conv_lin):
        if l[0] == "conv" or i < len(conv_lin) - 1:
            s = stats[id(l[1])]
            mins[ai] = s[0]
            maxs[ai] = s[1]
            ai += 1
    if ai != n_pts:
        raise SystemExit(f"校准点数量不一致: 期望 {n_pts}, 得到 {ai}")

    params = []
    for i in range(n_pts):
        scale = (maxs[i] - mins[i]) / 255.0 + 1e-9
        zp = float(torch.clamp(torch.round(torch.tensor(-mins[i] / scale)), 0, 255).item())
        params.append((scale, zp))
    return params


def load_model(args):
    if not args.checkpoint:
        raise SystemExit("需要 --checkpoint <模型文件>")

    # 旧说话人模型的整模型 pickle 引用 __main__.CNNVoiceClassifier，
    # 预注册内嵌类，使 torch.save(model) 的旧格式可直接反序列化
    import __main__
    if not hasattr(__main__, "CNNVoiceClassifier"):
        setattr(__main__, "CNNVoiceClassifier", CNNVoiceClassifier)

    # 直接加载：完整模型对象时 pickle 会自动导入模型类
    # （模型定义文件放在当前目录或 sys.path 中即可，零额外参数）
    try:
        obj = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    except Exception as e:
        raise SystemExit(
            f"无法加载 {args.checkpoint}：{e}\n"
            "常见原因与解决办法：\n"
            "  1) checkpoint 是 torch.save(model) 的完整模型，但模型类定义不在当前 "
            "可导入路径中——请把模型定义文件放到运行目录，或用 --model-class 模块:类名；\n"
            "  2) checkpoint 是 state_dict（只有权重）——需要用 --arch-config 结构.json "
            "或 --model-class 模块:类名 提供结构；\n"
            "  3) 文件损坏或不是 PyTorch 格式。") from e

    # 完整模型对象：直接可用
    if hasattr(obj, "state_dict"):
        return obj, args.checkpoint + " (完整模型对象)"

    # state_dict：必须提供结构信息
    if isinstance(obj, dict):
        sd = obj["state_dict"] if "state_dict" in obj else obj
        if "config" in obj and "state_dict" in obj:
            # 带结构配置的 checkpoint（推荐保存格式）：
            # torch.save({"state_dict":..., "config":...}, ...) 零参数转换
            try:
                model = SpeakerNet(**obj["config"])
                model.load_state_dict(obj["state_dict"])
                return model, args.checkpoint + " (SpeakerNet, 内嵌 config)"
            except Exception:
                pass  # config 不匹配 SpeakerNet 时走下方通用路径
        if args.arch_config:
            import json
            cfg_text = args.arch_config
            if os.path.isfile(cfg_text):
                with open(cfg_text, encoding="utf-8") as f:
                    cfg_text = f.read()
            model = SpeakerNet(**json.loads(cfg_text))
            model.load_state_dict(sd)
            return model, args.checkpoint + " (SpeakerNet, arch-config)"
        if args.model_class:
            import importlib
            mod_name, cls_name = args.model_class.split(":")
            cls = getattr(importlib.import_module(mod_name), cls_name)
            model = cls()
            model.load_state_dict(sd)
            return model, args.checkpoint + " (model-class, state_dict)"
        raise SystemExit(
            "checkpoint 是 state_dict（只有权重，没有网络结构），无法零依赖还原。\n"
            "请选择：\n"
            "  1) 训练时用 torch.save(model, ...) 保存完整模型对象，并把模型定义文件"
            "放在当前目录——之后转换只需 --checkpoint；\n"
            "  2) 用 --arch-config 结构.json 或 --model-class 模块:类名 提供结构。")

    raise SystemExit("无法识别的模型格式（既不是完整模型也不是 state_dict）")


def collect_layers(model):
    """递归遍历模型，返回层描述列表。
    每个元素: (kind, module, 附加参数)
    kind: conv / bn / relu / pool / flatten / linear
    """
    layers = []

    def walk(module):
        for _, child in module.named_children():
            if isinstance(child, nn.Sequential):
                walk(child)
            elif isinstance(child, nn.Conv1d):
                pad = child.padding[0] if isinstance(child.padding, tuple) else child.padding
                layers.append(("conv", child, (1, child.kernel_size[0]), child.stride[0], pad))
            elif isinstance(child, nn.Conv2d):
                pad = child.padding[0] if isinstance(child.padding, tuple) else child.padding
                layers.append(("conv", child,
                               (child.kernel_size[0], child.kernel_size[1]), child.stride[0], pad))
            elif isinstance(child, (nn.BatchNorm1d, nn.BatchNorm2d)):
                layers.append(("bn", child, child.num_features))
            elif isinstance(child, nn.ReLU):
                layers.append(("relu", child, 0.0))
            elif isinstance(child, nn.LeakyReLU):
                layers.append(("relu", child, float(child.negative_slope)))
            elif isinstance(child, nn.Sigmoid):
                layers.append(("sigmoid", child))
            elif isinstance(child, nn.MaxPool1d):
                k = child.kernel_size if isinstance(child.kernel_size, tuple) \
                    else (child.kernel_size,)
                s = child.stride if isinstance(child.stride, tuple) else (child.stride,)
                layers.append(("pool", child, k, s))
            elif isinstance(child, nn.MaxPool2d):
                # 注意：MaxPool2d(2) 的 kernel_size 是 int，必须展开为 (2,2)，
                # 不能与 MaxPool1d(2) 的 (2,) 混淆（否则 2D 行维不会池化）
                k = child.kernel_size if isinstance(child.kernel_size, tuple) \
                    else (child.kernel_size, child.kernel_size)
                s = child.stride if isinstance(child.stride, tuple) \
                    else (child.stride, child.stride)
                layers.append(("pool", child, k, s))
            elif isinstance(child, nn.Flatten):
                layers.append(("flatten", child))
            elif isinstance(child, nn.Dropout):
                continue
            elif isinstance(child, nn.Linear):
                layers.append(("linear", child, child.in_features, child.out_features))
            else:
                if len(list(child.children())) > 0:
                    walk(child)
                else:
                    raise SystemExit(f"不支持的层类型: {type(child).__name__}")

    walk(model)
    return layers


def out_len(length, k, stride, pad=0):
    return (length + 2 * pad - k) // stride + 1


def track_dims(layers, input_shape):
    """逐层跟踪 (row, col, channel)，返回与 layers 等长的 dims 列表"""
    row, col, ch = input_shape
    dims = []
    for l in layers:
        kind = l[0]
        if kind == "conv":
            _, mod, (kr, kc), stride, pad = l
            row = 1 if row == 1 else out_len(row, kr, stride, pad)
            col = out_len(col, kc, stride, pad)
            ch = mod.out_channels
            dims.append(("conv", row, col, ch))
        elif kind == "pool":
            _, mod, k, s = l
            kr = k[0] if len(k) > 1 else 1
            kc = k[-1]
            sr = s[0] if len(s) > 1 else 1
            sc = s[-1]
            row = 1 if row == 1 else out_len(row, kr, sr)
            col = out_len(col, kc, sc)
            dims.append(("pool", row, col, ch))
        elif kind == "linear":
            _, mod, in_f, out_f = l
            row, col, ch = 1, out_f, 1
            dims.append(("linear", row, col, ch))
        else:
            dims.append((kind, row, col, ch))
    return dims


def quantize_conv_int8(conv_mod, bn_mod):
    """BN 折叠 + per-channel int8 权重量化，返回 (wq, wscale, bias)"""
    w = conv_mod.weight.detach().cpu().numpy()
    gamma = bn_mod.weight.detach().cpu().numpy()
    beta = bn_mod.bias.detach().cpu().numpy()
    mean = bn_mod.running_mean.detach().cpu().numpy()
    var = bn_mod.running_var.detach().cpu().numpy()
    scale = gamma / np.sqrt(var + 1e-5)
    if w.ndim == 4:  # Conv2d: (out, in, kh, kw)
        wf = (w * scale[:, None, None, None]).astype(np.float32)
    else:            # Conv1d: (out, in, k)
        wf = (w * scale[:, None, None]).astype(np.float32)
    bias = (beta - mean * scale).astype(np.float32)
    wscale = np.abs(wf).max(axis=tuple(range(1, wf.ndim)), keepdims=True) / 127.0
    wq = np.clip(np.round(wf / wscale), -128, 127).astype(np.int8)
    return wq, wscale.reshape(-1), bias


def quantize_linear_int8(mod):
    """全连接 per-channel int8 权重量化，返回 (wq, wscale, bias)"""
    w = mod.weight.detach().cpu().numpy()
    wscale = np.abs(w).max(axis=1, keepdims=True) / 127.0
    wq = np.clip(np.round(w / wscale), -128, 127).astype(np.int8)
    bias = mod.bias.detach().cpu().numpy().astype(np.float32) if mod.bias is not None \
        else np.zeros(mod.out_features, dtype=np.float32)
    return wq, wscale.reshape(-1), bias


def layer_desc(l):
    """人类可读的层描述，用于生成注释"""
    kind = l[0]
    if kind == "conv":
        mod = l[1]
        kc = l[2][-1]
        return f"{mod.__class__.__name__}({mod.in_channels} -> {mod.out_channels}, k={kc})"
    if kind == "bn":
        return f"BatchNorm({l[2]})"
    if kind == "relu":
        return "LeakyReLU" if l[2] != 0.0 else "ReLU"
    if kind == "sigmoid":
        return "Sigmoid"
    if kind == "pool":
        kc = l[2][-1]
        sc = l[3][-1]
        return f"MaxPool(k={kc}, s={sc})"
    if kind == "flatten":
        return "Flatten"
    if kind == "linear":
        return f"Linear({l[2]} -> {l[3]})"
    return kind


def fmt_float(v):
    return repr(float(np.float32(v)))


def fmt_array(arr, per_line=12, indent="  "):
    vals = [fmt_float(v) for v in np.asarray(arr, dtype=np.float32).reshape(-1)]
    lines = []
    for i in range(0, len(vals), per_line):
        lines.append(indent + ",".join(vals[i:i + per_line]) + ",")
    return "\n".join(lines)


def fmt_i8_array(arr, per_line=24, indent="  "):
    vals = [str(int(v)) for v in np.asarray(arr).reshape(-1)]
    lines = []
    for i in range(0, len(vals), per_line):
        lines.append(indent + ",".join(vals[i:i + per_line]) + ",")
    return "\n".join(lines)


def fmt_struct(fields):
    """生成多行结构体初始化：
    static X x = {
        .a = 1,
        .b = 2,
    };
    """
    lines = ["{"]
    for k, v in fields:
        lines.append(f"    .{k} = {v},")
    lines.append("};")
    return "\n".join(lines)


def buf_size_expr(r, c, chn):
    return f"{chn} * {r} * {c}" if r != 1 else f"{chn} * {c}"


def generate(model, name, input_shape, out_dir):
    layers = collect_layers(model)
    os.makedirs(out_dir, exist_ok=True)
    base = os.path.join(out_dir, name)

    # ---------- 维度跟踪 ----------
    row, col, ch = input_shape
    dims = []
    for l in layers:
        kind = l[0]
        if kind == "conv":
            _, mod, (kr, kc), stride, pad = l
            row = 1 if row == 1 else out_len(row, kr, stride, pad)
            col = out_len(col, kc, stride, pad)
            ch = mod.out_channels
            dims.append(("conv", row, col, ch))
        elif kind == "pool":
            _, mod, k, s = l
            kr = k[0] if len(k) > 1 else 1
            kc = k[-1]
            sr = s[0] if len(s) > 1 else 1
            sc = s[-1]
            row = 1 if row == 1 else out_len(row, kr, sr)
            col = out_len(col, kc, sc)
            dims.append(("pool", row, col, ch))
        elif kind == "linear":
            _, mod, in_f, out_f = l
            row, col, ch = 1, out_f, 1
            dims.append(("linear", row, col, ch))
        else:
            dims.append((kind, row, col, ch))

    # ---------- 校验 ----------
    for i, l in enumerate(layers):
        if l[0] == "conv" and (i + 1 >= len(layers) or layers[i + 1][0] != "bn"):
            raise SystemExit("每个 Conv 层后必须紧跟 BatchNorm（库接口要求）")

    pidx = 0  # 参数层序号（conv/linear）
    cidx = 0  # 卷积组序号
    bidx = 0  # 缓冲区序号

    # ---------- param.h ----------
    param_h = [f"#ifndef __{name.upper()}_PARAM_H__\n#define __{name.upper()}_PARAM_H__\n\n"]
    param_h.append("/* 自动生成：模型参数声明 */\n\n")

    # ---------- param.c ----------
    param_c = [f'#include "{name}_param.h"\n\n']
    param_c.append("/* 自动生成：模型参数定义 */\n\n")

    for li, l in enumerate(layers):
        kind = l[0]
        if kind == "conv":
            mod = l[1]
            bn = layers[li + 1][1]
            tag = f"l{pidx}"
            w = mod.weight.detach().numpy()
            param_c.append(f"/* 层 {li}: {layer_desc(l)} + BatchNorm({mod.out_channels}) 参数 */\n")
            param_c.append(f"const float {tag}_weight[{w.size}] = {{\n{fmt_array(w)}\n}};\n\n")
            for pn, arr in (("bn_gamma", bn.weight), ("bn_beta", bn.bias),
                            ("bn_mean", bn.running_mean), ("bn_var", bn.running_var)):
                a = arr.detach().numpy()
                param_c.append(f"const float {tag}_{pn}[{a.size}] = {{\n{fmt_array(a)}\n}};\n\n")
            param_h.append(f"/* 层 {li}: {layer_desc(l)} + BN */\n")
            for pn in ("weight", "bn_gamma", "bn_beta", "bn_mean", "bn_var"):
                param_h.append(f"extern const float {tag}_{pn}[];\n")
            param_h.append("\n")
            pidx += 1
        elif kind == "linear":
            mod = l[1]
            tag = f"l{pidx}"
            w = mod.weight.detach().numpy()
            b = mod.bias.detach().numpy() if mod.bias is not None \
                else np.zeros(mod.out_features, dtype=np.float32)
            param_c.append(f"/* 层 {li}: {layer_desc(l)} 参数 */\n")
            param_c.append(f"const float {tag}_weight[{w.size}] = {{\n{fmt_array(w)}\n}};\n\n")
            param_c.append(f"const float {tag}_bias[{b.size}] = {{\n{fmt_array(b)}\n}};\n\n")
            param_h.append(f"/* 层 {li}: {layer_desc(l)} */\n")
            param_h.append(f"extern const float {tag}_weight[];\n")
            param_h.append(f"extern const float {tag}_bias[];\n\n")
            pidx += 1
    param_h.append("\n#endif\n")

    # ---------- app.h ----------
    input_size = input_shape[0] * input_shape[1] * input_shape[2]
    if layers[-1][0] == "linear":
        output_size = layers[-1][3]
    else:
        _, r, c, chn = dims[-1]
        output_size = chn * r * c
    app_h = [f"#ifndef __{name.upper()}_APP_H__\n#define __{name.upper()}_APP_H__\n\n"]
    app_h.append("/*\n")
    app_h.append(f" * 自动生成：{name} 前向推理接口\n")
    app_h.append(f" * 用法：包含本头文件，调用 {name}_forward()\n")
    app_h.append(" */\n\n")
    app_h.append("#include <stdint.h>\n\n")
    app_h.append(f"#define {name.upper()}_INPUT_SIZE  {input_size}\n")
    app_h.append(f"#define {name.upper()}_OUTPUT_SIZE {output_size}\n")
    app_h.append(f"#define {name.upper()}_INPUT_ROW   {input_shape[0]}\n")
    app_h.append(f"#define {name.upper()}_INPUT_COL   {input_shape[1]}\n")
    app_h.append(f"#define {name.upper()}_INPUT_CH    {input_shape[2]}\n\n")
    app_h.append(f"#define {name}_INPUT_SIZE  {name.upper()}_INPUT_SIZE\n")
    app_h.append(f"#define {name}_OUTPUT_SIZE {name.upper()}_OUTPUT_SIZE\n\n")
    app_h.append("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
    app_h.append(f"/* 前向推理：input 为长度 {name.upper()}_INPUT_SIZE 的浮点特征；\n")
    app_h.append(f" * logits 输出 {name.upper()}_OUTPUT_SIZE 个值；class_id 输出类别（可传 NULL） */\n")
    app_h.append(f"int {name}_forward(const float *input, float *logits, uint32_t *class_id);\n\n")
    app_h.append("#ifdef __cplusplus\n}\n#endif\n\n#endif\n")

    # ---------- app.c ----------
    app_c = [f'#include "{name}_app.h"\n']
    app_c.append(f'#include "{name}_param.h"\n')
    app_c.append('#include "conv.h"\n')
    app_c.append('#include <string.h>\n\n')
    app_c.append("/* 自动生成：模型结构与运行代码 */\n\n")

    # 结构体与缓冲区
    pidx = 0
    cidx = 0
    pool_idx = 0
    linear_idx = 0
    for li, l in enumerate(layers):
        kind = l[0]
        if kind == "conv":
            mod = l[1]
            _, r, c, chn = dims[li]
            kr, kc = l[2]
            tag = f"l{pidx}"
            buf = f"buf_{bidx}"
            app_c.append(f"/* 层 {li}: {layer_desc(l)} + BN，输出 {chn} x {r} x {c} */\n")
            app_c.append(f"static float {buf}[{buf_size_expr(r, c, chn)}];\n\n")
            app_c.append("static Conv2dFilter filter_%d = %s\n\n" % (
                cidx, fmt_struct([
                    ("row", l[2][0]),
                    ("col", l[2][-1]),
                    ("channel", mod.in_channels),
                    ("filter_num", mod.out_channels),
                    ("data", f"(float *){tag}_weight"),
                ])))
            app_c.append("static BatchNorm2d bn_%d = %s\n\n" % (
                cidx, fmt_struct([
                    ("size", mod.out_channels),
                    ("mean", f"(float *){tag}_bn_mean"),
                    ("var", f"(float *){tag}_bn_var"),
                    ("gamma", f"(float *){tag}_bn_gamma"),
                    ("beta", f"(float *){tag}_bn_beta"),
                ])))
            app_c.append("static Conv2dConfig cfg_%d = %s\n\n" % (
                cidx, fmt_struct([
                    ("stride", l[3]),
                    ("pad", l[4]),
                    ("filter", f"&filter_{cidx}"),
                    ("bn", f"&bn_{cidx}"),
                ])))
            cidx += 1
            pidx += 1
            bidx += 1
        elif kind == "pool":
            k, s = l[2], l[3]
            kr = k[0] if len(k) > 1 else 1
            kc = k[-1]
            sc = s[-1]
            app_c.append(f"/* 层 {li}: {layer_desc(l)} */\n")
            app_c.append("static MaxPoolConfig pool_%d = %s\n\n" % (
                pool_idx, fmt_struct([
                    ("row", kr),
                    ("col", kc),
                    ("stride", sc),
                    ("pad", 0),
                ])))
            pool_idx += 1
        elif kind == "linear":
            mod = l[1]
            tag = f"l{pidx}"
            buf = f"buf_{bidx}"
            app_c.append(f"/* 层 {li}: {layer_desc(l)} */\n")
            app_c.append(f"static float {buf}[{mod.out_features}];\n\n")
            app_c.append("static LinearParam fc_%d = %s\n\n" % (
                linear_idx, fmt_struct([
                    ("inp_size", mod.in_features),
                    ("fea_size", mod.out_features),
                    ("weight", f"(float *){tag}_weight"),
                    ("bias", f"(float *){tag}_bias"),
                ])))
            linear_idx += 1
            pidx += 1
            bidx += 1

    # 前向实现
    app_c.append(f"int {name}_forward(const float *input, float *logits, uint32_t *class_id)\n")
    app_c.append("{\n")
    app_c.append("  Conv2dData x, y;\n")
    app_c.append("  int ret;\n\n")
    app_c.append("  /* 输入 */\n")
    app_c.append(f"  x.row = {name.upper()}_INPUT_ROW;\n")
    app_c.append(f"  x.col = {name.upper()}_INPUT_COL;\n")
    app_c.append(f"  x.channel = {name.upper()}_INPUT_CH;\n")
    app_c.append("  x.data = (float *)input;\n\n")

    pidx = 0
    cidx = 0
    pool_idx = 0
    linear_idx = 0
    bidx = 0
    for li, l in enumerate(layers):
        kind = l[0]
        if kind == "conv":
            buf = f"buf_{bidx}"
            app_c.append(f"  /* 层 {li}: {layer_desc(l)} + BN */\n")
            app_c.append(f"  y.data = {buf};\n")
            app_c.append(f"  ret = conv2d_bn_no_bias(&x, &cfg_{cidx}, &y);\n")
            app_c.append("  if (ret != CNN_NORMAL) return ret;\n")
            app_c.append("  x = y;\n\n")
            cidx += 1
            pidx += 1
            bidx += 1
        elif kind == "relu":
            slope = l[2]
            app_c.append(f"  /* 层 {li}: {layer_desc(l)} */\n")
            app_c.append(f"  ret = leaky_relu({slope}f, x.data, x.channel * x.row * x.col, x.data);\n")
            app_c.append("  if (ret != CNN_NORMAL) return ret;\n\n")
        elif kind == "sigmoid":
            # fp32 支持 Sigmoid（库内 sigmoid 为原地 void 函数）
            app_c.append(f"  /* 层 {li}: {layer_desc(l)} */\n")
            app_c.append(f"  sigmoid(x.data, x.channel * x.row * x.col);\n\n")
        elif kind == "pool":
            app_c.append(f"  /* 层 {li}: {layer_desc(l)} */\n")
            app_c.append(f"  ret = maxpool(&x, &pool_{pool_idx}, &x);\n")
            app_c.append("  if (ret != CNN_NORMAL) return ret;\n\n")
            pool_idx += 1
        elif kind == "flatten":
            app_c.append(f"  /* 层 {li}: Flatten（缓冲区连续，无需复制） */\n\n")
        elif kind == "linear":
            buf = f"buf_{bidx}"
            app_c.append(f"  /* 层 {li}: {layer_desc(l)} */\n")
            app_c.append(f"  ret = linear_layer(x.data, &fc_{linear_idx}, {buf});\n")
            app_c.append("  if (ret != CNN_NORMAL) return ret;\n")
            app_c.append(f"  x.data = {buf};\n")
            app_c.append("  x.row = 1;\n")
            app_c.append(f"  x.col = fc_{linear_idx}.fea_size;\n")
            app_c.append("  x.channel = 1;\n\n")
            linear_idx += 1
            bidx += 1

    app_c.append("  /* 输出 */\n")
    app_c.append(f"  if (logits) memcpy(logits, x.data, {output_size} * sizeof(float));\n")
    app_c.append(f"  if (class_id) *class_id = argmax(x.data, {output_size}, NULL);\n")
    app_c.append("  return CNN_NORMAL;\n")
    app_c.append("}\n")

    with open(base + "_param.h", "w", encoding="utf-8") as f:
        f.write("".join(param_h))
    with open(base + "_param.c", "w", encoding="utf-8") as f:
        f.write("".join(param_c))
    with open(base + "_app.h", "w", encoding="utf-8") as f:
        f.write("".join(app_h))
    with open(base + "_app.c", "w", encoding="utf-8") as f:
        f.write("".join(app_c))

    print(f"模型名: {name}")
    print(f"输出目录: {out_dir}")
    print(f"生成文件: {name}_param.h / {name}_param.c / {name}_app.h / {name}_app.c")
    print(f"结构: {[(l[0], layer_desc(l)) for l in layers]}")
    print(f"输入形状: {input_shape}, 输入大小: {input_size}")


def generate_int8(model, name, input_shape, out_dir, act_params):
    """生成 int8 量化版 param/app：
    - param：int8 权重 + per-channel scale + 折叠 bias + 激活 scale/zp
    - app：使用库内通用 int8 算子（conv1d_int8/maxpool1d_u8/linear_int8）
    接口与 fp32 版一致：<name>_forward(input, logits, class_id)
    """
    layers = collect_layers(model)
    dims = track_dims(layers, input_shape)
    n_conv = sum(1 for l in layers if l[0] == "conv")
    n_lin = sum(1 for l in layers if l[0] == "linear")
    n_act = 1 + n_conv + max(n_lin - 1, 0)

    if len(act_params) != n_act:
        raise SystemExit(f"int8 生成需要 {n_act} 个激活量化点，校准得到 {len(act_params)} 个")
    for i, l in enumerate(layers):
        if l[0] == "conv" and (i + 1 >= len(layers) or layers[i + 1][0] != "bn"):
            raise SystemExit("每个 Conv 层后必须紧跟 BatchNorm（库接口要求）")
        if l[0] == "sigmoid":
            raise SystemExit("int8 量化不支持 Sigmoid 激活（fp32 导出可用）。"
                             "请改用 ReLU/LeakyReLU，或仅用 fp32 导出。")
        if l[0] == "relu" and l[2] != 0.0:
            raise SystemExit(f"int8 量化只支持 ReLU（当前 LeakyReLU 负斜率 {l[2]}）。"
                             "LeakyReLU 输出含负值，无法在 uint8 链路中表达；"
                             "请改用 ReLU 或用 fp32 导出。")

    os.makedirs(out_dir, exist_ok=True)
    base = os.path.join(out_dir, name)
    input_size = input_shape[0] * input_shape[1] * input_shape[2]
    if layers[-1][0] == "linear":
        output_size = layers[-1][3]
    else:
        _, r, c, chn = dims[-1]
        output_size = chn * r * c

    # ---------- 量化参数 ----------
    conv_params = []
    lin_params = []
    pidx = 0
    for li, l in enumerate(layers):
        kind = l[0]
        if kind == "conv":
            bn = layers[li + 1][1]
            conv_params.append((f"{name}_l{pidx}",) + quantize_conv_int8(l[1], bn))
            pidx += 1
        elif kind == "linear":
            lin_params.append((f"{name}_l{pidx}",) + quantize_linear_int8(l[1]))
            pidx += 1

    param_h = [f"#ifndef __{name.upper()}_PARAM_H__\n#define __{name.upper()}_PARAM_H__\n\n"]
    param_h.append("/* 自动生成：int8 量化模型参数声明 */\n\n")
    param_h.append("#include <stdint.h>\n\n")
    param_c = [f'#include "{name}_param.h"\n\n']
    param_c.append("/* 自动生成：int8 量化模型参数定义 */\n\n")

    for tag, wq, ws, b in conv_params + lin_params:
        param_c.append(f"const int8_t {tag}_weight[{wq.size}] = {{\n{fmt_i8_array(wq)}\n}};\n\n")
        param_c.append(f"const float {tag}_wscale[{ws.size}] = {{\n{fmt_array(ws)}\n}};\n\n")
        param_c.append(f"const float {tag}_bias[{b.size}] = {{\n{fmt_array(b)}\n}};\n\n")
        param_h.append(f"extern const int8_t {tag}_weight[];\n")
        param_h.append(f"extern const float {tag}_wscale[];\n")
        param_h.append(f"extern const float {tag}_bias[];\n\n")
    param_c.append(f"const float {name}_act_scale[{n_act}] = {{ {', '.join(fmt_float(s) for s, _ in act_params)} }};\n\n")
    param_c.append(f"const int {name}_act_zp[{n_act}] = {{ {', '.join(str(zp) for _, zp in act_params)} }};\n\n")
    param_h.append(f"extern const float {name}_act_scale[];\n")
    param_h.append(f"extern const int {name}_act_zp[];\n\n")
    param_h.append("\n#endif\n")

    # ---------- app.h ----------
    app_h = [f"#ifndef __{name.upper()}_APP_H__\n#define __{name.upper()}_APP_H__\n\n"]
    app_h.append("/*\n")
    app_h.append(f" * 自动生成：{name} 前向推理接口（int8 量化版）\n")
    app_h.append(f" * 用法：包含本头文件，调用 {name}_forward()\n")
    app_h.append(" */\n\n")
    app_h.append("#include <stdint.h>\n\n")
    app_h.append(f"#define {name.upper()}_INPUT_SIZE  {input_size}\n")
    app_h.append(f"#define {name.upper()}_OUTPUT_SIZE {output_size}\n")
    app_h.append(f"#define {name.upper()}_INPUT_ROW   {input_shape[0]}\n")
    app_h.append(f"#define {name.upper()}_INPUT_COL   {input_shape[1]}\n")
    app_h.append(f"#define {name.upper()}_INPUT_CH    {input_shape[2]}\n\n")
    app_h.append(f"#define {name}_INPUT_SIZE  {name.upper()}_INPUT_SIZE\n")
    app_h.append(f"#define {name}_OUTPUT_SIZE {name.upper()}_OUTPUT_SIZE\n\n")
    app_h.append("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
    app_h.append(f"/* 输入量化参数（来自模型校准）：把浮点特征量化成 uint8 时使用 */\n")
    app_h.append(f"extern const float {name}_input_scale;\n")
    app_h.append(f"extern const int {name}_input_zp;\n\n")
    app_h.append(f"/* uint8 直入入口：input 为已按 input_scale/input_zp 量化的 {name.upper()}_INPUT_SIZE 个 uint8 */\n")
    app_h.append(f"int {name}_forward_u8(const uint8_t *input, float *logits, uint32_t *class_id);\n\n")
    app_h.append(f"/* fp32 便捷入口：内部先按 input_scale/input_zp 量化再推理 */\n")
    app_h.append(f"int {name}_forward(const float *input, float *logits, uint32_t *class_id);\n\n")
    app_h.append("#ifdef __cplusplus\n}\n#endif\n\n#endif\n")

    # ---------- app.c ----------
    app_c = [f'#include "{name}_app.h"\n']
    app_c.append(f'#include "{name}_param.h"\n')
    app_c.append('#include "conv.h"\n')
    app_c.append('#include <string.h>\n\n')
    app_c.append("/* 自动生成：int8 量化模型结构与运行代码 */\n\n")

    # 缓冲区声明
    app_c.append(f"static uint8_t q_in[{input_size}];\n")
    conv_bufs = []
    lin_fbufs = []
    lin_qbufs = []
    ci = 0
    li = 0
    for li_, l in enumerate(layers):
        kind = l[0]
        if kind == "conv":
            _, r, c, chn = dims[li_]
            cbuf = f"cbuf_{ci}"
            conv_bufs.append(cbuf)
            app_c.append(f"static uint8_t {cbuf}[{buf_size_expr(r, c, chn)}];\n")
            ci += 1
        elif kind == "linear":
            out_f = l[3]
            if li < n_lin - 1:
                fbuf = f"fbuf_{len(lin_fbufs)}"
                qbuf = f"qbuf_{len(lin_qbufs)}"
                lin_fbufs.append(fbuf)
                lin_qbufs.append(qbuf)
                app_c.append(f"static float {fbuf}[{out_f}];\n")
                app_c.append(f"static uint8_t {qbuf}[{out_f}];\n")
            li += 1
    app_c.append(f"static float logits_buf[{output_size}];\n")
    app_c.append(f"/* 输入量化参数（来自模型校准，即 act_scale[0]/act_zp[0]） */\n")
    app_c.append(f"const float {name}_input_scale = {fmt_float(act_params[0][0])}f;\n")
    app_c.append(f"const int {name}_input_zp = {int(act_params[0][1])};\n\n")

    # 前向核心（uint8 直入）
    app_c.append(f"static int {name}_core(const uint8_t *q_in, float *logits, uint32_t *class_id)\n")
    app_c.append("{\n")
    app_c.append("  uint32_t i;\n")
    app_c.append("  int ret;\n\n")
    app_c.append("  if (!q_in) return CNN_POINTER_NULL;\n\n")

    cur = "q_in"
    cur_row = input_shape[0]
    cur_col = input_shape[1]
    cur_act = 0
    ci = 0
    li = 0
    for li_, l in enumerate(layers):
        kind = l[0]
        if kind == "conv":
            mod = l[1]
            tag = conv_params[ci][0]
            _, r, c, chn = dims[li_]
            cbuf = conv_bufs[ci]
            app_c.append(f"  /* 层 {li_}: {layer_desc(l)} + BN（ReLU 内置于卷积） */\n")
            app_c.append(f"  ret = conv2d_int8({cur}, {cur_row}, {cur_col}, {mod.in_channels}, {tag}_weight, {l[2][0]}, {l[2][-1]}, {l[3]}, {l[4]}, {mod.out_channels},\n")
            app_c.append(f"                      {tag}_wscale, {tag}_bias,\n")
            app_c.append(f"                      {name}_act_scale[{cur_act}], {name}_act_zp[{cur_act}],\n")
            app_c.append(f"                      {name}_act_scale[{1 + ci}], {name}_act_zp[{1 + ci}], {cbuf});\n")
            app_c.append("  if (ret != CNN_NORMAL) return ret;\n\n")
            cur = cbuf
            cur_row = r
            cur_col = c
            cur_act = 1 + ci
            ci += 1
        elif kind == "pool":
            k = l[2]
            s = l[3]
            kc = k[-1]
            sc = s[-1]
            _, r, c, chn = dims[li_]
            app_c.append(f"  /* 层 {li_}: {layer_desc(l)}（uint8 域取 max） */\n")
            app_c.append(f"  for (i = 0; i < {chn}; i++) {{\n")
            # 原地池化：输出紧凑写回同一缓冲（先读后写 + 升序，任意 stride 安全），
            # 不再为池化分配独立缓冲，节省 RAM
            app_c.append(f"    maxpool_u8({cur} + i * {cur_col}, 1, {cur_col}, {cur} + i * {c}, 1, {kc}, {sc});\n")
            app_c.append("  }\n\n")
            cur_row = r
            cur_col = c
        elif kind == "flatten":
            app_c.append(f"  /* 层 {li_}: Flatten（缓冲区连续，无需复制） */\n\n")
        elif kind == "linear":
            tag = lin_params[li][0]
            in_f = l[2]
            out_f = l[3]
            if li < n_lin - 1:
                fbuf = lin_fbufs[li]
                qbuf = lin_qbufs[li]
                app_c.append(f"  /* 层 {li_}: {layer_desc(l)} */\n")
                app_c.append(f"  ret = linear_int8({cur}, {in_f}, {tag}_weight, {tag}_wscale, {tag}_bias, {out_f},\n")
                app_c.append(f"                     {name}_act_scale[{cur_act}], {name}_act_zp[{cur_act}], {fbuf});\n")
                app_c.append("  if (ret != CNN_NORMAL) return ret;\n")
                app_c.append(f"  quantize_u8({fbuf}, {out_f}, {name}_act_scale[{1 + n_conv + li}], {name}_act_zp[{1 + n_conv + li}], {qbuf});\n\n")
                cur = qbuf
                cur_col = out_f
                cur_act = 1 + n_conv + li
            else:
                app_c.append(f"  /* 层 {li_}: {layer_desc(l)}（输出 logits） */\n")
                app_c.append(f"  ret = linear_int8({cur}, {in_f}, {tag}_weight, {tag}_wscale, {tag}_bias, {out_f},\n")
                app_c.append(f"                     {name}_act_scale[{cur_act}], {name}_act_zp[{cur_act}], logits_buf);\n")
                app_c.append("  if (ret != CNN_NORMAL) return ret;\n\n")
            li += 1

    app_c.append("  /* 输出 */\n")
    if layers[-1][0] != "linear":
        # conv 结尾：特征图为 uint8 量化域，先反量化到 float logits_buf
        app_c.append(f"  for (i = 0; i < {output_size}; i++) {{\n")
        app_c.append(f"    logits_buf[i] = ((float){conv_bufs[-1]}[i] - {name}_act_zp[{n_conv}]) "
                     f"* {name}_act_scale[{n_conv}];\n")
        app_c.append("  }\n")
    app_c.append(f"  if (logits) memcpy(logits, logits_buf, {output_size} * sizeof(float));\n")
    app_c.append(f"  if (class_id) *class_id = argmax(logits_buf, {output_size}, NULL);\n")
    app_c.append("  return CNN_NORMAL;\n")
    app_c.append("}\n")
    app_c.append("\n")
    app_c.append(f"int {name}_forward_u8(const uint8_t *input, float *logits, uint32_t *class_id)\n")
    app_c.append("{\n")
    app_c.append("    if (!input) return CNN_POINTER_NULL;\n")
    app_c.append(f"    return {name}_core(input, logits, class_id);\n")
    app_c.append("}\n")
    app_c.append("\n")
    app_c.append(f"int {name}_forward(const float *input, float *logits, uint32_t *class_id)\n")
    app_c.append("{\n")
    app_c.append("    if (!input) return CNN_POINTER_NULL;\n")
    app_c.append(f"    quantize_u8(input, {name.upper()}_INPUT_SIZE, {name}_input_scale, {name}_input_zp, q_in);\n")
    app_c.append(f"    return {name}_core(q_in, logits, class_id);\n")
    app_c.append("}\n")

    with open(base + "_param.h", "w", encoding="utf-8") as f:
        f.write("".join(param_h))
    with open(base + "_param.c", "w", encoding="utf-8") as f:
        f.write("".join(param_c))
    with open(base + "_app.h", "w", encoding="utf-8") as f:
        f.write("".join(app_h))
    with open(base + "_app.c", "w", encoding="utf-8") as f:
        f.write("".join(app_c))

    # 保存 npz（与 verify_int8.py 兼容的字段格式）
    data = {}
    for i, (tag, wq, ws, b) in enumerate(conv_params + lin_params):
        data[f"w{i}_int8"] = wq
        data[f"w{i}_scale"] = ws
        data[f"b{i}"] = b
    for i, (s, zp) in enumerate(act_params):
        data[f"a{i}_scale"] = np.float32(s)
        data[f"a{i}_zp"] = np.float32(zp)
    npz_path = os.path.join(out_dir, name + "_quant.npz")
    np.savez(npz_path, **data)

    print(f"模型名: {name}（int8 量化）")
    print(f"输出目录: {out_dir}")
    print(f"生成文件: {name}_param.h / {name}_param.c / {name}_app.h / {name}_app.c")
    print(f"量化参数: {npz_path}")


def generate_int8_dynamic(model, name, input_shape, out_dir):
    """生成动态量化版 C 代码：每层卷积输出在线找 max 量化，无需任何校准数据。

    - param：int8 权重 + per-channel scale + 折叠 bias（无激活 scale/zp）；
    - app：调用 conv2d_int8_dynamic 链式传递 scale；
      中间 Linear 输出做动态非对称量化；最后输出层（Linear 或 Conv）输出 fp32。
    """
    layers = collect_layers(model)
    dims = track_dims(layers, input_shape)
    n_conv = sum(1 for l in layers if l[0] == "conv")
    n_lin = sum(1 for l in layers if l[0] == "linear")

    for i, l in enumerate(layers):
        if l[0] == "conv" and (i + 1 >= len(layers) or layers[i + 1][0] != "bn"):
            raise SystemExit("每个 Conv 层后必须紧跟 BatchNorm（库接口要求）")
        if l[0] == "sigmoid":
            raise SystemExit("int8 量化不支持 Sigmoid 激活（fp32 导出可用）。"
                             "请改用 ReLU/LeakyReLU，或仅用 fp32 导出。")
        if l[0] == "relu" and l[2] != 0.0:
            raise SystemExit(f"int8 量化只支持 ReLU（当前 LeakyReLU 负斜率 {l[2]}）。"
                             "LeakyReLU 输出含负值，无法在 uint8 链路中表达；"
                             "请改用 ReLU 或用 fp32 导出。")

    os.makedirs(out_dir, exist_ok=True)
    base = os.path.join(out_dir, name)
    input_size = input_shape[0] * input_shape[1] * input_shape[2]
    if layers[-1][0] == "linear":
        output_size = layers[-1][3]
    else:
        _, r, c, chn = dims[-1]
        output_size = chn * r * c

    # ---------- 量化参数（无激活参数） ----------
    conv_params = []
    lin_params = []
    pidx = 0
    for li, l in enumerate(layers):
        kind = l[0]
        if kind == "conv":
            bn = layers[li + 1][1]
            conv_params.append((f"{name}_l{pidx}",) + quantize_conv_int8(l[1], bn))
            pidx += 1
        elif kind == "linear":
            lin_params.append((f"{name}_l{pidx}",) + quantize_linear_int8(l[1]))
            pidx += 1

    param_h = [f"#ifndef __{name.upper()}_PARAM_H__\n#define __{name.upper()}_PARAM_H__\n\n"]
    param_h.append("/* 自动生成：int8 动态量化模型参数声明 */\n\n")
    param_h.append("#include <stdint.h>\n\n")
    param_c = [f'#include "{name}_param.h"\n\n']
    param_c.append("/* 自动生成：int8 动态量化模型参数定义（无激活 scale/zp，"
                   "激活 scale 每帧在线计算） */\n\n")
    for tag, wq, ws, b in conv_params + lin_params:
        param_c.append(f"const int8_t {tag}_weight[{wq.size}] = {{\n{fmt_i8_array(wq)}\n}};\n\n")
        param_c.append(f"const float {tag}_wscale[{ws.size}] = {{\n{fmt_array(ws)}\n}};\n\n")
        param_c.append(f"const float {tag}_bias[{b.size}] = {{\n{fmt_array(b)}\n}};\n\n")
        param_h.append(f"extern const int8_t {tag}_weight[];\n")
        param_h.append(f"extern const float {tag}_wscale[];\n")
        param_h.append(f"extern const float {tag}_bias[];\n\n")
    param_h.append("\n#endif\n")

    # ---------- app.h ----------
    app_h = [f"#ifndef __{name.upper()}_APP_H__\n#define __{name.upper()}_APP_H__\n\n"]
    app_h.append("/*\n")
    app_h.append(f" * 自动生成：{name} 前向推理接口（int8 动态量化版）\n")
    app_h.append(" * 激活 scale 每帧在线计算，无需校准数据。\n")
    app_h.append(" */\n\n")
    app_h.append("#include <stdint.h>\n\n")
    app_h.append(f"#define {name.upper()}_INPUT_SIZE  {input_size}\n")
    app_h.append(f"#define {name.upper()}_OUTPUT_SIZE {output_size}\n")
    app_h.append(f"#define {name.upper()}_INPUT_ROW   {input_shape[0]}\n")
    app_h.append(f"#define {name.upper()}_INPUT_COL   {input_shape[1]}\n")
    app_h.append(f"#define {name.upper()}_INPUT_CH    {input_shape[2]}\n\n")
    app_h.append(f"#define {name}_INPUT_SIZE  {name.upper()}_INPUT_SIZE\n")
    app_h.append(f"#define {name}_OUTPUT_SIZE {name.upper()}_OUTPUT_SIZE\n\n")
    app_h.append("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
    app_h.append("/* 输入固定量化：float 输入按 1/255、zp=0 量化（图像/特征范围 [0,1]） */\n")
    app_h.append(f"int {name}_forward_u8(const uint8_t *input, float *logits, uint32_t *class_id);\n")
    app_h.append(f"int {name}_forward(const float *input, float *logits, uint32_t *class_id);\n\n")
    app_h.append("#ifdef __cplusplus\n}\n#endif\n\n#endif\n")

    # ---------- app.c ----------
    app_c = [f'#include "{name}_app.h"\n']
    app_c.append(f'#include "{name}_param.h"\n')
    app_c.append('#include "conv.h"\n')
    app_c.append('#include <string.h>\n\n')
    app_c.append("/* 自动生成：int8 动态量化模型结构与运行代码 */\n\n")

    # 缓冲区声明
    app_c.append(f"static uint8_t q_in[{input_size}];\n")
    conv_bufs = []
    lin_fbufs = []
    lin_qbufs = []
    ci = 0
    li = 0
    for li_, l in enumerate(layers):
        kind = l[0]
        if kind == "conv":
            _, r, c, chn = dims[li_]
            cbuf = f"cbuf_{ci}"
            conv_bufs.append(cbuf)
            app_c.append(f"static uint8_t {cbuf}[{buf_size_expr(r, c, chn)}];\n")
            ci += 1
        elif kind == "linear":
            out_f = l[3]
            if li < n_lin - 1:
                fbuf = f"fbuf_{len(lin_fbufs)}"
                qbuf = f"qbuf_{len(lin_qbufs)}"
                lin_fbufs.append(fbuf)
                lin_qbufs.append(qbuf)
                app_c.append(f"static float {fbuf}[{out_f}];\n")
                app_c.append(f"static uint8_t {qbuf}[{out_f}];\n")
            li += 1
    app_c.append(f"static float logits_buf[{output_size}];\n\n")

    # 前向核心（uint8 直入）
    app_c.append(f"static int {name}_core(const uint8_t *q_in, float *logits, uint32_t *class_id)\n")
    app_c.append("{\n")
    app_c.append("  uint32_t i;\n")
    app_c.append("  int ret;\n")
    app_c.append("  float act_scale;\n")
    if n_lin > 1:
        app_c.append("  float fmin, fmax, fscale;\n")
        app_c.append("  int fzp;\n")
    app_c.append("\n")
    app_c.append("  if (!q_in) return CNN_POINTER_NULL;\n\n")

    cur = "q_in"
    cur_row = input_shape[0]
    cur_col = input_shape[1]
    cur_act_scale = "0.00392156863f"  # 输入固定 scale=1/255
    cur_act_zp = "0"
    ci = 0
    li = 0
    for li_, l in enumerate(layers):
        kind = l[0]
        if kind == "conv":
            mod = l[1]
            tag = conv_params[ci][0]
            _, r, c, chn = dims[li_]
            cbuf = conv_bufs[ci]
            app_c.append(f"  /* 层 {li_}: {layer_desc(l)} + BN（动态量化） */\n")
            app_c.append(f"  ret = conv2d_int8_dynamic({cur}, {cur_row}, {cur_col}, {mod.in_channels}, {tag}_weight, {l[2][0]}, {l[2][-1]}, {l[3]}, {l[4]}, {mod.out_channels},\n")
            app_c.append(f"                      {tag}_wscale, {tag}_bias,\n")
            app_c.append(f"                      {cur_act_scale}, {cur_act_zp}, &act_scale, {cbuf});\n")
            app_c.append("  if (ret != CNN_NORMAL) return ret;\n\n")
            cur = cbuf
            cur_row = r
            cur_col = c
            cur_act_scale = "act_scale"
            cur_act_zp = "0"
            ci += 1
        elif kind == "pool":
            k = l[2]
            s = l[3]
            kc = k[-1]
            sc = s[-1]
            _, r, c, chn = dims[li_]
            app_c.append(f"  /* 层 {li_}: {layer_desc(l)}（uint8 域取 max） */\n")
            app_c.append(f"  for (i = 0; i < {chn}; i++) {{\n")
            app_c.append(f"    maxpool_u8({cur} + i * {cur_col}, 1, {cur_col}, {cur} + i * {c}, 1, {kc}, {sc});\n")
            app_c.append("  }\n\n")
            cur_row = r
            cur_col = c
        elif kind == "flatten":
            app_c.append(f"  /* 层 {li_}: Flatten（缓冲区连续，无需复制） */\n\n")
        elif kind == "linear":
            tag = lin_params[li][0]
            in_f = l[2]
            out_f = l[3]
            if li < n_lin - 1:
                fbuf = lin_fbufs[li]
                qbuf = lin_qbufs[li]
                app_c.append(f"  /* 层 {li_}: {layer_desc(l)}（动态非对称量化） */\n")
                app_c.append(f"  ret = linear_int8({cur}, {in_f}, {tag}_weight, {tag}_wscale, {tag}_bias, {out_f},\n")
                app_c.append(f"                     {cur_act_scale}, {cur_act_zp}, {fbuf});\n")
                app_c.append("  if (ret != CNN_NORMAL) return ret;\n")
                app_c.append(f"  fmin = {fbuf}[0]; fmax = {fbuf}[0];\n")
                app_c.append(f"  for (i = 1; i < {out_f}; i++) {{\n")
                app_c.append(f"    if ({fbuf}[i] < fmin) fmin = {fbuf}[i];\n")
                app_c.append(f"    if ({fbuf}[i] > fmax) fmax = {fbuf}[i];\n")
                app_c.append("  }\n")
                app_c.append("  fscale = (fmax - fmin) / 255.0f;\n")
                app_c.append("  if (fscale < 1e-9f) fscale = 1e-9f;\n")
                app_c.append("  fzp = (int)lrintf(-fmin / fscale);\n")
                app_c.append("  fzp = fzp < 0 ? 0 : (fzp > 255 ? 255 : fzp);\n")
                app_c.append(f"  quantize_u8({fbuf}, {out_f}, fscale, fzp, {qbuf});\n\n")
                cur = qbuf
                cur_col = out_f
                cur_act_scale = "fscale"
                cur_act_zp = "fzp"
            else:
                app_c.append(f"  /* 层 {li_}: {layer_desc(l)}（输出 logits） */\n")
                app_c.append(f"  ret = linear_int8({cur}, {in_f}, {tag}_weight, {tag}_wscale, {tag}_bias, {out_f},\n")
                app_c.append(f"                     {cur_act_scale}, {cur_act_zp}, logits_buf);\n")
                app_c.append("  if (ret != CNN_NORMAL) return ret;\n\n")
            li += 1

    app_c.append("  /* 输出 */\n")
    if layers[-1][0] != "linear":
        # conv 结尾：量化域反量化到 float logits
        app_c.append(f"  for (i = 0; i < {output_size}; i++) {{\n")
        app_c.append(f"    logits_buf[i] = ((float){conv_bufs[-1]}[i]) * act_scale;\n")
        app_c.append("  }\n")
    app_c.append(f"  if (logits) memcpy(logits, logits_buf, {output_size} * sizeof(float));\n")
    app_c.append(f"  if (class_id) *class_id = argmax(logits_buf, {output_size}, NULL);\n")
    app_c.append("  return CNN_NORMAL;\n")
    app_c.append("}\n")
    app_c.append("\n")
    app_c.append(f"int {name}_forward_u8(const uint8_t *input, float *logits, uint32_t *class_id)\n")
    app_c.append("{\n")
    app_c.append("    if (!input) return CNN_POINTER_NULL;\n")
    app_c.append(f"    return {name}_core(input, logits, class_id);\n")
    app_c.append("}\n")
    app_c.append("\n")
    app_c.append(f"int {name}_forward(const float *input, float *logits, uint32_t *class_id)\n")
    app_c.append("{\n")
    app_c.append("    if (!input) return CNN_POINTER_NULL;\n")
    app_c.append(f"    quantize_u8(input, {name.upper()}_INPUT_SIZE, 0.00392156863f, 0, q_in);\n")
    app_c.append(f"    return {name}_core(q_in, logits, class_id);\n")
    app_c.append("}\n")

    with open(base + "_param.h", "w", encoding="utf-8") as f:
        f.write("".join(param_h))
    with open(base + "_param.c", "w", encoding="utf-8") as f:
        f.write("".join(param_c))
    with open(base + "_app.h", "w", encoding="utf-8") as f:
        f.write("".join(app_h))
    with open(base + "_app.c", "w", encoding="utf-8") as f:
        f.write("".join(app_c))

    print(f"模型名: {name}（int8 动态量化）")
    print(f"输出目录: {out_dir}")
    print(f"生成文件: {name}_param.h / {name}_param.c / {name}_app.h / {name}_app.c")
    print(f"结构: {[(l[0], layer_desc(l)) for l in layers]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", required=True,
                    help="要转换的模型文件（PyTorch 完整模型或 state_dict，必填）")
    ap.add_argument("--arch-config", default=None,
                    help="SpeakerNet 结构 JSON（导出实验框架模型时必填），"
                         "如 '{\"channels\":[32,64,64],\"head\":\"flat\","
                         "\"fc\":[32,4],\"kernel\":5,\"pool\":4,\"dropout\":0.3}'")
    ap.add_argument("--model-class", default=None,
                    help="任意 CNN 模型的类定义（'模块名:类名'，如 "
                         "'my_models:MyCNN'），加载 torch.save(model) 的完整模型"
                         "或 state_dict 时使用，无需 arch-config")
    ap.add_argument("--name", default=None, help="模型名（默认取检查点文件名）")
    ap.add_argument("--input-shape", default=None,
                    help="输入形状 row,col,ch（缺省时从模型结构自动推断，"
                         "推断失败再手动指定）")
    ap.add_argument("--out-dir", default=None, help="输出目录（默认脚本目录/output）")
    ap.add_argument("--quant", choices=["fp32", "int8"], default="fp32",
                    help="fp32 或 int8 量化模型")
    ap.add_argument("--calib-batches", type=int, default=400,
                    help="int8 模式激活校准使用的 batch 数")
    ap.add_argument("--calib-dir", default=None,
                    help="int8 校准数据目录（任意位置运行时的 wav 目录，"
                         "不依赖工程 datasets）")
    ap.add_argument("--calib-loader", default=None,
                    help="int8 校准 DataLoader 构造函数（'模块名:函数名'，返回 "
                         "PyTorch DataLoader，元素 (x, y)），适用于任意 CNN")
    ap.add_argument("--calib", default=None,
                    help="加载预计算量化参数 npz（a*_scale/a*_zp，由之前导出生成），"
                         "跳过现场校准")
    args = ap.parse_args()

    model, src = load_model(args)
    name = args.name or model_name_from_path(args.checkpoint)
    if args.input_shape:
        shape = tuple(int(v) for v in args.input_shape.split(","))
        if len(shape) != 3:
            raise SystemExit("--input-shape 需要 row,col,ch 三个值")
    else:
        shape = infer_input_shape(model)
        if shape is None:
            raise SystemExit(
                "无法自动推断输入形状（模型需包含 Linear 层且结构可反推）。"
                "请用 --input-shape row,col,ch 指定。")
        print(f"自动推断输入形状: (row={shape[0]}, col={shape[1]}, ch={shape[2]})")
    script_dir = os.path.dirname(os.path.abspath(__file__))
    default_out = os.path.normpath(os.path.join(script_dir, "..", "output"))
    out_dir = args.out_dir or default_out
    print(f"模型来源: {src}")
    if args.quant == "int8":
        out_dir = os.path.normpath(os.path.join(default_out, "int8"))
        if args.calib:
            # 加载预计算量化参数，跳过现场校准
            act_params = load_calib_npz(args.calib)
            generate_int8(model, name, shape, out_dir, act_params)
        elif args.calib_dir or args.calib_loader:
            # 显式提供校准数据：现场校准 -> 静态量化
            model = model.to(DEVICE)
            if args.calib_loader:
                import importlib
                mod_name, fn_name = args.calib_loader.split(":")
                calib_loader = getattr(importlib.import_module(mod_name), fn_name)()
            else:
                calib_loader = build_calib_loader(args.calib_dir)
            print("int8 模式：校准激活 scale...")
            act_params = calibrate_any(model, calib_loader, n_batches=args.calib_batches)
            generate_int8(model, name, shape, out_dir, act_params)
        else:
            # 默认：int8 动态量化（每层在线找 max，无需任何校准数据）
            print("int8 模式：动态量化（每层在线找 max，无需校准数据；"
                  "可用 --calib 加载预计算量化参数或 --calib-dir/--calib-loader 现场校准）")
            generate_int8_dynamic(model, name, shape, out_dir)
    else:
        generate(model, name, shape, out_dir)


if __name__ == "__main__":
    main()
