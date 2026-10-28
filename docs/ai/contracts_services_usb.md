# 服务、性能与USB公共API契约

基准：87bfbe4a6efc50f9b828128c713a1b5af2900ef8；逐项读取公共头及实际实现。
公共上下文、单位和证据纪律见[AI入口](README.md)。本文件描述当前行为与调用者责任，
不增加内核能力；公开头中供内核使用的导出符号明确分类，不能因可链接就当应用API。
原型对应声明；内联函数展示定义头。NULL错误指RTOS_ERR_NULL，其他大写状态加RTOS_ERR_前缀，OK指RTOS_OK。

## 数据类型与共同边界

### 定时器对象

rtos_timer_t是调用方提供的结构存储，callback类型rtos_timer_cb_t为void(*)(void*)；
rtos_timer_mode_t仅接受ONE_SHOT=0、PERIODIC=1。对象字段由实现维护，不能应用直接改写。

| 字段 | 单位与语义 |
|---|---|
| is_initialized / is_active | 已创建/在活跃链；不表示异步删除已被调用者确认 |
| deactivated | 回调摘链窗口的stop/delete痕迹，防止周期重插；内部字段 |
| generation | 32位创建代次，队列命令附带副本；回绕跳过0，不是不可耗尽的身份 |
| mode / period / expire_tick | 模式、1..0x7fffffff tick周期、32位到期tick |
| callback / arg / name | 回调、借用参数、借用名字指针；arg/name不复制 |
| next / prev | 由服务维护的活跃链链接，应用不得遍历后并发改写 |

软件服务共用一个任务，精度/延迟受tick、IRQ、调度优先级、回调耗时影响。
回调可以执行任务API，但阻塞会延迟所有后续回调及控制命令；不能等待仅本服务能够完成的操作。
普通Thread提交控制命令的100是tick，不是固定100ms；ISR投递不等待。
命令队列深度固定16。所有在途命令都借用timer指针，代次检查要求地址仍可解引用。

### 性能对象与统计解释

rtos_perf_system_t、rtos_perf_task_t、rtos_perf_memory_t是调用方输出结构，没有内部动态分配。
以下字段均为当前实现字段；不能依据头注释把32位累计值解释为无限运行记录。

| 类型 | 字段 | 实际含义 |
|---|---|---|
| system | cpu_usage_x100 / idle_usage_x100 | 0..10000表示0.00..100.00%，get_system推进窗口并更新缓存 |
| system | uptime_ticks / uptime_ms | 当前32位tick及其毫秒换算；tick回绕会使uptime重起 |
| system | total_switches / switch_rate | 热路径累计切换数/窗口次每秒；默认HOTPATH_STATS=0不采集 |
| system | sched_latency_avg_us / sched_latency_count | 窗口内派发延迟平均微秒及采样数 |
| system | sched_latency_max_us | 累计latency_max换算，不是仅本窗口最大值，reset不清它 |
| system | task_count / tick_rate_hz / cpu_freq_hz | 性能登记数、配置tick率、SystemCoreClock快照 |
| task | task_id / name / priority / state | TCB身份、固定容量名字、当前有效优先级和状态快照 |
| task | cpu_usage_x100 / run_cycles / run_time_us | 窗口占比、32位累计计数、据当前计时模式换算的32位微秒 |
| task | switch_count | TCB调度提交次数，HOTPATH_STATS关闭仍会更新 |
| task | stack_total_bytes / stack_used_bytes / stack_free_bytes | 字节；used按水印扫描或保存top_of_stack估算，并非内存保护 |
| memory | task_pool_capacity / task_pool_used | 配置MAX_TASKS/性能登记数；不是动态heap容量 |
| memory | pool_total_bytes / pool_used_bytes / pool_free_bytes | 已登记固定池总/用/空闲字节，不含未登记池和通用heap |
| memory | pool_usage_x100 / registered_pools | 汇总池占比0..10000及参与汇总的登记数量 |

默认USE_PERF_MONITOR=1但PERF_HOTPATH_STATS=0：CPU/累计周期/派发延迟不可用或为0；
get_system固定输出cpu_usage_x100=0、idle_usage_x100=10000，后者由10000减CPU字段得到，
不是实测的100%空闲率。任务TCB的switch_count、栈与内存观察仍可用。get_system会同时推进每个任务的窗口基准；
随后立即get_task不代表与刚才system同一段窗口，没有联合原子系统+任务采样API。
DWT为32位，168MHz约25.57秒回绕，差值只有未遗漏回绕时有效；不能假设系统必定在此间隔内切换。
输出指针只检查NULL，不验证可访问性/实际数组大小/与内核对象别名。

### 性能关闭宏

USE_PERF_MONITOR=0时，公共查询与内部钩子由头文件同名函数式宏替代，不存在性能实现状态。
init返回OK；reset为空；get_system/get_memory/get_task/get_all_tasks返回PARAM且不写输出，
但其显式参数各求值一次。get_cpu_usage/get_switch_rate/get_cycles/registry_count为0，
task_at为NULL，cycles_to_us为0；这些后者宏不求值被忽略的实参。
内部init_cycles/on_* /register_pool宏为空，不求值实参；不要在宏参数中放必须执行的副作用。

### USB对象与裁剪

USB句柄封装主要供CherryUSB OSAL适配器，应用优先使用已核对的设备读写接口。
所有句柄的in_use及内部对象由实现管理；只验证NULL不等于会检查池成员/旧代次，用户不得伪造或复用已删除句柄。

| 类型 | 字段/所有权 |
|---|---|
| rtos_usb_sem_t | sem为内嵌计数信号量，in_use为静态槽使用标记 |
| rtos_usb_mutex_t | mutex为内嵌递归互斥对象，in_use由池管理 |
| rtos_usb_mq_t | queue、uintptr_t storage[USB_MQ_DEPTH]、in_use；元素只复制值 |
| rtos_usb_thread_t | 内嵌tcb、对齐的stack[USB_THREAD_STACK_SIZE]、in_use(0空闲/1有效/2保留) |
| rtos_usb_timer_t | 内嵌timer及in_use；删除命令接纳后的槽可能仍被活跃定时器占用 |
| rtos_usb_isr_handler_t | void(*)(void*)，arg原样借用；处理函数在实际分发者上下文执行 |

USE_USB=0时，句柄类型及其sem/mutex/mq/thread/timer封装和timeout_ticks均不声明。
只保留头中列出的init/register_isr/cdc_init成功宏、malloc=NULL、读写/available=0、connected=false，
以及irq_dispatch/free/msleep/cdc_isr/flush_rx空宏；这些宏不求值实参。
msc_get_stats宏会对非NULL的reads/writes写0，每个非NULL指针表达式可能求值两次；busid不求值。
该宏仍会解析解引用表达式；C代码关闭USB时不要给reads/writes传未定型的NULL或整数0，
应传uint32_t变量地址或(uint32_t *)NULL，避免被忽略的分支仍产生编译错误。
关闭分支没有为展开宏补充rtos_types.h和stdbool.h，应用应显式包含这两个头后再使用相关宏。
USB开启而HID或MSC关闭时，HID read/write仍有返回0的真实函数，MSC stats真实函数写0。

rtos_config.h中的USE_USB/PERF/TIMERS/USE_HID等多数开关为无ifndef的固定定义，
不能声称任意-D覆盖都有效。修改配置应完整重编译；验证裁剪时只能使用明确的隔离配置副本，不能偷偷改生产头。
USB_DEVICE_MODE/HOST_MODE不是当前设备源文件的完整能力裁剪边界；三个USB C文件主要受USE_USB控制。
主机模式宏仅提供配置基础，当前工程不包含完整USB host集成/硬件验证。
USB_NOCACHE_RAM_SECTION默认空、对齐默认4字节，不能泛化成已经解决其他目标的D-cache/总线可访问性。

## 定时器与性能逐项契约

<a id="api-rtos_timer_create"></a>

### rtos_timer_create

```c
rtos_status_t rtos_timer_create(rtos_timer_t *timer, const char *name, rtos_timer_cb_t callback, void *arg, rtos_tick_t period, rtos_timer_mode_t mode);
```

- 分类：application。
- 参数：timer：自然对齐、可写rtos_timer_t存储；name：可NULL的名字指针；callback：非NULL的void(*)(void*)；arg：原样回调参数；period：1..0x7fffffff tick；mode：ONE_SHOT或PERIODIC。
- 上下文：Thread/启动阶段或合法syscall ISR；优先级由断言守卫检查，关断言不授权高优先级IRQ。
- 阻塞：不等待，临界区扫描活跃链以按地址查重。
- 返回/副作用：NULL timer/callback、周期0/越界、模式非法均PARAM；同址仍在活跃链或正在执行回调为ALREADY；否则清零对象并构造停止状态，返回OK。
- 生命周期/边界：首次存储可未初始化；停止/到期且不在回调中的同址对象可重建；name/arg只借用不复制。generation为32位递增、跳过0但会回绕，不能宣传无限代次防护。
- 源码：[声明](../../RTOS/Core/Inc/rtos_timer.h)；[实现](../../RTOS/Services/rtos_timer.c)。

<a id="api-rtos_timer_start"></a>

### rtos_timer_start

```c
rtos_status_t rtos_timer_start(rtos_timer_t *timer);
```

