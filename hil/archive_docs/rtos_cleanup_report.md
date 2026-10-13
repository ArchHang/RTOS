# RTOS 源码清理与规范化报告

> 日期: 2026-09-17
> 范围: `RTOS/Core/**`、`RTOS/Port/GCC/ARM_CM7/port.c`(内核本体), 兼及 `Core/Src/{main,selftest}.c` 的告警
> 依据: `.trae/skills/c-code-style/SKILL.md`(通用 C 规范)
> 回滚点: `hil/backup_precleanup/*.before`(清理前的 11 个文件原文)
> 验收: `hil/repro_all.ps1 -Only bench,boundary,selftest`

---

## 0. 验收结果(与清理前基线逐位对比)

| 项 | 清理前 | 清理后 | 结论 |
|---|---|---|---|
| BM0 critical_pair | 23 | **23** | 不变 |
| BM1 yield_switch | 191 | **191** | 不变 |
| BM2 sem_roundtrip | 1021 | **1021** | 不变 |
| BM3 sem_wake_latency | 381 (331/424) | **381 (331/424)** | 不变 |
| BM4 queue_send_wake | 513 (507/723) | **489 (448/697)** | 略快(布局位移) |
| BM5 task_create_del | 5767 | **5767** | 不变 |
| BM6 delay1_period | 168000 | **168000** | 不变 |
| BM7 isr_wake_latency | 456 (448/592) | **456 (448/592)** | 不变 |
| boundary 夹具 | 103 PASS / 0 FAIL | **103 PASS / 0 FAIL** | 不变 |
| selftest 回归 | 419 PASS / 0 FAIL / 11 INFO / 430 | **419 PASS / 0 FAIL / 11 INFO / 430** | 不变 |
| PendSV 反汇编 | 16 条指令 | **16 条指令, 逐条一致** | 不变(见 §2.1) |
| 完整重建警告数(RTOS+Core) | 32 | **4**(均非内核) | −28 |
| 内核 `RTOS/**` 警告数 | 6 | **0** | −6 |
| 日常固件 `.text` | 196036 | **195856** | −180 B |
| `bss` | 125648 | **125600** | −48 B |

内核逐模块 `.text`(字节):

| 模块 | 清理前 | 清理后 | 变化 |
|---|---|---|---|
| `port.o` | 689 | **641** | −48(删除诊断块与包装层) |
| `rtos_queue.o` | 1619 | **1427** | **−192**(四处重复块合并) |
| `rtos_sched.o` | 1704 | 1712 | +8(`rtos_sched_block` 分支合并) |
| 其余 10 个模块 | — | — | 0(证明未触碰其逻辑) |

**功能零变化**: 所有基准值逐位一致(仅 BM4 因 `.text` 位移出现 ±5% 内的正常浮动, 与
`docs/rtos_vs_freertos_perf_report.md` §2.1 已披露的布局敏感性一致)。

---

## 1. 删除的死代码

判定方法(可复现):
1. 抽出所有顶层函数定义, 统计**全工程**标识符出现次数, 出现 1 次(仅定义处)= 无调用者;
2. 对每个结构体成员统计 `->member` / `.member` / `obj.member` 三种访问形式;
3. 用 `arm-none-eabi-gcc -O2 -Wall -Wextra -Wunused-function -Wunused-variable` **全量重建**
   (必须逐文件删除 `.o` 后 `-B`, 否则增量构建会漏报), 交叉核对。

### 1.1 `port.c`: PendSV 切换耗时统计(约 70 行, 全部为 `#if 0` 死代码)

| 删除项 | 原位置 | 证据 |
|---|---|---|
| `volatile uint32_t rtos_diag_sw_cnt/sum/min/max/last` | port.c:315-319 | 除诊断块自身外零引用 |
| `void rtos_diag_switch_reset(void)` | port.c:322 | **全工程零调用**(外部链接, 故编译器不告警) |
| `static inline uint32_t rtos_diag_cycles(void)` | port.c:332 | 仅被 `#if RTOS_DIAG_SW_TIMING` 块引用 |
| `#define RTOS_DIAG_SW_TIMING 0` + `rtos_diag_switch_record()` | port.c:351-374 | 宏恒为 0, 整块从不编译 |
| `#define RTOS_DIAG_SW_RET` 三态宏 | port.c:380-386 | 恒取 `else` 分支, 只展开成一条 `bx lr` |