- 分类：application。
- 参数：timer：已创建且存储仍有效的对象。
- 上下文：Thread或合法syscall ISR；服务任务自身调用就地执行。
- 阻塞：普通Thread可能等待命令队列空位最多100 tick；ISR不等待；服务回调内不经过命令队列。
- 返回/副作用：NULL/未初始化PARAM；其他返回底层queue_send的OK/TIMEOUT/SCHED/ISR等实际结果。OK通常仅代表命令已接纳。处理命令时以当时tick重新设置到期时刻，已活跃则先摘链再重启。
- 生命周期/边界：对象存储须覆盖命令消费及回调窗口；队列命令保存generation，重建后旧代次命令丢弃。不得在外层临界区执行可能等待命令队列的路径。
- 源码：[声明](../../RTOS/Core/Inc/rtos_timer.h)；[实现](../../RTOS/Services/rtos_timer.c)。

<a id="api-rtos_timer_stop"></a>

### rtos_timer_stop

```c
rtos_status_t rtos_timer_stop(rtos_timer_t *timer);
```

- 分类：application。
- 参数：timer：已创建且存储有效的对象。
- 上下文：Thread或合法syscall ISR；回调中就地执行，包含自停。
- 阻塞：普通Thread命令投递最多100 tick；ISR0；服务任务就地。
- 返回/副作用：NULL/未初始化PARAM，其他为queue_send实际状态；OK不保证其他Thread提交的stop已执行。执行时摘除活跃定时器；回调窗口设置deactivated阻止周期重插。
- 生命周期/边界：停止不等于删除；外部停止命令排队时可能仍有回调。对象/arg继续存活直到执行者不再访问。
- 源码：[声明](../../RTOS/Core/Inc/rtos_timer.h)；[实现](../../RTOS/Services/rtos_timer.c)。

<a id="api-rtos_timer_change_period"></a>

### rtos_timer_change_period

```c
rtos_status_t rtos_timer_change_period(rtos_timer_t *timer, rtos_tick_t period);
```

- 分类：application。
- 参数：timer：有效已创建对象；period：1..0x7fffffff tick。
- 上下文：Thread或合法syscall ISR；服务任务就地。
- 阻塞：普通Thread投递最多100 tick；ISR0；服务任务不排队。
- 返回/副作用：NULL/未初始化/周期0或>=0x80000000为PARAM；否则queue_send状态。活跃对象执行命令后按新周期从处理时刻重启；停止对象仅改period，保持停止。
- 生命周期/边界：回调时本对象已摘链，改周期不等价于无条件start；命令及对象的寿命同start。不能照头注释泛化为所有状态都停止后重启。
- 源码：[声明](../../RTOS/Core/Inc/rtos_timer.h)；[实现](../../RTOS/Services/rtos_timer.c)。

<a id="api-rtos_timer_delete"></a>

### rtos_timer_delete

```c
rtos_status_t rtos_timer_delete(rtos_timer_t *timer);
```

- 分类：application。
- 参数：timer：有效已创建对象。
- 上下文：Thread或合法syscall ISR；服务回调中可以自删。
- 阻塞：普通Thread投递最多100 tick；ISR0；服务任务就地。
- 返回/副作用：NULL/未初始化PARAM；否则queue_send状态。OK代表删除命令已接受，执行时摘链、is_initialized=0并设置deactivated。
- 生命周期/边界：不能在OK返回后立即释放storage。须保证命令已消费且正在执行的回调已经结束；自删回调返回后服务仍会读取对象。无公共同步删除确认API，不以is_active==0代替完整生命周期确认。
- 源码：[声明](../../RTOS/Core/Inc/rtos_timer.h)；[实现](../../RTOS/Services/rtos_timer.c)。

<a id="api-rtos_timer_service_task"></a>

### rtos_timer_service_task

```c
void rtos_timer_service_task(void *arg);
```

- 分类：internal-task-entry。
- 参数：arg：忽略，可NULL。
- 上下文：仅由timer_init创建的唯一服务任务入口，不供应用再建第二个服务任务。
- 阻塞：无限循环，等待命令或下次到期；回调在临界区外同一服务任务执行，回调阻塞会延后其他定时器与命令。
- 返回/副作用：无返回；到期节点逐个摘链、回调、周期重插，每次回调后消费控制命令。周期按原到期相位递推，跳过已错过周期。
- 生命周期/边界：不得手工调用/删除服务TCB或修改内部活跃链；callback及arg必须在实际执行期间有效。
- 源码：[声明](../../RTOS/Core/Inc/rtos_timer.h)；[实现](../../RTOS/Services/rtos_timer.c)。

<a id="api-rtos_timer_init"></a>

### rtos_timer_init

```c
rtos_status_t rtos_timer_init(void);
```

- 分类：initialization-internal。
- 参数：无参数。
- 上下文：Thread/启动阶段；运行时检查ISR，ISR返回RTOS_ERR_ISR，不依赖断言。由rtos_init按配置调用。
- 阻塞：不等待；在临界区内初始化16条命令队列和静态服务任务。
- 返回/副作用：首次成功OK；已成功初始化返回ALREADY且保留状态；失败透传queue_init/task_create_static状态，s_inited保持0，允许重试。
- 生命周期/边界：初始化不能用于运行中重置服务；调用方不拥有服务TCB/栈。USE_TIMERS=0仅跳过kernel自动调用，timer.c本身未裁剪。
- 源码：[声明](../../RTOS/Core/Inc/rtos_timer.h)；[实现](../../RTOS/Services/rtos_timer.c)。

<a id="api-rtos_perf_init"></a>

### rtos_perf_init

```c
rtos_status_t rtos_perf_init(void);
```

- 分类：initialization-internal。
- 参数：无参数。
- 上下文：只供rtos_init的启动阶段，不能与运行中的钩子/查询并发；没有运行时Thread拒绝机制。
- 阻塞：不等待；可能访问DWT/DEMCR寄存器并清空注册表。
- 返回/副作用：开启时固定返回OK，清零监视器及内核统计基准，不保留先前注册信息。
- 生命周期/边界：重复手工调用会丢失已有任务/池观测登记；仅清监视账本，不创建或删除业务对象。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_reset"></a>

### rtos_perf_reset

```c
void rtos_perf_reset(void);
```

- 分类：application。
- 参数：无参数。
- 上下文：公共契约为Thread；入口RTOS_ASSERT_ISR_OK仅检查允许的ISR优先级，未强制拒绝所有ISR。
- 阻塞：不等待，持临界区遍历已注册任务。
- 返回/副作用：void；将窗口基准移到当前累计值，不清任务累计run_cycles/switch_count，不重置DWT计数，不立即更新快速查询缓存。
- 生命周期/边界：与查询共享窗口，多个诊断调用者需协调测量周期；不能把reset理解为重新初始化注册表。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_get_system"></a>

### rtos_perf_get_system

```c
rtos_status_t rtos_perf_get_system(rtos_perf_system_t *stats);
```

- 分类：application。
- 参数：stats：外部可写rtos_perf_system_t，非NULL，避免与内部对象别名。
- 上下文：Thread；入口只有syscall优先级断言，不提供ISR错误码。
- 阻塞：不等待，但临界区内计算并遍历任务注册表推进窗口。
- 返回/副作用：NULL返回NULL错误且不写输出；成功填满快照并返回OK，更新快速CPU/切换率缓存和整个测量窗口，包括每任务窗口基准。
- 生命周期/边界：uptime_ticks为32位tick；uptime_ms由其换算，不是64位长运行计时。latency_max来自累计最大值，平均/count来自窗口；详见字段表。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_get_memory"></a>

### rtos_perf_get_memory

```c
rtos_status_t rtos_perf_get_memory(rtos_perf_memory_t *stats);
```

- 分类：application。
- 参数：stats：外部可写rtos_perf_memory_t，非NULL。
- 上下文：Thread；RTOS_ASSERT_ISR_OK断言守卫；不允许高优先级IRQ。
- 阻塞：不等待，遍历已注册内存池，临界区外先清输出。
- 返回/副作用：NULL返回NULL错误且不写；成功输出聚合统计和OK。总量仅包含显式注册池，不等于整个SRAM或通用heap使用量。
- 生命周期/边界：注册池控制块须一直有效；记录没有注销API。task_pool_used是性能注册任务数，可能包含应用静态任务，不是底层任务池槽位的独立精确读数。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_get_task"></a>

### rtos_perf_get_task

```c
rtos_status_t rtos_perf_get_task(rtos_tcb_t *tcb, rtos_perf_task_t *stats);
```

- 分类：application。
- 参数：tcb：有效存活TCB，NULL选当前任务；stats：可写输出，非NULL。
- 上下文：Thread；syscall优先级断言。
- 阻塞：不等待；临界区读TCB并可能线性扫描栈水印。
- 返回/副作用：stats=NULL返回NULL错误；解析后tcb=NULL返回PARAM，均不写输出；其他有效对象成功写满并OK。非NULL tcb没有注册/代次/地址合法性校验。
- 生命周期/边界：调用方保证TCB、栈寿命，不能传已删除指针。运行任务top_of_stack是保存现场位置，不保证等于正在变化的PSP。累计周期/微秒为32位，不能当无限累计时长。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_task_registry_count"></a>

### rtos_perf_task_registry_count

```c
uint32_t rtos_perf_task_registry_count(void);
```

- 分类：internal-observer。
- 参数：无参数。
- 上下文：内核观测原语，原始volatile计数可在任意上下文读；应用不得据此宣称一组原子快照。
- 阻塞：不等待，无临界区。
- 返回/副作用：返回当前性能注册条目数。
- 生命周期/边界：不固定随后task_at的对象寿命；需要稳定遍历时用get_all_tasks。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_task_at"></a>

### rtos_perf_task_at

```c
rtos_tcb_t *rtos_perf_task_at(uint32_t index);
```

- 分类：internal-observer。
- 参数：index：零起始性能注册索引。
- 上下文：内核idle栈巡检用原始只读原语；查询与使用须遵循注册表/任务生命周期同步。
- 阻塞：不等待，无临界区。
- 返回/副作用：index>=当前登记数返回NULL，否则返回内部TCB指针。
- 生命周期/边界：返回值不复制也不增加引用，不阻止删除/重排；不能把该指针缓存为永久应用句柄。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_get_all_tasks"></a>

### rtos_perf_get_all_tasks

```c
rtos_status_t rtos_perf_get_all_tasks(rtos_perf_task_t *stats_array, uint32_t max_count, uint32_t *actual_count);
```

- 分类：application。
- 参数：stats_array：可写数组，至少max_count项；max_count：>0容量；actual_count：可NULL，否则可写uint32_t输出。
- 上下文：Thread；syscall优先级断言。
- 阻塞：不等待；整个遍历持临界区，并嵌套执行每任务栈扫描。
- 返回/副作用：array=NULL返回NULL错误；max_count=0返回PARAM，失败不写array/count。成功最多写min(登记数,max_count)项，actual_count可选，返回OK；单项查询失败则该项清零，未写尾部保持原值。
- 生命周期/边界：max_count仅限制输出容量，调用方保证真实数组大小。截断不报错，也不返回所需总容量；注册表删除会swap-with-last，索引顺序不稳定。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_get_cpu_usage"></a>

### rtos_perf_get_cpu_usage

```c
uint32_t rtos_perf_get_cpu_usage(void);
```

- 分类：application。
- 参数：无参数。
- 上下文：只读缓存，Thread或ISR可用；不进入内核临界区。
- 阻塞：不等待。
- 返回/副作用：返回上次get_system更新的cpu_usage_x100，单位0.01%；初始化/默认关闭热路径统计时为0。
- 生命周期/边界：不主动采样；没有执行get_system时缓存可能长期不变。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_get_switch_rate"></a>

### rtos_perf_get_switch_rate

```c
uint32_t rtos_perf_get_switch_rate(void);
```

- 分类：application。
- 参数：无参数。
- 上下文：只读缓存，Thread或ISR可用。
- 阻塞：不等待，无临界区。
- 返回/副作用：返回上次get_system更新的每秒切换次数缓存。
- 生命周期/边界：0不能单独证明没有调度，默认HOTPATH_STATS=0就没有完整切换统计。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_get_cycles"></a>

### rtos_perf_get_cycles

```c
uint32_t rtos_perf_get_cycles(void);
```

- 分类：internal-timing。
- 参数：无参数。
- 上下文：内部计时原语，读取DWT或tick，不保护复合业务操作；调用前计时器须初始化。
- 阻塞：不等待。
- 返回/副作用：DWT模式返回CYCCNT原始32位周期；否则返回32位tick。
- 生命周期/边界：单位取决于PERF_USE_DWT；差值只能在未遗漏回绕的采样间隔解释，不能用一个32位值计算无限运行时间。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_init_cycles"></a>

### rtos_perf_init_cycles

```c
void rtos_perf_init_cycles(void);
```

- 分类：internal-timing。
- 参数：无参数。
- 上下文：内部启动初始化；DWT模式直接修改DEMCR、CYCCNT及CTRL。
- 阻塞：不等待。
- 返回/副作用：void；DWT模式启用跟踪并将CYCCNT清0；tick回退模式无操作。
- 生命周期/边界：会影响共享同一DWT的其他测量，不在运行中随意重置。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_cycles_to_us"></a>

### rtos_perf_cycles_to_us

```c
uint32_t rtos_perf_cycles_to_us(uint32_t cycles);
```

- 分类：internal-timing。
- 参数：cycles：与当前PERF_USE_DWT一致的32位计数差值。
- 上下文：纯换算，不取对象，Thread/ISR均可。
- 阻塞：不等待。
- 返回/副作用：DWT模式用cycles/(SystemCoreClock/1000000)截断，CPU MHz为0返回0；tick模式用64位中间量乘1000000/TICK_RATE后转uint32_t。
- 生命周期/边界：主频须正确；非整MHz会有截断偏差，结果不是64位累计时长。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_on_context_switch"></a>

### rtos_perf_on_context_switch

```c
void rtos_perf_on_context_switch(rtos_tcb_t *prev, rtos_tcb_t *next);
```

- 分类：internal-kernel-hook。
- 参数：prev：前任务TCB，可NULL；next：后任务TCB，当前实现忽略。
- 上下文：仅调度器在受保护的实际切换提交点调用，不是应用扩展回调。
- 阻塞：不阻塞，不自行取临界区。
- 返回/副作用：void；累加prev运行周期、idle/total周期、派发延迟和切换计数。
- 生命周期/边界：调用方保证TCB有效及保护域。默认HOTPATH_STATS=0时生产调度器不调用此钩子；不能由应用手工调用来伪造观测。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_on_pend_switch"></a>

### rtos_perf_on_pend_switch

```c
void rtos_perf_on_pend_switch(void);
```

- 分类：internal-kernel-hook。
- 参数：无参数。
- 上下文：仅移植层发起PendSV前调用；可位于合法内核ISR路径。
- 阻塞：不阻塞，无临界区。
- 返回/副作用：void；将当前计数写入perf_pend_cycle。
- 生命周期/边界：多次请求会覆盖时间基准；默认HOTPATH_STATS=0时移植层不调用。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_on_task_created"></a>

### rtos_perf_on_task_created

```c
void rtos_perf_on_task_created(rtos_tcb_t *tcb);
```

- 分类：internal-kernel-hook。
- 参数：tcb：新构造且存储有效TCB；NULL无操作。
- 上下文：仅任务创建内部协议，调用方确保构造/生命周期；不能对活跃对象重新调用。
- 阻塞：不等待；可能填栈水印并登记静态表。
- 返回/副作用：void；清任务性能字段，按配置填充未用栈区，表未满才登记；表满没有错误返回。
- 生命周期/边界：会写栈，错误调用可破坏现场；注册表容量与应用创建总量分别约束。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_on_task_deleted"></a>

### rtos_perf_on_task_deleted

```c
void rtos_perf_on_task_deleted(rtos_tcb_t *tcb);
```

- 分类：internal-kernel-hook。
- 参数：tcb：将删除的有效TCB；NULL无操作。
- 上下文：仅任务删除内部协议。
- 阻塞：不等待，临界区线性搜索。
- 返回/副作用：void；找到后swap-with-last移除性能登记，不释放TCB/栈。
- 生命周期/边界：必须在对应存储回收之前完成；不能将它当task_delete的替代。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_register_pool"></a>

### rtos_perf_register_pool

```c
void rtos_perf_register_pool(rtos_mempool_t *pool);
```

- 分类：internal-observer。
- 参数：pool：持续有效的rtos_mempool_t；NULL无操作。
- 上下文：mempool_init自动调用；允许任务/合法syscall ISR，优先级检查只在断言开启时执行。
- 阻塞：不等待，临界区查重。
- 返回/副作用：void；表未满且未重复时登记；满表静默忽略，没有错误码。
- 生命周期/边界：无注销API；池控制块必须覆盖之后所有get_memory查询，不能把短寿命局部池登记后返回。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

<a id="api-rtos_perf_on_scheduler_start"></a>

### rtos_perf_on_scheduler_start

```c
void rtos_perf_on_scheduler_start(void);
```

- 分类：internal-kernel-hook。
- 参数：无参数。
- 上下文：仅调度器首次启动协议。
- 阻塞：不阻塞。
- 返回/副作用：void；初始化计时窗口、累计系统计数与快速缓存，不清任务注册表。
- 生命周期/边界：不能在运行中用作通用reset。
- 源码：[声明](../../RTOS/Core/Inc/rtos_perf.h)；[实现](../../RTOS/Services/rtos_perf.c)。

## USB逐项契约

<a id="api-rtos_usb_timeout_ticks"></a>

### rtos_usb_timeout_ticks

```c
static inline rtos_tick_t rtos_usb_timeout_ticks(uint32_t ms)
```

- 分类：conversion-inline。
- 参数：ms：uint32毫秒，0不等待，0xffffffff永久。
- 上下文：纯整数换算，任意上下文；仅USB开启时声明。
- 阻塞：不等待。
- 返回/副作用：永久值原样返回；其他值用64位乘法向上取整为tick，有限结果钳位到0x7fffffff。
- 生命周期/边界：返回值用于内核tick API；它不会等待或操作硬件。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/Core/Inc/rtos_usb.h)。

<a id="api-rtos_usb_sem_create"></a>

### rtos_usb_sem_create

```c
rtos_usb_sem_t *rtos_usb_sem_create(uint32_t init_count, uint32_t max_count);
```

- 分类：osal-adapter。
- 参数：init_count：初始计数；max_count：请求上限。
- 上下文：OSAL资源封装；启动/Thread或合法syscall ISR，池分配路径仅断言检查IRQ门槛。
- 阻塞：不等待，扫描固定sem池。
- 返回/副作用：max_count先提高至至少1且至少init_count；池满/底层初始化失败返回NULL，成功返回池内句柄。
- 生命周期/边界：先usb_init；返回值借用静态池槽，不可heap_free。没有句柄代次，不得跨delete保留旧指针。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_sem_delete"></a>

### rtos_usb_sem_delete