代码自己的注释已写明"该块测完即删", 且 `RTOS_DIAG_SW_TIMING=1` 的路径**本身就是坏的**
(包装函数把 live 值放进 r4, 被 naked 体内的 `stmdb/ldmia {r4-r11}` 换掉, 实测 BusFault)。
删除后把"为什么不能这样测"的结论保留为 `rtos_port_pend_sv_handler` 的 `@note` —— 结论有价值,
坏实现没有。

### 1.2 内联 PendSV 包装层

```c
void rtos_port_pend_sv_handler(void) {   /* 原实现 */
#if RTOS_DIAG_SW_TIMING ... #else rtos_port_pend_sv_body(); #endif
}
```
`rtos_port_pend_sv_body` 是 `static`(仅本 TU 可见), 宏恒为 0, 因此二者构成"两层调用 + 恒真分支的
空包装"。已合并为**单个 `__attribute__((naked)) rtos_port_pend_sv_handler`**,
并把内联汇编的入参抽成宏 `PORT_PEND_SV_ASM_INPUTS`, 保持编译期布局断言(`"i"` 约束)不变。

### 1.3 从未被访问的结构体成员

| 成员 | 文件 | 引用数 | 说明 |
|---|---|---|---|
| `rtos_kernel.schedule_pending` | rtos_sched.h | **0** | 仅 `rtos.h` 一处过期注释提到它(该注释已一并修正为 `next_tcb` + PendSV) |
| `rtos_kernel.total_run_count` | rtos_sched.h | **0** | `idle_run_count` 有被 `rtos_task.c` 使用, 此成员没有 |
| `rtos_tcb.notify_type` | rtos_task.h | **0** | **只写不读**: `rtos_task_notify()` 设了它, 全内核无任何读取点(类型参数本身仍在用) |
| `{sem,event,mutex,queue,timer}.in_use` | 各自头文件 | **0** | 内核对象是"调用方提供 storage"的裸结构, 无对象池; `in_use` 是池化设计的遗留。**注**: USB OSAL 的池对象(`rtos_usb.h`)确实在用 `in_use`, 未改动 |

`rtos_tcb_t` 删除 `notify_type` 后 `sizeof` 不变(仍 140): 该字段原本占用填充字节, 删除是
"消除误导", 不是省内存 —— 如实说明。

### 1.4 空函数与死声明

- `rtos_sched_start_first_task()`(rtos_sched.c): 函数体只有两行注释, 全工程零调用。
  首任务启动的真正实现在移植层 `rtos_port_start_first_task()`。连同 `rtos_sched.h` 的声明一起删除。

### 1.5 `rtos_queue.c`: 四处重复代码块

| 重复内容 | 原出现位置 | 处理 |
|---|---|---|
| "有空位则入队(队尾/队首二选一)" | `rtos_queue_send` 情况2 + 其重试循环 | → `queue_storage_push()` |
| "队首出队 + 唤醒发送者并代其入队" | `rtos_queue_recv` 情况1 + 其重试循环 | → `queue_storage_pop()` + `queue_wake_sender_on_free_slot()` |
| "环形回绕取 head 前一格" | 上述两处内部 | → `queue_prev_index()` |

**效果**: `rtos_queue.o` 的 `.text` **1619 → 1427 字节(−11.9%)**, 且那两处重试循环原本是
"复制主路径再手改", 属于 `rtos-bug-patterns` 技能里的模式 2(重试循环遗漏首次分支)高发区 ——
现在两条路径共用同一份实现, 结构上不可能再分叉。

---

## 2. 修正的"代码与注释不符"(歧义源头)

| 位置 | 原描述 | 实际 | 处置 |
|---|---|---|---|
| `rtos_sched.h:9` / `rtos_sched.c:9` / `README.md:33` | "用 **CLZ/__builtin_clz** 找最高优先级" | 代码用的是 **`__builtin_ctz`**(取最低置位位) | 三处全部改为 ctz, 并写清"切勿写成 31-clz, 方向相反" |
| `rtos_sem.h:8` | "等待链(FIFO 或按优先级), 唤醒取链首" | 尾插 + 位图, 由 `__builtin_ctz` 选最高优先级 | 改为准确描述 |
| `rtos_queue.h:12` / 字段注释 | "等待链表按任务优先级排序, 保证高优先级先得到消息"(易被读成 FIFO) | 确实按优先级有序插入, 但**入链 O(n)** | 补上 O(n) 成本提示; 字段注释注明"按优先级有序" |
| `rtos_internal.h:23` | `@param timeout 超时(0=不阻塞立即返回)` | 实现**不特判 0**: 会入链并阻塞到下一 Tick | 改为准确契约, 并说明非阻塞语义由调用方前置判断 |
| `rtos_types.h` | `wake_tick` "0=永不超时" | 代码用 `0xFFFFFFFF` 表示永久, 0 是合法唤醒时刻 | 已在 §2 报告指出, 本次未改(属独立缺陷项) |
| `rtos_sched.h` | `rtos_sched_start` 标了 `__attribute__((noreturn))` | 加固路径 `return RTOS_ERR_SCHED` 与属性矛盾(编译器 `-Wreturn-type` 告警) | 去掉 `noreturn`, 声明与实现一致; `rtos.h` 的 `rtos_start` 同步修正 |