```c
void rtos_usb_sem_delete(rtos_usb_sem_t *sem);
```

- 分类：osal-adapter。
- 参数：sem：本USB池中有效句柄，可NULL。
- 上下文：启动/Thread或合法syscall ISR，依赖底层sem_deinit契约。
- 阻塞：不等待，唤醒底层等待者。
- 返回/副作用：void；NULL无操作；否则调用sem_deinit并将in_use清0，丢弃底层返回值。
- 生命周期/边界：须停止新访问并保证旧调用不再使用同址句柄后复用；没有池成员/代次校验。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_sem_take"></a>

### rtos_usb_sem_take

```c
rtos_status_t rtos_usb_sem_take(rtos_usb_sem_t *sem, rtos_tick_t timeout);
```

- 分类：osal-adapter。
- 参数：sem：有效句柄；timeout：tick，含NO_WAIT/WAIT_FOREVER。
- 上下文：Thread或合法syscall ISR；ISR必须timeout=0。
- 阻塞：可等待底层sem；阻塞条件与限制继承sem_take。
- 返回/副作用：sem=NULL为NULL错误；否则原样返回sem_take状态，不映射CherryUSB错误。
- 生命周期/边界：句柄及内部sem覆盖整个调用；删除会中止等待，不能先归还槽再继续用旧指针。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_sem_give"></a>

### rtos_usb_sem_give

```c
rtos_status_t rtos_usb_sem_give(rtos_usb_sem_t *sem);
```

- 分类：osal-adapter。
- 参数：sem：有效句柄。
- 上下文：Thread或合法syscall ISR。
- 阻塞：不等待。
- 返回/副作用：sem=NULL为NULL错误；其他为sem_give状态，包含饱和/未初始化等拒绝。
- 生命周期/边界：不负责管理调用者的data或任务生命周期。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_sem_reset"></a>

### rtos_usb_sem_reset

```c
void rtos_usb_sem_reset(rtos_usb_sem_t *sem);
```

- 分类：osal-adapter。
- 参数：sem：有效句柄，可NULL。
- 上下文：Thread或合法syscall ISR，断言守卫。
- 阻塞：不等待，临界区只清内部count。
- 返回/副作用：void；NULL无操作；不唤醒、不摘除现有等待者，也不重初始化对象。
- 生命周期/边界：已授予等待者的令牌不由此撤销；不能把它解释为广播取消。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_mutex_create"></a>

### rtos_usb_mutex_create

```c
rtos_usb_mutex_t *rtos_usb_mutex_create(void);
```

- 分类：osal-adapter。
- 参数：无参数。
- 上下文：Thread/启动阶段或合法syscall ISR；池分配和底层mutex_init只有ISR优先级断言，没有运行时ISR拒绝分支，断言关闭后不替用户过滤非法IRQ。生命周期管理建议由Thread协调；创建可在合法ISR调用不表示take/give也可在ISR调用。
- 阻塞：不等待，扫描静态mutex池。
- 返回/副作用：池满或mutex_init非OK返回NULL，失败释放预留槽，成功返回句柄。
- 生命周期/边界：需usb_init；不得直接free或修改in_use。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_mutex_delete"></a>

### rtos_usb_mutex_delete

```c
void rtos_usb_mutex_delete(rtos_usb_mutex_t *mutex);
```

- 分类：osal-adapter。
- 参数：mutex：有效池句柄，可NULL。
- 上下文：Thread/合法syscall ISR；底层mutex_deinit仅执行RTOS_ASSERT_ISR_OK，没有运行时ISR拒绝分支。断言关闭后不替用户过滤非法IRQ优先级。生命周期管理建议由Thread协调。
- 阻塞：不等待。
- 返回/副作用：NULL无操作；否则调用mutex_deinit、清in_use，忽略底层错误。
- 生命周期/边界：销毁须由生命周期管理者协调；即使在合法ISR中调用，此void包装也不报告失败或确认所有并发使用者已结束。归还静态池槽前必须结束旧句柄的并发访问。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_mutex_take"></a>

### rtos_usb_mutex_take

```c
rtos_status_t rtos_usb_mutex_take(rtos_usb_mutex_t *mutex);
```

- 分类：osal-adapter。
- 参数：mutex：有效池句柄。
- 上下文：Thread，ISR由底层mutex_take拒绝。
- 阻塞：永久等待RTOS_WAIT_FOREVER，不能在外层临界区或服务依赖环中调用。
- 返回/副作用：NULL为NULL错误；其他原样返回底层mutex_take结果。
- 生命周期/边界：递归、继承、删除中止语义同内核mutex；取得成功后由所有者give。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_mutex_give"></a>

### rtos_usb_mutex_give

```c
rtos_status_t rtos_usb_mutex_give(rtos_usb_mutex_t *mutex);
```

- 分类：osal-adapter。
- 参数：mutex：有效池句柄，当前任务应为所有者。
- 上下文：Thread；ISR由底层拒绝。
- 阻塞：不等待。
- 返回/副作用：NULL为NULL错误；其他原样返回mutex_give状态。
- 生命周期/边界：释放一次递归层级；并不注销对象。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_mq_create"></a>

### rtos_usb_mq_create

```c
rtos_usb_mq_t *rtos_usb_mq_create(uint32_t max_msgs);
```

- 分类：osal-adapter。
- 参数：max_msgs：1..RTOS_CONFIG_USB_MQ_DEPTH条。
- 上下文：启动/Thread或合法syscall ISR，受底层queue_init契约约束。
- 阻塞：不等待，扫描固定mq池。
- 返回/副作用：0/超过内嵌深度/池满/queue_init失败返回NULL；成功返回句柄。
- 生命周期/边界：元素固定为uintptr_t，内嵌storage；存的是事件值/指针数值，不拥有指向的数据。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_mq_delete"></a>

### rtos_usb_mq_delete

```c
void rtos_usb_mq_delete(rtos_usb_mq_t *mq);
```

- 分类：osal-adapter。
- 参数：mq：有效池句柄，可NULL。
- 上下文：启动/Thread或合法syscall ISR，继承queue_deinit契约。
- 阻塞：不等待。
- 返回/副作用：NULL无操作；否则queue_deinit后清in_use，丢弃底层状态。
- 生命周期/边界：先停止访问；排队指针值不会被自动free，槽复用不识别旧裸句柄。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_mq_send"></a>

### rtos_usb_mq_send

```c
rtos_status_t rtos_usb_mq_send(rtos_usb_mq_t *mq, uintptr_t addr, rtos_tick_t timeout);
```

- 分类：osal-adapter。
- 参数：mq：有效句柄；addr：按值复制的uintptr_t；timeout：tick。
- 上下文：Thread或合法syscall ISR；ISR必须timeout=0。
- 阻塞：满队列且非0 timeout可等待；继承queue_send。
- 返回/副作用：NULL句柄为NULL错误；其他原样返回底层queue_send状态。
- 生命周期/边界：函数把addr局部值交给同步queue_send，整个调用期间有效；addr指向的外部数据寿命仍由业务管理。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_mq_recv"></a>

### rtos_usb_mq_recv

```c
rtos_status_t rtos_usb_mq_recv(rtos_usb_mq_t *mq, uintptr_t *addr, rtos_tick_t timeout);
```

- 分类：osal-adapter。
- 参数：mq：有效句柄；addr：可写uintptr_t输出，非NULL；timeout：tick。
- 上下文：Thread或合法syscall ISR；ISR必须timeout=0。
- 阻塞：空队列且非0 timeout可等待。
- 返回/副作用：mq/addr任一NULL为NULL错误；其他原样返回queue_recv结果。
- 生命周期/边界：成功只复制一个uintptr_t；失败不应使用未完成输出，指针数值不带所有权协议。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_thread_create"></a>

### rtos_usb_thread_create

```c
rtos_usb_thread_t *rtos_usb_thread_create(const char *name, uint32_t stack_size, rtos_prio_t prio, rtos_task_func_t entry, void *arg);
```

- 分类：osal-adapter。
- 参数：name：可NULL名字，底层复制到TCB；stack_size：32位字数；prio：小值优先；entry：非NULL void(*)(void*)；arg：借用入口参数。
- 上下文：仅Thread/启动，底层task_create_static运行时拒绝ISR；优先级过大被钳位至最低。
- 阻塞：不等待，扫描线程池；可能回收已DELETED且不是current的旧静态槽。
- 返回/副作用：entry=NULL/池满/底层创建失败返回NULL；stack_size为0或过大用内嵌上限，再抬到MINIMAL；成功返回池内TCB/栈句柄。
- 生命周期/边界：使用静态TCB/栈，不依赖任务堆；arg覆盖任务使用期。配置应保证USB_THREAD_STACK_SIZE>=MINIMAL，不能靠请求值扩大实际数组。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_thread_delete"></a>

### rtos_usb_thread_delete

```c
void rtos_usb_thread_delete(rtos_usb_thread_t *thread);
```

- 分类：osal-adapter。
- 参数：thread：有效句柄；NULL表示删除当前任务。
- 上下文：仅Thread；运行时ISR调用直接无操作。
- 阻塞：不等待；自删除会切换且通常不返回到调用点。
- 返回/副作用：void；外部删除预留in_use=2，底层OK才清槽，失败恢复1；自删槽延迟到不再为current时由分配者回收。
- 生命周期/边界：不能立刻复用自删任务的栈；返回入口也走内核删除。无状态返回的包装不能当删除成功确认。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_thread_schedule_other"></a>

### rtos_usb_thread_schedule_other

```c
void rtos_usb_thread_schedule_other(void);
```

- 分类：osal-adapter。
- 参数：无参数。
- 上下文：运行中的Thread；依赖当前有效任务、task_set_priority/yield；不用于ISR。
- 阻塞：会主动让出CPU，其他任务可运行。
- 返回/副作用：无当前任务时无操作；否则保存当前有效优先级，改成最低，yield，再用保存值调用set_priority，忽略各返回状态。
- 生命周期/边界：这是调度让步操作，会改变基础优先级设置；不提供互斥所有权或优先级继承不变的承诺。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_timer_create"></a>

### rtos_usb_timer_create

```c
rtos_usb_timer_t *rtos_usb_timer_create(const char *name, uint32_t ms, rtos_timer_cb_t cb, void *arg, bool is_periodic);
```

- 分类：osal-adapter。
- 参数：name：借用名字；ms：周期毫秒，转换后须1..0x7fffffff tick；cb：非NULL回调；arg：借用参数；is_periodic：true周期/false单次。
- 上下文：Thread/启动或合法syscall ISR，继承timer_create；先有可用timer服务。
- 阻塞：不等待；扫描USB timer池，碰到仍活跃/回调的异步删除槽会尝试后续槽。
- 返回/副作用：NULL cb/无槽/转换后周期非法/其他创建失败为NULL；成功返回内部timer句柄。ms=0或永久值不是合法周期。
- 生命周期/边界：内部timer占静态槽；参数及对象遵守异步timer生命周期，无普通heap释放。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_timer_delete"></a>

### rtos_usb_timer_delete

```c
void rtos_usb_timer_delete(rtos_usb_timer_t *timer);
```

- 分类：osal-adapter。
- 参数：timer：有效句柄，可NULL。
- 上下文：同delete_checked。
- 阻塞：同delete_checked。
- 返回/副作用：void，丢弃delete_checked结果；NULL无操作。
- 生命周期/边界：void返回不能判断删除是否接纳，需检查结果时用checked。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_timer_delete_checked"></a>

### rtos_usb_timer_delete_checked

```c
rtos_status_t rtos_usb_timer_delete_checked(rtos_usb_timer_t *timer);
```

- 分类：osal-adapter。
- 参数：timer：有效USB timer句柄，非NULL。
- 上下文：Thread或合法syscall ISR；服务回调中就地。
- 阻塞：普通Thread命令投递最多100 tick，ISR0。
- 返回/副作用：NULL为NULL错误；透传timer_delete状态，仅OK时清in_use，投递失败保留槽。
- 生命周期/边界：OK仍是异步接受；旧timer对象存储永久在静态池，下一次create有活跃/回调查重和代次检查。旧业务句柄仍不可复用。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_timer_start"></a>

### rtos_usb_timer_start

```c
void rtos_usb_timer_start(rtos_usb_timer_t *timer);
```

- 分类：osal-adapter。
- 参数：timer：有效句柄，可NULL。
- 上下文：同内核timer_start。
- 阻塞：普通Thread最多100 tick投递；ISR0；服务就地。
- 返回/副作用：NULL无操作，其他忽略timer_start状态。
- 生命周期/边界：无法由void返回判断命令是否接纳，不能以此作为开始计时确认。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_timer_stop"></a>

### rtos_usb_timer_stop

```c
void rtos_usb_timer_stop(rtos_usb_timer_t *timer);
```

- 分类：osal-adapter。
- 参数：timer：有效句柄，可NULL。
- 上下文：同内核timer_stop。
- 阻塞：普通Thread最多100 tick投递；ISR0；服务就地。
- 返回/副作用：NULL无操作，其他忽略timer_stop状态。
- 生命周期/边界：不表示回调已结束，不能凭返回释放arg。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_init"></a>

### rtos_usb_init

```c
rtos_status_t rtos_usb_init(void);
```

- 分类：application。
- 参数：无参数；调用前已完成rtos_init。
- 上下文：仅启动阶段、rtos_start之前一次调用；实现没有重复初始化守卫或Thread运行时拒绝。
- 阻塞：不等待；清所有对象池、ISR映射并初始化小/大块池。
- 返回/副作用：返回mempool_init实际状态，成功OK；先前池清理不是事务回滚。
- 生命周期/边界：运行中重复调用会抹掉活跃句柄/映射，禁止这样使用；不创建USB协议任务，不代表设备枚举成功。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_register_isr"></a>

### rtos_usb_register_isr

```c
rtos_status_t rtos_usb_register_isr(uint32_t irqn, rtos_usb_isr_handler_t handler, void *arg);
```

- 分类：application。
- 参数：irqn：映射键，通常CMSIS外部IRQ编号；handler：非NULL void(*)(void*)；arg：原样借用参数。
- 上下文：启动/Thread或合法syscall ISR；断言守卫。
- 阻塞：不等待，临界区扫描固定表。
- 返回/副作用：handler=NULL为NULL错误；同irqn替换handler/arg为OK；新键有槽OK、表满NO_MEM，不改NVIC。
- 生命周期/边界：handler/arg在映射使用期间有效。应用自己配置并使能NVIC，不能将编码阈值0x50当HAL逻辑优先级参数。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_irq_dispatch"></a>

### rtos_usb_irq_dispatch

```c
void rtos_usb_irq_dispatch(uint32_t irqn);
```

- 分类：application。
- 参数：irqn：与register_isr相同映射键。
- 上下文：用于合法syscall USB IRQ入口，断言守卫；不在高优先级IRQ/NMI/HardFault调用。
- 阻塞：不等待，本身不取临界区，handler也须满足IRQ语义。
- 返回/副作用：找到第一匹配项则调用handler(arg)，未注册为空操作。
- 生命周期/边界：映射无公共注销API；运行期更新须和实际IRQ调用协调，不能让arg先销毁。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_malloc"></a>

### rtos_usb_malloc

```c
void *rtos_usb_malloc(size_t size);
```

- 分类：application。
- 参数：size：size_t字节数。
- 上下文：启动/Thread或合法syscall ISR，继承mempool分配的临界区/断言约束。
- 阻塞：不等待。
- 返回/副作用：0或>512返回NULL；1..64只尝试64B小池，65..512只尝试512B大池；目标池满NULL，无跨池回退。
- 生命周期/边界：先usb_init；返回只保证本池当前对齐(4B配置)，不要推断任意DMA/缓存属性。USB堆与rtos_heap/libc malloc独立，必须配对usb_free。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_free"></a>

### rtos_usb_free

```c
void rtos_usb_free(void *ptr);
```

- 分类：application。
- 参数：ptr：usb_malloc返回的仍有效块，可NULL。
- 上下文：Thread或合法syscall ISR；实际落入mempool_free后检查优先级契约。
- 阻塞：不等待；mempool_free可能扫描空闲链检查重复归还。
- 返回/副作用：void；NULL/不在两个USB堆范围内无操作；范围内调用mempool_free但忽略其错误。
- 生命周期/边界：不擦除内容，不撤销旧裸指针；切勿将任意地址或已复用旧指针交给此接口。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_msleep"></a>

### rtos_usb_msleep

```c
void rtos_usb_msleep(uint32_t ms);
```

- 分类：application。
- 参数：ms：毫秒，0与永久值由timeout_ticks转换。
- 上下文：仅Thread；函数自身没有显式ISR拒绝，依赖task_delay的上下文处理。
- 阻塞：通过task_delay等待tick，0为让步语义。
- 返回/副作用：void，不提供睡眠实际时长或失败状态。
- 生命周期/边界：不用于硬实时微秒延时；不在外层临界区或调度挂起中请求阻塞。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-rtos_usb_cdc_init"></a>

### rtos_usb_cdc_init

```c
rtos_status_t rtos_usb_cdc_init(uint8_t busid, uintptr_t reg_base);
```

- 分类：application。
- 参数：busid：0..CONFIG_USBDEV_MAX_BUS-1；reg_base：真实DWC2控制器寄存器基址。
- 上下文：启动Thread，usb_init之后、rtos_start之前；不是纯软件构造，初始化可能立即触发USB IRQ。
- 阻塞：不等待业务数据；调用CherryUSB初始化并可创建MSC等服务资源。
- 返回/副作用：busid越界PARAM；已初始化ALREADY；同步对象不足NO_MEM并回收本轮部分资源；usbd_initialize失败会deinitialize并回收后返回PARAM；成功OK。reg_base有效性不由本函数证明。
- 生命周期/边界：默认CDC+MSC+HID消耗4个sem、2个mutex，另有MSC线程/mq资源。无对称公共cdc_deinit；外设供电、时钟、GPIO、NVIC由板级完成。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_device.c)。

<a id="api-rtos_usb_cdc_isr"></a>

### rtos_usb_cdc_isr

```c
void rtos_usb_cdc_isr(uint8_t busid);
```

- 分类：application。
- 参数：busid：有效总线ID。
- 上下文：合法syscall USB IRQ入口；越界直接返回，正常路径断言检查优先级。
- 阻塞：不等待，转发USBD_IRQHandler。
- 返回/副作用：void；与ST glue提供的IRQ入口/注册分发方案择一接入，不能重复派发一次IRQ。
- 生命周期/边界：控制器已初始化且寄存器可访问；函数调用不证明真实USB线缆/主机互通。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_device.c)。

<a id="api-rtos_usb_cdc_is_connected"></a>

### rtos_usb_cdc_is_connected

```c
bool rtos_usb_cdc_is_connected(uint8_t busid);
```