顺带**修复了一个真实缺陷**: 删除 `rtos.h:71` 的 `noreturn` 属性前, 该属性让 `rtos_start()`
的返回路径成为"声明不返回却返回", 是本次 `-Wall -Wextra` 告警之一。

---

## 3. 按规范做的其它修正

- **`rtos_internal.h` 缺失声明**(真实缺陷): `rtos_internal_block_current_on` 的原型在清理过程中
  一度被误删, 编译器立刻给出 `implicit declaration`; 已恢复并做全头文件声明集比对(13/13 一致)。
  *这也是"先编译后交付"的价值*: 该错误在文本比对里不明显, 编译一次即暴露。
- **`main.c`**: 四个固件变体的 TCB 指针(`selftest_tcb`/`bench_tcb`/`boundary_tcb`/`scenario_tcb`)
  用与使用处相同的 `FIRMWARE_*` 宏守卫, 消除 `-Wunused-variable`。
- **`selftest.c`**: 删除未使用的 `sys2`; 修正 `rtos_task_create_static(&t, ...)` 的实参类型
  (形参是 `rtos_tcb_t *`, 传 `&t` 即 `rtos_tcb_t **`, 属真实类型错误)。
- **`hil/build_fw.ps1`**: `-Clean` 路径下 `make clean` 的 stderr 会被
  `$ErrorActionPreference='Stop'` 误判为终止错误, 已放宽该段。

---

## 4. 有意保留的可疑项(未删除, 请确认)

| 项 | 位置 | 为什么保留 |
|---|---|---|
| `audio_cnt` | `main.c:179` | 属于**已实现但当前被注释停用**的音频累积功能(攒满 2048 字节触发 AI 推理)。删掉会连带删除一个真实特性的状态机 |
| `ft_dispatch()` | `main.c:468` | HardFault 命令分发器, 9 个 `ft_*` 任务的服务端。调用点在 `task_uart` 内被注释掉, 所以当前是死函数; 但它是 1~9 级故障测试的**唯一入口** |
| CherryUSB 的两条 `-Wformat` 警告 | `RTOS/lib/third/CherryUSB/port/dwc2/usb_dc_dwc2.c:263,506` | 第三方组件(Apache-2.0), 按"平台/厂商代码不擅自改"处理 |
| `RTOS/**` 中 `rtos_perf_*` 的 18 个 `perf_*` 内核字段 | `rtos_sched.h` | **全部在用**: 由 `rtos_perf.c` 以 `rtos_kernel.perf_xxx` 形式访问(初次粗筛时因漏了 `rtos_kernel.` 前缀被误判为死字段, 已回滚并复验) |

---

## 5. 未做的项(超出"清理"范围, 属独立缺陷, 已在复现报告中记录)

启动前调 `rtos_task_notify_wait` 解引用 NULL `current_tcb`; 启动前 `rtos_mutex_take` 报成功却没拿到锁;
`rtos_timer_delete` 的异步语义; perf 注册表满后栈溢出监测静默失效; `rtos_types.h` 的 `wake_tick` 注释。
详见 `docs/rtos_repro_audit_report.md` §3.9。

---

## 6. 复现与回滚

```powershell
# 验收(全部应通过: BM 与基线逐位一致, boundary 103/0, selftest 419/0)
powershell -ExecutionPolicy Bypass -File hil\repro_all.ps1 -Only bench,boundary,selftest

# 查看清理前后的差异
#   清理前原文: hil\backup_precleanup\*.before
#   逐行差异:   Compare-Object (Get-Content <file>.before) (Get-Content <file>)

# 回滚单个文件
Copy-Item hil\backup_precleanup\rtos_queue.c.before RTOS\Core\Src\rtos_queue.c -Force
```

> 提示: CubeIDE 的 `make clean` **不会删除全部 `.o`**(实测残留 59 个), 因此判断"警告是否
> 真的消失"必须手动清空 `Debug/**/*.o` 后重建, 否则读到的是上次编译的缓存结果。