- 分类：application。
- 参数：busid：总线ID。
- 上下文：只读软件标志，Thread/ISR可读；不等待，无锁。
- 阻塞：不等待。
- 返回/副作用：busid越界false；否则返回connected。该标志在CONFIGURED事件设true，RESET/DISCONNECTED设false。
- 生命周期/边界：不是主机已打开串口/DTR断言的保证，也不证明下一次传输必成功；它是瞬时状态。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_device.c)。

<a id="api-rtos_usb_cdc_write"></a>

### rtos_usb_cdc_write

```c
int rtos_usb_cdc_write(uint8_t busid, const uint8_t *data, uint32_t len, uint32_t timeout_ms);
```

- 分类：application。
- 参数：busid：已初始化总线；data：len字节稳定只读输入；len：>0字节数，实际范围建议<=INT_MAX以匹配int结果；timeout_ms：毫秒，0不等待、0xffffffff永久。
- 上下文：仅Thread；运行时检测ISR并返回0。
- 阻塞：一个总tick预算覆盖TX互斥锁、旧传输完成和全部分块；0预算仍可启动一次空闲传输，但不能保证同步完成。
- 返回/副作用：越界/NULL/len0/ISR/未连接为0；其他返回已完成块的总字节数，可短写；0合并超时、断连、底层失败，不给细分错误。
- 生命周期/边界：每块先复制进独立内部TX缓存，返回后不会继续读取用户data；超时已提交的当前块可能仍发送，且不计入返回的已完成字节，重试须考虑业务去重。RESET/重配会更换session终止旧操作。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_device.c)。

<a id="api-rtos_usb_cdc_read"></a>

### rtos_usb_cdc_read

```c
int rtos_usb_cdc_read(uint8_t busid, uint8_t *buf, uint32_t len, uint32_t timeout_ms);
```

- 分类：application。
- 参数：busid：已初始化总线；buf：至少len字节可写；len：>0最大接收字节；timeout_ms：毫秒，0不等待、0xffffffff永久。
- 上下文：公共使用契约为Thread；实现仅断言检查syscall优先级，没有统一ISR拒绝，不能据偶然有数据路径推断可在ISR等待。
- 阻塞：RX为空且连接时等待信号量，使用总预算；临界区内最多两次memcpy读取环形数据。
- 返回/副作用：非法busid/NULL/len0为0；有数据立即返回<=len字节，不保证填满；无数据且未连接/超时/sem错误为0。0不改buf，有效读会消费环形数据。
- 生命周期/边界：先尝试读取缓冲，再检查connected，断连后尚存数据也可能返回。多读者由临界区竞争消费，不是广播；输出长度须以返回值为准。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_device.c)。

<a id="api-rtos_usb_cdc_available"></a>

### rtos_usb_cdc_available

```c
uint32_t rtos_usb_cdc_available(uint8_t busid);
```

- 分类：application。
- 参数：busid：总线ID。
- 上下文：Thread或合法syscall ISR，断言守卫。
- 阻塞：不等待，临界区快照head/tail。
- 返回/副作用：越界返回0；否则返回当前RX已缓存字节数。
- 生命周期/边界：只是观察，不预留字节；并发读者可能先消费，不能按先前数量保证下一次read。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_device.c)。

<a id="api-rtos_usb_cdc_flush_rx"></a>

### rtos_usb_cdc_flush_rx

```c
void rtos_usb_cdc_flush_rx(uint8_t busid);
```

- 分类：application。
- 参数：busid：总线ID。
- 上下文：Thread或合法syscall ISR，断言守卫。
- 阻塞：不等待，临界区将tail推进到head。
- 返回/副作用：越界无操作；丢弃尚未读取的RX字节，不重置/清空rx_sem，也不停止OUT接收。
- 生命周期/边界：不是发送flush、不是断开连接，也不影响将来收到的数据。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_device.c)。

<a id="api-rtos_usb_msc_get_stats"></a>

### rtos_usb_msc_get_stats

```c
void rtos_usb_msc_get_stats(uint8_t busid, uint32_t *reads, uint32_t *writes);
```

- 分类：application。
- 参数：busid：当前被忽略，实际后端为一份全局RAM盘；reads/writes：可NULL，否则可写uint32_t输出。
- 上下文：仅普通观测；只读32位volatile累计值，不取临界区，任务/ISR读取不是双字段原子快照。
- 阻塞：不等待。
- 返回/副作用：void；MSC启用时分别写累计读/写扇区数；MSC关闭时非NULL输出写0。
- 生命周期/边界：单位是512字节扇区，累计uint32会回绕；不表示主机文件系统提交或物理持久化成功。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_msc.c)。

<a id="api-rtos_usb_hid_write"></a>

### rtos_usb_hid_write

```c
int rtos_usb_hid_write(uint8_t busid, const uint8_t *data, uint32_t len, uint32_t timeout_ms);
```

- 分类：application。
- 参数：busid：已初始化总线；data：至少min(len,64)字节只读输入；len：>0字节，最大截断64；timeout_ms：毫秒，0或永久同CDC。
- 上下文：HID开启时仅Thread，ISR运行时返回0。
- 阻塞：总预算覆盖TX互斥、旧传输等待与本次完成等待。
- 返回/副作用：非法输入/未连接/超时/底层失败为0，成功返回截断后的实际长度；HID关闭时固定0，不读data。
- 生命周期/边界：数据复制到内部HID TX缓存后提交；超时提交可能仍在途，用户data在返回后可复用；session变化使本次返回0。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_device.c)。

<a id="api-rtos_usb_hid_read"></a>

### rtos_usb_hid_read

```c
int rtos_usb_hid_read(uint8_t busid, uint8_t *buf, uint32_t len, uint32_t timeout_ms);
```

- 分类：application。
- 参数：busid：已初始化总线；buf：至少len字节可写；len：>0最大读取量；timeout_ms：毫秒。
- 上下文：公共使用契约为Thread；实现为syscall优先级断言，不保证一律拒绝ISR。
- 阻塞：无报告且连接时按总预算等待；有报告时临界区复制。
- 返回/副作用：非法输入/无数据未连接/等待失败为0；成功返回min(报告长,len)，随后整份报告槽清空，未复制尾部丢弃；HID关闭固定0且buf不改。
- 生命周期/边界：单报告槽，新报告在旧报告待读期间会丢弃；不提供可靠无损流式队列，业务须按报告协议处理。
- 源码：[声明](../../RTOS/Core/Inc/rtos_usb.h)；[实现](../../RTOS/USB/rtos_usb_device.c)。


## CherryUSB配置宏

[usb_config.h](../../RTOS/Core/Inc/usb_config.h)是编译配置，不是额外运行时API。
以下数值是本基线默认值，除显式固定定义外均使用ifndef，可由先前定义覆盖；头文件不会完成板级时钟/引脚/缓存配置。

| 宏 | 默认与实际约束 |
|---|---|
| USB_OSAL_WAITING_FOREVER | 0xffffffff，CherryUSB等待参数的永久值，单位体系为毫秒 |
| RTOS_CONFIG_USB_VID / RTOS_CONFIG_USB_PID | 0x0483 / 0x5740，设备描述符标识，不保证主机驱动安装 |
| RTOS_CONFIG_USB_CDC_RX_BUF_SIZE | 512字节环形RX，至少EP_OUT缓冲大小；支持非2次幂路径 |
| RTOS_CONFIG_USB_CDC_EP_OUT_BUF_SIZE | 64字节端点OUT缓存，调用者移植须与控制器/MPS匹配 |
| RTOS_CONFIG_USB_CDC_TX_BUF_SIZE | 512字节内部TX分块缓存；目标需正值且适配端点，头部仅对RX/OUT大小关系执行#error |
| CONFIG_USB_DBG_LEVEL | USB_DBG_INFO，CherryUSB日志级别，由其日志头解释 |
| CONFIG_USB_ALIGN_SIZE | 4字节，仅对齐，不能推出CPU缓存一致性 |
| USB_NOCACHE_RAM_SECTION | 空属性，默认没有专用非缓存段/MPU配置 |
| CONFIG_USB_MEMPOOL_MAX_BLOCK_COUNT | 16，CherryUSB自身内存池约束，区别于rtos_mempool |
| CONFIG_USBDEV_MAX_BUS | 1，设备数组容量；当前F446端点/FIFO实现仍按单控制器集成 |
| CONFIG_USBDEV_REQUEST_BUFFER_LEN | 512字节，EP0请求缓存 |
| CONFIG_USBDEV_EP0_PRIO | RTOS_CONFIG_USB_THREAD_PRIORITY；实际EP0_THREAD未默认定义 |
| CONFIG_USBDEV_EP0_STACKSIZE | USB_THREAD_STACK_SIZE*4字节，CherryUSB侧单位 |
| CONFIG_USBDEV_EP_NUM | 8，协议栈端点数组参数，不代表F446有8个硬件端点 |
| CONFIG_USBDEV_MSC_MAX_LUN | 1，仅USB及MSC开启时默认定义 |
| CONFIG_USBDEV_MSC_MAX_BUFSIZE | 512字节MSC缓存，USB及MSC开启时 |
| CONFIG_USBDEV_MSC_THREAD | USB及MSC开启时直接定义为存在宏，MSC工作在线程上下文 |
| CONFIG_USBDEV_MSC_STACKSIZE | USB_THREAD_STACK_SIZE*4字节 |
| CONFIG_USBDEV_MSC_PRIO | USB_THREAD_PRIORITY |
| CONFIG_USBDEV_MSC_MANUFACTURER_STRING | RTOS；SCSI字段最多8字符 |
| CONFIG_USBDEV_MSC_PRODUCT_STRING | RAM Disk；字段最多16字符 |
| CONFIG_USBDEV_MSC_VERSION_STRING | 1.00；字段最多4字符 |
| CONFIG_USB_DWC2_CUSTOM_FIFO | USB开启时定义为存在宏，绑定本文F446的FIFO回调 |
| CONFIG_USB_HS | 本头仅注释建议，没有默认定义；不能把它视作当前已启用/已验证高速 |
| CONFIG_USBHOST_MAX_RHPORTS | HOST_MODE开启时默认1 |
| CONFIG_USBHOST_MAX_EXTHUBS | HOST_MODE开启时默认1 |
| CONFIG_USBHOST_MAX_EHPORTS | HOST_MODE开启时默认4 |
| CONFIG_USBHOST_MAX_INTERFACES | HOST_MODE开启时默认8 |
| CONFIG_USBHOST_MAX_INTF_ALTSETTINGS | HOST_MODE开启时默认2 |
| CONFIG_USBHOST_MAX_ENDPOINTS | HOST_MODE开启时默认4 |
| CONFIG_USBHOST_PSC_PRIO | HOST_MODE开启时默认USB_THREAD_PRIORITY |
| CONFIG_USBHOST_PSC_STACKSIZE | HOST_MODE开启时默认USB_THREAD_STACK_SIZE*4字节 |
| CONFIG_USBHOST_REQUEST_BUFFER_LEN | HOST_MODE开启时默认2048字节 |
| CONFIG_USBHOST_MAX_BUS | HOST_MODE开启时默认1；这些宏不构成当前项目完整host实现 |

<a id="api-CONFIG_USB_PRINTF"></a>

### CONFIG_USB_PRINTF

~~~c
#define CONFIG_USB_PRINTF(...) printf(__VA_ARGS__)
~~~

- 分类：configuration-macro；ifndef允许覆盖，默认依赖stdio.h声明的printf。
- 参数：可变参数遵守printf格式与实参类型契约，不能把外部数据直接作为格式字符串。
- 上下文/阻塞：取决于项目stdio重定向，默认USART HAL输出可能阻塞；宏本身没有ISR、重入或时延保护。
- 返回/副作用：默认就是printf的int结果与输出副作用；不是RTOS错误码。配置覆盖后以替代实现为准。
- 生命周期/边界：调用期间格式字符串及指向的实参有效；不保证日志与其他任务输出原子交错。
- 源码：[定义](../../RTOS/Core/Inc/usb_config.h)。

## CherryUSB OSAL桥接补充契约

以下接口由第三方[usb_osal.h](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)声明，
本RTOS在[rtos_usb_osal.c](../../RTOS/USB/rtos_usb_osal.c)实现。它们是协议栈适配边界，
并非把所有rtos_usb_*返回值直接改名：0表示成功，TIMEOUT映射-USB_ERR_TIMEOUT，其余RTOS错误压缩为-1。
USE_USB=0时实现文件不产生这些外部定义，也没有对应OSAL存根，不能继续链接协议栈调用后声称USB关闭可用。
桥接层带错误日志的create包装按启动/Thread调用约定使用；其失败路径会执行USB_LOG_ERR，
当前printf重定向不保证ISR时延或重入安全，不能只凭底层池操作允许合法ISR就推广整个create包装。
该约束没有一套统一的运行时拒绝，实际执行者仍须遵守；give/非阻塞send等明确允许的ISR路径另见各项。

usb_osal_thread_t/sem_t/mutex_t/mq_t均为void*，须来自各自create且只能交给同族API；
void*不会检查混用类型。usb_thread_entry_t在本工程默认配置为void(*)(void*)，
usb_timer_handler_t为void(*)(void*)。struct usb_osal_timer的handler、argument、is_period、
timeout_ms、timer字段由create填写：最后一个字段指向USB静态定时器句柄，外层结构从USB堆分配。
回调实际接收argument，不是外层timer结构。调用方不能修改这些字段或把该结构用libc free释放。

<a id="api-usb_osal_thread_create"></a>

### usb_osal_thread_create

```c
usb_osal_thread_t usb_osal_thread_create(const char *name, uint32_t stack_size, uint32_t prio, usb_thread_entry_t entry, void *args);
```

- 参数：name名字可NULL；stack_size字节；prio小值优先；entry非NULL入口；args借用参数。
- 上下文、阻塞、结果与寿命：Thread/启动；字节数向上取整为32位字，再交rtos_usb_thread_create，0用USB默认栈，过大最终截断；prio先钳位。成功返回void*句柄，失败NULL并可能输出日志。参数存储覆盖线程使用期。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_thread_create)。

<a id="api-usb_osal_thread_delete"></a>

### usb_osal_thread_delete

```c
void usb_osal_thread_delete(usb_osal_thread_t thread);
```

- 参数：thread为本族句柄，可NULL表示当前任务。
- 上下文、阻塞、结果与寿命：上下文/自删延迟槽回收继承rtos_usb_thread_delete，void不反馈失败，不可继续访问被删线程对象。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_thread_delete)。

<a id="api-usb_osal_thread_schedule_other"></a>

### usb_osal_thread_schedule_other

```c
void usb_osal_thread_schedule_other(void);
```

- 参数：无参数。
- 上下文、阻塞、结果与寿命：运行中的Thread让步，保存有效优先级、临时降到最低后恢复；可能让其他任务运行，不是ISR API。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_thread_schedule_other)。

<a id="api-usb_osal_sem_create"></a>

### usb_osal_sem_create

```c
usb_osal_sem_t usb_osal_sem_create(uint32_t initial_count);
```

- 参数：initial_count为初始计数。
- 上下文、阻塞、结果与寿命：返回sem void*或NULL；通过rtos_usb_sem_create(initial_count,1)。实际底层会把max提高至initial_count，不能把initial_count>1仍称作严格二值信号量。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_sem_create)。

<a id="api-usb_osal_sem_create_counting"></a>

### usb_osal_sem_create_counting

```c
usb_osal_sem_t usb_osal_sem_create_counting(uint32_t max_count);
```

- 参数：max_count为请求计数上限。
- 上下文、阻塞、结果与寿命：以初值0调用USB sem create，max=0会被提高为1，成功句柄或NULL；池生命周期同USB sem。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_sem_create)。

<a id="api-usb_osal_sem_delete"></a>

### usb_osal_sem_delete

```c
void usb_osal_sem_delete(usb_osal_sem_t sem);
```

- 参数：sem为有效本族句柄，可NULL。
- 上下文、阻塞、结果与寿命：void；同USB sem delete，删除中止等待；调用者协调旧使用者及句柄复用。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_sem_delete)。

<a id="api-usb_osal_sem_take"></a>

### usb_osal_sem_take

```c
int usb_osal_sem_take(usb_osal_sem_t sem, uint32_t timeout);
```

- 参数：sem非NULL有效句柄；timeout为毫秒，0不等、0xffffffff永久。
- 上下文、阻塞、结果与寿命：Thread可等待；合法syscall ISR仅timeout=0。NULL为-1；换算tick后调用take，0成功、-USB_ERR_TIMEOUT超时、其他-1。整个调用期间句柄有效。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_sem_take)。

<a id="api-usb_osal_sem_give"></a>

### usb_osal_sem_give

```c
int usb_osal_sem_give(usb_osal_sem_t sem);
```

- 参数：sem为有效句柄。
- 上下文、阻塞、结果与寿命：任务/合法syscall ISR非阻塞；NULL=-1，底层状态经OSAL压缩。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_sem_give)。

<a id="api-usb_osal_sem_reset"></a>

### usb_osal_sem_reset

```c
void usb_osal_sem_reset(usb_osal_sem_t sem);
```

- 参数：sem有效或NULL。
- 上下文、阻塞、结果与寿命：仅清可用计数，不唤醒/取消已排队等待者；void，其他上下文限制同USB sem reset。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_sem_reset)。

<a id="api-usb_osal_mutex_create"></a>

### usb_osal_mutex_create

```c
usb_osal_mutex_t usb_osal_mutex_create(void);
```

- 参数：无参数。
- 上下文、阻塞、结果与寿命：建议Thread/启动创建，非阻塞，返回mutex void*或NULL。底层支持合法syscall ISR创建且没有运行时ISR拒绝；但本包装失败时可能输出日志，日志路径未提供通用ISR安全保证，不能据此承诺整个包装在ISR中可安全使用。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_mutex_create)。

<a id="api-usb_osal_mutex_delete"></a>

### usb_osal_mutex_delete

```c
void usb_osal_mutex_delete(usb_osal_mutex_t mutex);
```

- 参数：mutex为有效本族句柄，可NULL。
- 上下文、阻塞、结果与寿命：Thread/合法syscall ISR，非阻塞；底层仅有ISR优先级断言，没有运行时ISR拒绝，断言关闭后非法IRQ不会被过滤。生命周期管理建议由Thread协调；void返回不能作为并发用户已结束的确认。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_mutex_delete)。

<a id="api-usb_osal_mutex_take"></a>

### usb_osal_mutex_take

```c
int usb_osal_mutex_take(usb_osal_mutex_t mutex);
```

- 参数：mutex为有效句柄。
- 上下文、阻塞、结果与寿命：Thread永久等待，NULL=-1；其他状态压缩为OSAL值，不可用于ISR或外层临界区。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_mutex_take)。

<a id="api-usb_osal_mutex_give"></a>

### usb_osal_mutex_give

```c
int usb_osal_mutex_give(usb_osal_mutex_t mutex);
```

- 参数：mutex为当前任务拥有的有效句柄。
- 上下文、阻塞、结果与寿命：Thread非阻塞，NULL=-1；所有权/递归层数及错误同底层，结果经OSAL映射。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_mutex_give)。

<a id="api-usb_osal_mq_create"></a>

### usb_osal_mq_create

```c
usb_osal_mq_t usb_osal_mq_create(uint32_t max_msgs);
```

- 参数：max_msgs为1..USB_MQ_DEPTH条。
- 上下文、阻塞、结果与寿命：返回uintptr_t元素队列句柄或NULL；创建失败可能输出日志，缓冲内嵌不动态分配。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_mq_create)。

<a id="api-usb_osal_mq_delete"></a>

### usb_osal_mq_delete

```c
void usb_osal_mq_delete(usb_osal_mq_t mq);
```

- 参数：mq为有效本族句柄，可NULL。
- 上下文、阻塞、结果与寿命：void注销队列并归还静态槽；不会自动释放队列中uintptr_t指向的数据。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_mq_delete)。

<a id="api-usb_osal_mq_send"></a>

### usb_osal_mq_send

```c
int usb_osal_mq_send(usb_osal_mq_t mq, uintptr_t addr);
```

- 参数：mq有效句柄；addr为事件数值或指针数值。
- 上下文、阻塞、结果与寿命：Thread自动永久等待，合法syscall ISR自动NO_WAIT；NULL=-1，其他结果经OSAL映射。只复制uintptr_t，不复制其指向载荷。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_mq_send)。

<a id="api-usb_osal_mq_recv"></a>

### usb_osal_mq_recv

```c
int usb_osal_mq_recv(usb_osal_mq_t mq, uintptr_t *addr, uint32_t timeout);
```

- 参数：mq有效；addr为可写uintptr_t*，非NULL；timeout毫秒。
- 上下文、阻塞、结果与寿命：NULL参数=-1；Thread可等待，合法syscall ISR仅timeout=0；转换tick调用recv，映射0/超时/其他错误；输出寿命覆盖整个调用。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_mq_recv)。

<a id="api-usb_osal_timer_create"></a>

### usb_osal_timer_create

```c
struct usb_osal_timer *usb_osal_timer_create(const char *name, uint32_t timeout_ms, usb_timer_handler_t handler, void *argument, bool is_period);
```

- 参数：name借用名字；timeout_ms为正毫秒周期；handler非NULL回调；argument借用参数；is_period为周期标志。
- 上下文、阻塞、结果与寿命：先从USB堆申请外层结构，再申请USB定时器槽，失败回收本轮外层块并返回NULL；成功不自动启动。回调参数是argument，回调/外部参数寿命遵守异步定时器协议。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_timer_create)。

<a id="api-usb_osal_timer_delete"></a>

### usb_osal_timer_delete

```c
void usb_osal_timer_delete(struct usb_osal_timer *timer);
```

- 参数：timer为有效外层结构，可NULL。
- 上下文、阻塞、结果与寿命：Thread/合法syscall ISR按timer删除协议；只有checked返回OK才释放外层USB堆块，否则保留；void无法反馈拒绝。handler只借用argument，不能在尚在执行回调期间释放业务参数。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_timer_delete_checked)。

<a id="api-usb_osal_timer_start"></a>

### usb_osal_timer_start

```c
void usb_osal_timer_start(struct usb_osal_timer *timer);
```

- 参数：timer为有效外层结构，可NULL。
- 上下文、阻塞、结果与寿命：解引用timer字段后转USB timer start；可能等待100 tick命令投递，void丢弃状态；成功接纳与定时器已开始不是同一判据。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_timer_start)。

<a id="api-usb_osal_timer_stop"></a>

### usb_osal_timer_stop

```c
void usb_osal_timer_stop(struct usb_osal_timer *timer);
```

- 参数：timer为有效外层结构，可NULL。
- 上下文、阻塞、结果与寿命：转USB timer stop，void不确认停止/回调结束，参数存储不能立即回收。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_timer_stop)。

<a id="api-usb_osal_enter_critical_section"></a>

### usb_osal_enter_critical_section

```c
size_t usb_osal_enter_critical_section(void);
```

- 参数：无参数。
- 上下文、阻塞、结果与寿命：调用RTOS_PORT_ENTER_CRITICAL并固定返回size_t的0；Thread/合法syscall ISR，遵守端口嵌套及禁止阻塞约束；不用于NMI/HardFault。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-usb_osal_leave_critical_section"></a>

### usb_osal_leave_critical_section

```c
void usb_osal_leave_critical_section(size_t flag);
```

- 参数：flag为对应enter返回值，当前实现忽略其数值。
- 上下文、阻塞、结果与寿命：配对调用RTOS_PORT_EXIT_CRITICAL，恢复靠端口最外层保存状态与嵌套计数；不能多退或跨不匹配生命周期退。void。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)。

<a id="api-usb_osal_msleep"></a>

### usb_osal_msleep

```c
void usb_osal_msleep(uint32_t delay);
```

- 参数：delay为毫秒。
- 上下文、阻塞、结果与寿命：Thread；转USB msleep，不是忙等，也不是可在ISR调用的精确延时。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_msleep)。

<a id="api-usb_osal_malloc"></a>

### usb_osal_malloc

```c
void *usb_osal_malloc(size_t size);
```

- 参数：size为字节数。
- 上下文、阻塞、结果与寿命：USB堆64/512两档，0或>512或指定池满返回NULL；合法syscall ISR约束同底层。只配对usb_osal_free/rtos_usb_free。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_malloc)。

<a id="api-usb_osal_free"></a>

### usb_osal_free

```c
void usb_osal_free(void *ptr);
```

- 参数：ptr为USB堆有效块，可NULL。
- 上下文、阻塞、结果与寿命：同USB free，void不报告重复/非法归还，不能交回rtos_heap_free/libc free。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/common/usb_osal.h)；[实现](../../RTOS/USB/rtos_usb_osal.c)；[底层完整契约](#api-rtos_usb_free)。

## USB驱动和介质回调补充

以下是已实现并由协议栈调用的适配入口，应用不应把它们当设备传输API调用。

<a id="api-dwc2_get_user_fifo_config"></a>

### dwc2_get_user_fifo_config

```c
void dwc2_get_user_fifo_config(uint32_t reg_base, struct usb_dwc2_user_fifo_config *config);
```

- 参数：reg_base当前忽略；config为非NULL可写usb_dwc2_user_fifo_config输出。
- 上下文、返回与副作用：控制器初始化期间由DWC2驱动调用，不阻塞。填写RX=176个32位字、EP0/1/2/3/4/5 TX=16/64/16/16/16/16字，其他TX项清0。只适配当前F446 320字FIFO，不根据reg_base自动推导硬件容量。
- 寿命：驱动配置/缓冲在整个调用有效；这些函数不接管buffer或config存储。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/port/dwc2/usb_dwc2_param.h)；[实现](../../RTOS/USB/rtos_usb_device.c)。

<a id="api-usbd_msc_get_cap"></a>

### usbd_msc_get_cap

```c
void usbd_msc_get_cap(uint8_t busid, uint8_t lun, uint32_t *block_num, uint32_t *block_size);
```

- 参数：busid/lun忽略；block_num/block_size均须非NULL可写。
- 上下文、返回与副作用：USB+MSC启用时由类驱动调用，不阻塞；写全局RAM盘容量/512字节扇区大小，没有NULL保护。配置盘大小应为正的512倍数；实际#error只检查整除关系，不能将其描述成完整非空检查。
- 寿命：驱动配置/缓冲在整个调用有效；这些函数不接管buffer或config存储。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/class/msc/usbd_msc.h)；[实现](../../RTOS/USB/rtos_usb_msc.c)。

<a id="api-usbd_msc_sector_read"></a>

### usbd_msc_sector_read

```c
int usbd_msc_sector_read(uint8_t busid, uint8_t lun, uint32_t sector, uint8_t *buffer, uint32_t length);
```

- 参数：busid/lun忽略；sector起始扇区；buffer可写至少length字节；length为512倍数字节。
- 上下文、返回与副作用：默认MSC_THREAD服务上下文；非法起始扇区/NULL/非整扇区/超介质范围返回-1且不拷贝，成功memcpy到buffer、累计读扇区数并返回0。有效sector上length=0可成功。不是物理盘I/O。
- 寿命：驱动配置/缓冲在整个调用有效；这些函数不接管buffer或config存储。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/class/msc/usbd_msc.h)；[实现](../../RTOS/USB/rtos_usb_msc.c)。

<a id="api-usbd_msc_sector_write"></a>

### usbd_msc_sector_write

```c
int usbd_msc_sector_write(uint8_t busid, uint8_t lun, uint32_t sector, uint8_t *buffer, uint32_t length);
```

- 参数：busid/lun忽略；sector起始扇区；buffer可读至少length字节；length为512倍数字节。
- 上下文、返回与副作用：默认MSC_THREAD；只读配置固定-1且不读buffer；可写配置校验同read，成功memcpy到RAM盘、累计写扇区并0，非法-1。RAM盘断电丢失，不提供文件系统持久化提交。
- 寿命：驱动配置/缓冲在整个调用有效；这些函数不接管buffer或config存储。
- 源码：[声明](../../RTOS/lib/third/CherryUSB/class/msc/usbd_msc.h)；[实现](../../RTOS/USB/rtos_usb_msc.c)。
