# 内核、任务、调度与移植层公共契约

基准：`87bfbe4a6efc50f9b828128c713a1b5af2900ef8`。逐项核对公共头及实际实现，
描述该版本的行为和调用者约束；没有修改生产源码，不把注释意图自动当作实现保证。
公共头既有应用API，也有内核/移植导出，“可见”不等于应用可直接调用。

## 检索与共用前提

- 搜索完整符号名，或使用稳定锚点`#api-符号名`；机器导航见[metadata_kernel.json](metadata_kernel.json)。
- `application`为应用入口，`weak_hook`为可覆盖钩子；`internal_export`/`port_internal`仅内核协议，
  `exception_entry`仅异常；`inline_port`/`port_lowlevel`要求端口协议；`hil_only`仅指定夹具。
- 合法syscall ISR默认指F446四位逻辑优先级5..15，编码值>=0x50。除逐项标明的只读
  寄存器/全局诊断观察外，0..4及NMI/HardFault禁止调用内核对象API。断言关闭不会放宽
  限制，也不保证运行时安全拒绝；诊断观察不授权随后修改内核状态。
- “不阻塞”指不进入任务等待，不保证短耗时、无临界区或无扫描/复制/调度。
- 所有指针须实际可访问、对齐和满足存储寿命；未经说明的API不提供任意地址探测。
- 存活TCB地址登记可拒绝未登记/构造中对象，不能识别旧指针是否指向同址重建的新任务。
- 任务优先级数值越小越高，栈容量为32位字，timeout/delay单位是RTOS tick。
  有限等待使用1..0x7fffffff；调度器对更大值执行永久阻塞，不是报PARAM或截到最大有限值。
- 实际阻塞要求运行任务、调度RUNNING，且外层没有阻止PendSV执行的屏蔽/临界区。
  当前block_permitted只检查RUNNING/current，不完整检查外层临界区误用。

## 应用入口、任务与钩子

<a id="api-rtos_get_version"></a>
### `rtos_get_version`

类别：`application`。声明：[rtos.h](../../RTOS/Core/Inc/rtos.h)；实现：[rtos_kernel.c](../../RTOS/Core/Src/rtos_kernel.c)。

```c
const char *rtos_get_version(void);
```

- 参数：无参数。
- 上下文：任意上下文只读查询。
- 行为/阻塞：返回版本常量，不阻塞、不分配内存。
- 返回/失败：返回静态字符串"1.0.0"，不是Git提交身份。
- 寿命/所有权：只读静态字符串，不得修改或释放。

<a id="api-rtos_init"></a>
### `rtos_init`

类别：`application`。声明：[rtos.h](../../RTOS/Core/Inc/rtos.h)；实现：[rtos_kernel.c](../../RTOS/Core/Src/rtos_kernel.c)。

```c
rtos_status_t rtos_init(void);
```

- 参数：无参数；BSP先完成时钟、内存和中断基础配置。
- 上下文：仅启动Thread，start之前调用一次；入口无ISR安全拒绝保证。
- 行为/阻塞：依次perf_init、heap_init、sched_init(含静态idle)、可选timer_init；不进入任务等待。
- 返回/失败：idle_tcb已存在返回ALREADY，否则逐项透传错误；成功OK。失败可能已完成前序初始化，不提供全系统事务回滚。
- 寿命/所有权：建立全局内核/服务对象，不作为运行中重置入口。
- 边界：不会自动创建所有应用IPC对象池；应用须显式init各对象。

<a id="api-rtos_start"></a>
### `rtos_start`

类别：`application`。声明：[rtos.h](../../RTOS/Core/Inc/rtos.h)；实现：[rtos_kernel.c](../../RTOS/Core/Src/rtos_kernel.c)。

```c
rtos_status_t rtos_start(void);
```

- 参数：无参数；已init成功、ready集合和BSP异常配置有效。
- 上下文：启动Thread。
- 行为/阻塞：调用sched_start，经SVC进入首个任务；成功不再返回。
- 返回/失败：ISR返回ISR；状态非NOT_STARTED返回ALREADY；ready位图无合法任务返回SCHED；正常不返回。
- 寿命/所有权：成功后使用任务各自PSP，不再继续普通启动调用流程。

<a id="api-rtos_task_create"></a>
### `rtos_task_create`

类别：`application`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_task.c](../../RTOS/Core/Src/rtos_task.c)。

```c
rtos_status_t rtos_task_create(rtos_tcb_t **tcb_out,
    uint32_t stack_size,
    rtos_task_func_t entry,
    void *arg,
    rtos_prio_t priority,
    const char *name);
```

- 参数：tcb_out：非NULL可写指针输出；stack_size：32位栈字数，至少96；entry：非NULL入口；arg：可NULL借用参数；priority：0..MAX_PRIORITIES-1；name：可NULL，否则调用期间稳定的NUL结尾字符串。
- 上下文：Thread，启动前或运行中；ISR明确拒绝。
- 行为/阻塞：heap分配TCB和栈，构造并登记为READY。更高优先级任务可能在API返回前运行。无任务等待。
- 返回/失败：ISR、NULL、PARAM(优先级/最小栈/请求超过当前heap最大单块)、NO_MEM(实际分配失败)、ALREADY(地址已保留)、OK。失败回滚本次分配，不发布tcb_out。
- 寿命/所有权：成功后TCB/栈归任务生命周期；arg内容不复制，name截断复制；删除后不得使用句柄。
- 边界：USE_STATIC_ALLOCATION=1不把此路径改成静态任务池；MAX_TASKS不是创建硬上限。预检最大块不是并发下分配成功承诺。

<a id="api-rtos_task_create_static"></a>
### `rtos_task_create_static`

类别：`application`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_task.c](../../RTOS/Core/Src/rtos_task.c)。

```c
rtos_status_t rtos_task_create_static(rtos_tcb_t *tcb,
    rtos_stack_t *stack_buf,
    uint32_t stack_size,
    rtos_task_func_t entry,
    void *arg,
    rtos_prio_t priority,
    const char *name);
```

- 参数：tcb：自然对齐控制块；stack_buf：调用方栈；stack_size：栈字数96..0x7fffffff/sizeof(rtos_stack_t)；entry非NULL；arg可NULL；priority合法；name可NULL否则有效字符串。
- 上下文：仅Thread。
- 行为/阻塞：保留TCB地址，初始化字段/栈后加入READY；无heap分配，运行中可能抢占。
- 返回/失败：ISR、NULL、PARAM(自然对齐/栈长/优先级)、ALREADY(地址构造中或存活)、OK。不要把所有失败分支都概括成完全不写存储。
- 寿命/所有权：调用方保证完整TCB/栈真实容量、互不重叠、栈8字节对齐；存储保持到任务实际切出和所有内核引用结束。
- 边界：实现仅检查stack_buf对齐到_Alignof(rtos_stack_t)，不能声称4字节对齐一定被拒绝；port会向下8字节对齐栈顶。

<a id="api-rtos_task_delete"></a>
### `rtos_task_delete`

类别：`application`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_task.c](../../RTOS/Core/Src/rtos_task.c)。

```c
rtos_status_t rtos_task_delete(rtos_tcb_t *tcb);
```

- 参数：tcb：存活任务；NULL或显式current均为自删除。
- 上下文：仅Thread；ISR始终ISR。自删除还要求当前允许切出。
- 行为/阻塞：摘调度/等待链，撤销继承，移交所持mutex，失效next并注销登记；可调度，无等待完成协议。
- 返回/失败：ISR、NULL(无当前)、PARAM(未登记/已删/idle)、SCHED(自删不能切出)、OK。自删正常切走，不能依赖之后继续业务。
- 寿命/所有权：其他动态任务立即释放TCB/栈；动态自删延至idle在切换后回收；静态不释放，但自删静态存储也要等CPU真正切出才可重用。
- 边界：不自动释放普通heap块、外设和业务缓冲。地址句柄无跨同址重建的通用代次保护。

<a id="api-rtos_task_yield"></a>
### `rtos_task_yield`

类别：`application`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_task.c](../../RTOS/Core/Src/rtos_task.c)。

```c
void rtos_task_yield(void);
```

- 参数：无参数。
- 上下文：Thread；ISR实际直接return。
- 行为/阻塞：运行且current=RUNNING、同优先级多人就绪时轮转并pend切换；否则无操作。
- 返回/失败：void，无等待/错误码。
- 寿命/所有权：不转移资源。
- 边界：头注释称ISR标记切换，当前实现却为ISR空操作；不要把它用作ISR唤醒接口。

<a id="api-rtos_task_delay"></a>
### `rtos_task_delay`

类别：`application`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_task.c](../../RTOS/Core/Src/rtos_task.c)。

```c
void rtos_task_delay(rtos_tick_t ticks);
```

- 参数：ticks：tick数量；0等价yield；有限使用1..0x7fffffff；更大值包括WAIT_FOREVER会永久BLOCKED。
- 上下文：运行Thread；ISR空操作。非零值在调度未运行/挂起/无当前时直接返回。
- 行为/阻塞：离开READY并在有限到期时重回READY；可能阻塞。
- 返回/失败：void；无错误码，返回不证明真正延时过。
- 寿命/所有权：不清理业务资源；持有他人需要的锁时延时会影响业务。
- 边界：单次有限等待上限不是系统运行寿命；超过范围不是截到最大有限值。

<a id="api-rtos_task_delay_until"></a>
### `rtos_task_delay_until`

类别：`application`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_task.c](../../RTOS/Core/Src/rtos_task.c)。

```c
void rtos_task_delay_until(rtos_tick_t *last_wake_tick, rtos_tick_t period);
```

- 参数：last_wake_tick：独占可写周期基准，首次用当前tick初始化；period：非0 tick周期，有限值应在1..0x7fffffff。
- 上下文：运行Thread；NULL/period=0、ISR、不可阻塞时直接返回，后两者不更新基准。
- 行为/阻塞：计算已过时间；基准按周期推进，落后时跳过错过的周期对齐到下一未来点，再等待剩余时间。
- 返回/失败：void；无独立范围错误码；允许路径会先改*last_wake_tick再阻塞。
- 寿命/所有权：每个周期任务保有自己的基准变量，不允许他人并发改写。
- 边界：实际是双参数周期接口，不能根据旧@param注释误当成单个绝对tick接口；不保证到期即获得CPU。

<a id="api-rtos_task_get_current"></a>
### `rtos_task_get_current`

类别：`application`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_task.c](../../RTOS/Core/Src/rtos_task.c)。

```c
rtos_tcb_t *rtos_task_get_current(void);
```

- 参数：无参数。
- 上下文：Thread/ISR只读观察；无ISR守卫。
- 行为/阻塞：直接读current_tcb，无临界区或阻塞。
- 返回/失败：返回当前任务指针，启动前可能NULL。
- 寿命/所有权：借用、不增加引用；不能跨删除长期保存。
- 边界：读取内核已发布的current_tcb；调度已选next而PendSV未提交时current仍是旧任务。
  PendSV提交后到异常返回前CPU仍处于Handler，指针已变也不证明目标任务执行过Thread代码。

<a id="api-rtos_task_get_priority"></a>
### `rtos_task_get_priority`

类别：`application`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_task.c](../../RTOS/Core/Src/rtos_task.c)。

```c
rtos_prio_t rtos_task_get_priority(const rtos_tcb_t *tcb);
```

- 参数：tcb：有效存活TCB，NULL表示current。
- 上下文：只读观察，无ISR守卫、无内部临界区或登记验证。
- 行为/阻塞：读有效priority，包括继承提升。
- 返回/失败：有目标返回priority；NULL且无current返回MAX_PRIORITIES非法哨兵。
- 寿命/所有权：非NULL须实际可访问，不能用该函数探测悬空句柄。
- 边界：不是基础优先级查询；跨字段快照和并发删除需调用方合法同步。

<a id="api-rtos_task_set_priority"></a>
### `rtos_task_set_priority`

类别：`application`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_task.c](../../RTOS/Core/Src/rtos_task.c)。

```c
rtos_status_t rtos_task_set_priority(rtos_tcb_t *tcb, rtos_prio_t prio);
```

- 参数：tcb：存活任务，NULL=current；prio：0..MAX_PRIORITIES-1。
- 上下文：Thread或合法syscall ISR；默认F446逻辑优先级5..15。RTOS_ASSERT_ISR_OK仅在断言开启时检查优先级，关闭断言不放宽契约。
- 行为/阻塞：更新base_priority并重算继承、重挂ready/等待链及持有者依赖，可能请求切换；不阻塞。
- 返回/失败：PARAM：无目标/非法prio/未登记/idle改非最低优先级；成功OK。
- 寿命/所有权：不转移TCB或栈所有权；地址复用不撤销旧裸句柄。
- 边界：有效priority可能因mutex继承保持高于所设基础prio。

<a id="api-rtos_task_suspend"></a>
### `rtos_task_suspend`

类别：`application`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_task.c](../../RTOS/Core/Src/rtos_task.c)。

```c
rtos_status_t rtos_task_suspend(rtos_tcb_t *tcb);
```

- 参数：tcb：存活任务；NULL或显式current表示自挂起。
- 上下文：Thread；合法syscall ISR只允许挂起其他任务；自挂起ISR明确拒绝。
- 行为/阻塞：摘ready/delay/对象等待；被中止等待设DELETED，撤销继承，置SUSPENDED并可请求切换。
- 返回/失败：NULL(无current)、PARAM(未登记/idle)、ISR(自挂起)、SCHED(自挂起不能切出)、OK；重复挂起存活非idle仍可OK。
- 寿命/所有权：保留TCB、栈和业务资源；不会释放所持mutex。恢复不重建被中止的原等待。
- 边界：不是业务停止握手，挂起持锁者会影响其他任务。ASSERT开时仍检查合法IRQ门槛。

<a id="api-rtos_task_resume"></a>
### `rtos_task_resume`

类别：`application`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_task.c](../../RTOS/Core/Src/rtos_task.c)。

```c
rtos_status_t rtos_task_resume(rtos_tcb_t *tcb);
```

- 参数：tcb：非NULL存活任务。
- 上下文：Thread或合法syscall ISR；默认F446逻辑优先级5..15。RTOS_ASSERT_ISR_OK仅在断言开启时检查优先级，关闭断言不放宽契约。
- 行为/阻塞：只有SUSPENDED转READY；高优先级目标可引发抢占，不阻塞。
- 返回/失败：NULL、PARAM(未登记)、OK；已非SUSPENDED也OK且不重复入链。
- 寿命/所有权：不分配/释放，不重建已取消的IPC或通知等待。
- 边界：恢复后原等待可返回DELETED；resume成功不是原资源等待成功。

<a id="api-rtos_task_notify"></a>
### `rtos_task_notify`

类别：`application`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_task.c](../../RTOS/Core/Src/rtos_task.c)。

```c
rtos_status_t rtos_task_notify(rtos_tcb_t *tcb, uint32_t value, rtos_notify_type_t type);
```

- 参数：tcb：非NULL存活目标；value：32位值/位；type：VALUE覆盖、BIT或、INCREMENT加1(忽略value)，NONE非法。
- 上下文：Thread或合法syscall ISR；默认F446逻辑优先级5..15。RTOS_ASSERT_ISR_OK仅在断言开启时检查优先级，关闭断言不放宽契约。
- 行为/阻塞：置PENDING，仅正在等自身通知的BLOCKED/DELAYED任务被唤醒；不阻塞，可请求切换。
- 返回/失败：NULL、PARAM(未登记/非法type)、OK。INCREMENT按uint32_t回绕，不返回OVERFLOW。
- 寿命/所有权：值写在TCB，不能把value中的指针视为自动受管存储。
- 边界：通知可覆盖/合并，不是逐条消息FIFO；普通delay或其他IPC等待不会因通知直接提前结束。

<a id="api-rtos_task_notify_wait"></a>
### `rtos_task_notify_wait`

类别：`application`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_task.c](../../RTOS/Core/Src/rtos_task.c)。

```c
rtos_status_t rtos_task_notify_wait(rtos_tick_t timeout, uint32_t *recv_value);
```

- 参数：timeout：tick等待，0不等，有限1..0x7fffffff，更大按永久；recv_value：可NULL或有效uint32_t输出。
- 上下文：仅Thread，ISR明确ISR。必须有current；已有PENDING可直接消费，只有真正要阻塞时要求RUNNING。
- 行为/阻塞：已有通知则取出清状态；无通知且0立即返回，否则阻塞。
- 返回/失败：ISR、SCHED(无current/不允许阻塞)、TIMEOUT、DELETED(挂起中止)、OK。无等待失败不写输出；实际阻塞恢复后错误也会写当时notify_value。
- 寿命/所有权：输出全调用有效；仅result=OK清通知。超时后才到达的PENDING保留给下次调用。
- 边界：不能以错误输出非零判断本次成功，或把所有错误都当输出不变。

<a id="api-rtos_idle_hook"></a>
### `rtos_idle_hook`

类别：`weak_hook`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_kernel.c](../../RTOS/Core/Src/rtos_kernel.c)。

```c
void rtos_idle_hook(void);
```

- 参数：无参数。
- 上下文：USE_IDLE_HOOK=1时由idle在临界区外Thread调用。
- 行为/阻塞：默认弱定义WFI，应用可同名强定义覆盖。
- 返回/失败：void；默认等中断，不是RTOS对象阻塞。
- 寿命/所有权：覆盖应快速返回，避免长期阻塞idle并延迟延期回收。
- 边界：idle不得被删除或挂起；USE_IDLE_HOOK=0时不调用，弱符号仍存在。

<a id="api-rtos_idle_task"></a>
### `rtos_idle_task`

类别：`internal_export`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_task.c](../../RTOS/Core/Src/rtos_task.c)。

```c
void rtos_idle_task(void *arg);
```

- 参数：arg：忽略，内核传NULL。
- 上下文：只供内核静态idle入口，应用禁止直接调用/创建副本。
- 行为/阻塞：循环处理自删延期回收、idle计数、WFI/idle_hook和每256轮栈巡检。
- 返回/失败：正常不返回，栈钩子默认停机。
- 寿命/所有权：idleTCB/栈归内核静态存储。
- 边界：巡检不是每次切换；PERF关闭时只扫ready/delay，不能完整覆盖永久BLOCKED/SUSPENDED。

<a id="api-rtos_stack_overflow_hook"></a>
### `rtos_stack_overflow_hook`

类别：`weak_hook`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_kernel.c](../../RTOS/Core/Src/rtos_kernel.c)。

```c
void rtos_stack_overflow_hook(rtos_tcb_t *tcb);
```

- 参数：tcb：idle巡检判定异常的借用TCB指针。
- 上下文：当前实现由idle在临界区外调用，不是保证在故障任务/异常上下文。
- 行为/阻塞：默认弱定义无限循环，可覆盖受控诊断/复位。
- 返回/失败：void，默认不返回。
- 寿命/所有权：不可释放或长期保留tcb；现场可能已损坏，不能依赖完整正常业务。
- 边界：依据TCB.stack_magic或保存sp低于stack_base，保存sp对运行任务并非实时PSP；不提供全面越界隔离。

<a id="api-rtos_task_delete_hook"></a>
### `rtos_task_delete_hook`

类别：`weak_hook`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_kernel.c](../../RTOS/Core/Src/rtos_kernel.c)。

```c
void rtos_task_delete_hook(rtos_tcb_t *tcb);
```

- 参数：tcb：已从live/perf登记移除、尚未释放的待删TCB。
- 上下文：仅USE_DELETE_HOOK=1，在delete的内核临界区内执行。
- 行为/阻塞：弱默认空操作；覆盖只能短小非阻塞清理。
- 返回/失败：void；返回后删除继续。
- 寿命/所有权：不得释放内核TCB/栈或保存指针到删除后使用。
- 边界：默认开关0；不要在钩子中等待被屏蔽IRQ、对象或调度。

<a id="api-rtos_assert_fail"></a>
### `rtos_assert_fail`

类别：`weak_hook`。声明：[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)；实现：[rtos_kernel.c](../../RTOS/Core/Src/rtos_kernel.c)。

```c
void rtos_assert_fail(const char *cond, const char *file, int line);
```

- 参数：cond/file：静态诊断字符串；line：C int源行号。
- 上下文：可由Thread、ISR、临界区或违约高IRQ触发，不能假定普通任务上下文。
- 行为/阻塞：默认弱定义忽略参数并无限循环，可覆盖受控诊断/复位。
- 返回/失败：void；默认不返回；关闭ASSERT时宏不调用且不求值条件。
- 寿命/所有权：借用诊断参数，不释放或改写。
- 边界：用户覆盖若返回，宏后续代码继续；断言不是始终存在的参数验证。

## 调度器应用观察与内部导出

<a id="api-rtos_sched_init"></a>
### `rtos_sched_init`

类别：`internal_export`。声明：[rtos_sched.h](../../RTOS/Core/Inc/rtos_sched.h)；实现：[rtos_sched.c](../../RTOS/Core/Src/rtos_sched.c)。

```c
rtos_status_t rtos_sched_init(void);
```

- 参数：无参数；应用优先使用完整rtos_init编排。
- 上下文：仅启动Thread，无统一入口ISR防护。
- 行为/阻塞：清kernel为NOT_STARTED，静态创建idle，初始化ID起点。
- 返回/失败：ALREADY(idle已建)，否则透传静态创建错误或OK；失败可能已清kernel。
- 寿命/所有权：全局静态内核，不可用作运行中复位。
- 边界：perf应先初始化，否则会漏掉idle观察登记。

<a id="api-rtos_sched_start"></a>
### `rtos_sched_start`

类别：`internal_export`。声明：[rtos_sched.h](../../RTOS/Core/Inc/rtos_sched.h)；实现：[rtos_sched.c](../../RTOS/Core/Src/rtos_sched.c)。

```c
rtos_status_t rtos_sched_start(void);
```

- 参数：无参数，初始化与异常/时钟配置正确。
- 上下文：仅启动Thread；ISR明确拒绝。
- 行为/阻塞：临时加强BASEPRI，选最高ready，置RUNNING/current，建立FPU/SysTick并SVC进入首任务。
- 返回/失败：ISR、ALREADY(非NOT_STARTED)、SCHED(无ready，恢复状态/原BASEPRI)；正常不返回。
- 寿命/所有权：当前任务与栈必须有效，不能重复启动第二个内核。
- 边界：即使普通临界区USE_BASEPRI=0，启动仍用BASEPRI保护PSP安装。

<a id="api-rtos_sched_schedule"></a>
### `rtos_sched_schedule`

类别：`internal_export`。声明：[rtos_sched.h](../../RTOS/Core/Inc/rtos_sched.h)；实现：[rtos_sched.c](../../RTOS/Core/Src/rtos_sched.c)。

```c
void rtos_sched_schedule(void);
```

- 参数：无参数；ready位图/环与current一致。
- 上下文：仅内核协议/合法异常路径，调用者已持有内核临界区；应用不可直接调用。无公共存活/上下文保护。
- 行为/阻塞：比较最高ready，按配置与状态选next并pend；非RUNNING或无ready直接返回。
- 返回/失败：void，不直接切CPU上下文。
- 寿命/所有权：调用者保证涉及TCB/栈存活。
- 边界：若仍选current会撤销过期next，并把当前READY恢复为RUNNING。

<a id="api-rtos_sched_add_ready"></a>
### `rtos_sched_add_ready`

类别：`internal_export`。声明：[rtos_sched.h](../../RTOS/Core/Inc/rtos_sched.h)；实现：[rtos_sched.c](../../RTOS/Core/Src/rtos_sched.c)。

```c
void rtos_sched_add_ready(rtos_tcb_t *tcb);
```

- 参数：tcb：非NULL、priority合法且未在ready/delay链接的内核任务。
- 上下文：仅内核协议/合法异常路径，调用者已持有内核临界区；应用不可直接调用。无公共存活/上下文保护。
- 行为/阻塞：尾插ready环并置位图，不改state，不主动schedule。
- 返回/失败：void，NULL只有ASSERT保护，关闭断言不提供空指针安全。
- 寿命/所有权：调用者先建立合法TCB并管理状态。
- 边界：重复入链或同时占用delay会破坏复用链接字段。

<a id="api-rtos_sched_remove_ready"></a>
### `rtos_sched_remove_ready`

类别：`internal_export`。声明：[rtos_sched.h](../../RTOS/Core/Inc/rtos_sched.h)；实现：[rtos_sched.c](../../RTOS/Core/Src/rtos_sched.c)。

```c
void rtos_sched_remove_ready(rtos_tcb_t *tcb);
```

- 参数：tcb：非NULL、按协议位于ready的任务。
- 上下文：仅内核协议/合法异常路径，调用者已持有内核临界区；应用不可直接调用。无公共存活/上下文保护。
- 行为/阻塞：摘ready环、更新head/bitmap、清next/prev；不改state或请求调度。
- 返回/失败：void，NULL只有ASSERT；不提供通用归属错误码。
- 寿命/所有权：不能以任意未链接/悬空TCB试探链。
- 边界：需与后续状态/调度更新组成同一事务。

<a id="api-rtos_sched_block"></a>
### `rtos_sched_block`

类别：`internal_export`。声明：[rtos_sched.h](../../RTOS/Core/Inc/rtos_sched.h)；实现：[rtos_sched.c](../../RTOS/Core/Src/rtos_sched.c)。

```c
void rtos_sched_block(rtos_tcb_t *tcb, rtos_tick_t ticks);
```

- 参数：tcb：待阻塞存活任务；ticks：tick数量，含内部0情形。
- 上下文：仅内核协议/合法异常路径，调用者已持有内核临界区；应用不可直接调用。无公共存活/上下文保护。
- 行为/阻塞：先从ready摘除；>0x7fffffff永久BLOCKED不入delay，其他值设wake_tick=now+ticks并DELAYED入链。
- 返回/失败：void；ticks=0在此不是yield，而是登记到期点。
- 寿命/所有权：调用者先完成阻塞许可和对象等待协议，再schedule并解除外层保护。
- 边界：不得绕过上层NO_WAIT判断而直接用此API模拟非阻塞。

<a id="api-rtos_sched_unblock"></a>
### `rtos_sched_unblock`

类别：`internal_export`。声明：[rtos_sched.h](../../RTOS/Core/Inc/rtos_sched.h)；实现：[rtos_sched.c](../../RTOS/Core/Src/rtos_sched.c)。

```c
void rtos_sched_unblock(rtos_tcb_t *tcb);
```

- 参数：tcb：确实BLOCKED/DELAYED的存活任务。
- 上下文：仅内核协议/合法异常路径，调用者已持有内核临界区；应用不可直接调用。无公共存活/上下文保护。
- 行为/阻塞：摘delay(若有)，置READY并入ready；若成为最高ready且运行可schedule。
- 返回/失败：void；NULL只有ASSERT，非法状态不全面拒绝。
- 寿命/所有权：对象等待结果与摘链由调用方先处理。
- 边界：重复unblock已READY会破坏链结构。

<a id="api-rtos_sched_remove_delayed"></a>
### `rtos_sched_remove_delayed`

类别：`internal_export`。声明：[rtos_sched.h](../../RTOS/Core/Inc/rtos_sched.h)；实现：[rtos_sched.c](../../RTOS/Core/Src/rtos_sched.c)。

```c
void rtos_sched_remove_delayed(rtos_tcb_t *tcb);
```

- 参数：tcb：非NULL存活任务，调用者掌握链归属。
- 上下文：仅内核协议/合法异常路径，调用者已持有内核临界区；应用不可直接调用。无公共存活/上下文保护。
- 行为/阻塞：仅state=DELAYED时摘delay；不置READY、不请求调度。
- 返回/失败：void，NULL只有ASSERT。
- 寿命/所有权：供delete/suspend等继续处理生命周期。
- 边界：不能用unblock代替此步骤，否则可能发布即将释放TCB为next。

<a id="api-rtos_sched_tick"></a>
### `rtos_sched_tick`

类别：`internal_export`。声明：[rtos_sched.h](../../RTOS/Core/Inc/rtos_sched.h)；实现：[rtos_sched.c](../../RTOS/Core/Src/rtos_sched.c)。

```c
void rtos_sched_tick(void);
```

- 参数：无参数；BSP每次真实RTOS tick恰好路由一次。
- 上下文：仅内核协议/合法异常路径，调用者已持有内核临界区；应用不可直接调用。无公共存活/上下文保护。
- 行为/阻塞：仅RUNNING递增32位tick、唤醒到期及选择最高ready/时间片，可能pend切换。
- 返回/失败：void；其他状态直接返回，无丢失tick补偿。
- 寿命/所有权：不要应用手工调用补tick；影响全部任务超时。
- 边界：USE_PREEMPTION=0不关闭此函数的其他ready选择路径。

<a id="api-rtos_sched_suspend"></a>
### `rtos_sched_suspend`

类别：`application`。声明：[rtos_sched.h](../../RTOS/Core/Inc/rtos_sched.h)；实现：[rtos_sched.c](../../RTOS/Core/Src/rtos_sched.c)。

```c
void rtos_sched_suspend(void);
```

- 参数：无参数。
- 上下文：Thread，ISR空操作。
- 行为/阻塞：RUNNING转SUSPENDED/depth1并清next；已挂起则加深。IRQ继续，RTOS tick及到期处理停止。
- 返回/失败：void；深度UINT32_MAX断言，关断言后不再增加；其他状态无操作。
- 寿命/所有权：每次合法挂起匹配resume；仅用于非阻塞工作，不保护ISR共享数据。
- 边界：不是所有带timeout调用必失败：已有资源的立即完成路径仍可成功；无暂停tick补偿。

<a id="api-rtos_sched_resume"></a>
### `rtos_sched_resume`

类别：`application`。声明：[rtos_sched.h](../../RTOS/Core/Inc/rtos_sched.h)；实现：[rtos_sched.c](../../RTOS/Core/Src/rtos_sched.c)。

```c
void rtos_sched_resume(void);
```

- 参数：无参数。
- 上下文：Thread，ISR空操作。
- 行为/阻塞：嵌套depth>1仅减1，最外层RUNNING并schedule；其他状态无操作。
- 返回/失败：void，不报告多余resume。
- 寿命/所有权：业务发布应在最外层resume前完成，可能在返回前抢占。
- 边界：不补偿挂起期间墙钟时间。

<a id="api-rtos_sched_get_tick_count"></a>
### `rtos_sched_get_tick_count`

类别：`application`。声明：[rtos_sched.h](../../RTOS/Core/Inc/rtos_sched.h)；实现：[rtos_sched.c](../../RTOS/Core/Src/rtos_sched.c)。

```c
rtos_tick_t rtos_sched_get_tick_count(void);
```

- 参数：无参数。
- 上下文：任意上下文只读。
- 行为/阻塞：直接读取volatile tick_count，无临界区或阻塞。
- 返回/失败：返回32位tick，模2^32回绕，调度挂起时停走。
- 寿命/所有权：值副本，不保证与其他字段同一快照。
- 边界：不是独立墙钟；回绕不是系统固定寿命。

<a id="api-rtos_sched_get_state"></a>
### `rtos_sched_get_state`

类别：`application`。声明：[rtos_sched.h](../../RTOS/Core/Inc/rtos_sched.h)；实现：[rtos_sched.c](../../RTOS/Core/Src/rtos_sched.c)。

```c
rtos_sched_state_t rtos_sched_get_state(void);
```

- 参数：无参数。
- 上下文：任意上下文只读。
- 行为/阻塞：直接读volatile sched_state，不阻塞。
- 返回/失败：NOT_STARTED/RUNNING/SUSPENDED/STOPPED枚举，当前无正常stop API设置STOPPED。
- 寿命/所有权：瞬时观察，随后可变化。
- 边界：一次RUNNING读取不能替代实际阻塞登记时的受保护许可判断。

<a id="api-rtos_sched_context_switch"></a>
### `rtos_sched_context_switch`

类别：`internal_export`。声明：[rtos_sched.h](../../RTOS/Core/Inc/rtos_sched.h)；实现：[rtos_sched.c](../../RTOS/Core/Src/rtos_sched.c)。

```c
void rtos_sched_context_switch(void);
```

- 参数：无参数；已经保存旧上下文且屏蔽内核IRQ。
- 上下文：仅内核协议/合法异常路径，调用者已持有内核临界区；应用不可直接调用。无公共存活/上下文保护。
- 行为/阻塞：调用内部commit更新current/next与task.state，清候选，可按配置记perf。
- 返回/失败：void；不是完整CPU现场切换，不等于yield。
- 寿命/所有权：应用直接调用会使软件current与CPU真实栈不一致。
- 边界：移植层使用返回TCB的内部commit入口；本void兼容入口不得作为业务API。

## 移植层、内联保护与异常入口

<a id="api-rtos_port_critical_nesting_error"></a>
### `rtos_port_critical_nesting_error`

类别：`internal_export`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[port.c](../../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c)。

```c
void rtos_port_critical_nesting_error(uint32_t nesting);
```

- 参数：nesting：未配对exit的0，或enter溢出的UINT32_MAX。
- 上下文：仅由临界区内联错误路径调用。
- 行为/阻塞：ASSERT检查0<nesting<UINT32_MAX，非法值触发assert_fail。
- 返回/失败：void；关断言后仅忽略参数，原inline已保留/恢复相关状态。
- 寿命/所有权：不修复缺失配对；不能继续假定失败enter已增加深度。

<a id="api-rtos_port_critical_test_hook"></a>
### `rtos_port_critical_test_hook`

类别：`hil_only`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[basepri_regressions.c](../../hil/fw/Src/basepri_regressions.c)。

```c
void rtos_port_critical_test_hook(uint32_t stage);
```

- 参数：stage：ENTER_MASKED/SAVED/COUNTED或EXIT_COUNTED/RESTORED阶段常量。
- 上下文：仅RTOS_HIL_CRITICAL_TEST专用夹具，位于临界区事务边界。
- 行为/阻塞：夹具提供定义并注入指定交错；生产无声明/调用。
- 返回/失败：void，非应用兼容接口。
- 寿命/所有权：夹具必须满足现场保存协议，应用不应补定义解决链接。

<a id="api-rtos_port_enter_critical"></a>
### `rtos_port_enter_critical`

类别：`inline_port`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)。

```c
static inline void rtos_port_enter_critical(void);
```

- 参数：无参数。
- 上下文：Thread或合法syscall IRQ；NMI/HardFault及违约高IRQ禁止。
- 行为/阻塞：短PRIMASK事务保护计数；BASEPRI模式用MAX加强并最外层保存原BASEPRI，另一模式保存原PRIMASK；加深后恢复相应状态。
- 返回/失败：void；溢出时恢复进入状态、不加深并报错；关断言不使误用有效。
- 寿命/所有权：必须配对exit，期间不得实际阻塞或等待被屏蔽IRQ；不转移资源。
- 边界：BASEPRI主体仍可被高IRQ抢占；saved_mask/深度是全局，不是每任务独立屏蔽状态。

<a id="api-rtos_port_exit_critical"></a>
### `rtos_port_exit_critical`

类别：`inline_port`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)。

```c
static inline void rtos_port_exit_critical(void);
```

- 参数：无参数；已成功enter且按执行流配对。
- 上下文：与enter相同；不能替其他执行者退出。
- 行为/阻塞：短PRIMASK事务减深度，归零恢复最外层原屏蔽；尾ISB使待处理异常先于后续指令被识别。
- 返回/失败：void；0深度不下溢，保留进入PRIMASK并报错；最外层退出可立即切换。
- 寿命/所有权：共享状态必须在exit前发布，退出后不得读可能已销毁对象来形成返回结果。
- 边界：恢复原值，不是一律BASEPRI=0；值0也不说明其他屏蔽条件消失。

<a id="api-rtos_port_in_isr"></a>
### `rtos_port_in_isr`

类别：`inline_port`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)。

```c
static inline rtos_bool_t rtos_port_in_isr(void);
```

- 参数：无参数。
- 上下文：任意上下文只读IPSR。
- 行为/阻塞：无状态修改、临界区或阻塞。
- 返回/失败：IPSR非0为TRUE，否则FALSE。
- 寿命/所有权：无资源改变。
- 边界：TRUE包括NMI/故障/SVC，不代表许可系统调用。

<a id="api-rtos_port_init_stack"></a>
### `rtos_port_init_stack`

类别：`port_internal`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[port.c](../../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c)。

```c
rtos_stack_t *rtos_port_init_stack(rtos_stack_t *stack_top,
    uint32_t stack_size,
    rtos_task_func_t entry,
    void *arg);
```

- 参数：stack_top：有效栈的一过末端；stack_size：栈字数，当前未用于边界校验；entry：非NULL入口；arg：可NULL初始R0。
- 上下文：仅任务构造协议；应用用create/static。
- 行为/阻塞：向下8字节对齐，写18个32位初始伪帧字；初始无S16-S31扩展软件帧。
- 返回/失败：返回指向R3填充字的sp；不检查NULL/容量/可执行性。
- 寿命/所有权：栈覆盖任务全过程，留足对齐、FPU及调用链真实需求。
- 边界：18字初始帧不是完整栈预算；保存布局必须与汇编一致。

<a id="api-rtos_port_context_switch"></a>
### `rtos_port_context_switch`

类别：`port_internal`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[port.c](../../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c)。

```c
void rtos_port_context_switch(void);
```

- 参数：无参数；内核已发布一致next/current。
- 上下文：仅受保护的内核调度协议。
- 行为/阻塞：可选记录pend时刻，写ICSR.PENDSVSET，DSB/ISB。
- 返回/失败：void；仅请求，实际切换等待异常/屏蔽条件允许。
- 寿命/所有权：涉及任务和栈保持有效至切换完成。
- 边界：不能以函数已返回推断目标任务已经运行。

<a id="api-rtos_port_start_first_task"></a>
### `rtos_port_start_first_task`

类别：`port_internal`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[port.c](../../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c)。

```c
void rtos_port_start_first_task(void) __attribute__((noreturn));
```

- 参数：无参数；current和初始栈/向量/优先级有效。
- 上下文：仅启动Thread。
- 行为/阻塞：设BASEPRI门槛，清PRIMASK，ISB后SVC0安装首个PSP。
- 返回/失败：声明noreturn，无配置错误返回。
- 寿命/所有权：进入首个任务，不恢复原启动调用流程。
- 边界：SVC须高于门槛；不是普通保存/恢复屏蔽接口。

<a id="api-rtos_port_start_scheduler"></a>
### `rtos_port_start_scheduler`

类别：`port_internal`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[port.c](../../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c)。

```c
void rtos_port_start_scheduler(void);
```

- 参数：无参数；SystemCoreClock>0且tick频率、除法和SysTick 24位重载范围合法。
- 上下文：仅受启动保护的Thread。
- 行为/阻塞：使能FPU ASPEN/LSPEN、清临界区计数/保存槽、写SysTick LOAD/VAL/CTRL。
- 返回/失败：void；只ASSERT检查时钟>0，不全面校验除法为0/余数/24位上界。
- 寿命/所有权：不配BSP时钟树或异常优先级，不可运行中重调。
- 边界：当前port编译要求ARMv7E-M；PRIMASK宏选项不证明支持M0/M3。

<a id="api-rtos_port_sys_tick_handler"></a>
### `rtos_port_sys_tick_handler`

类别：`exception_entry`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[port.c](../../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c)。

```c
void rtos_port_sys_tick_handler(void);
```

- 参数：无参数；BSP SysTick向量路由此入口一次。
- 上下文：合法低优先级SysTick异常。
- 行为/阻塞：RUNNING时enter→sched_tick→exit，其他状态空操作。
- 返回/失败：void，无漏tick补偿。
- 寿命/所有权：BSP可另维护HAL tick，不能重复推进RTOS tick。
- 边界：SysTick/PendSV最低优先级由BSP配置，不在此修正。

<a id="api-rtos_port_pend_sv_handler"></a>
### `rtos_port_pend_sv_handler`

类别：`exception_entry`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[port.c](../../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c)。

```c
void rtos_port_pend_sv_handler(void) __attribute__((naked));
```

- 参数：无参数；真实PendSV入口，current/PSP/栈有效。
- 上下文：仅最低优先级PendSV；禁止Thread普通调用。
- 行为/阻塞：naked汇编保存核心/条件FPU现场，保护提交current/next，恢复任务及临时屏蔽。
- 返回/失败：通过异常返回，不是普通C返回。
- 寿命/所有权：任一涉及TCB/栈须保持至实际切出完成。
- 边界：禁止套普通C包装后依赖旧r4-r11；向量分支核对最终反汇编。临时屏蔽槽不是跨任务携带任意BASEPRI。

<a id="api-rtos_port_svc_handler"></a>
### `rtos_port_svc_handler`

类别：`exception_entry`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[port.c](../../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c)。

```c
void rtos_port_svc_handler(void) __attribute__((naked));
```

- 参数：无参数；current是已构造首任务。
- 上下文：仅启动SVC Handler模式。
- 行为/阻塞：naked恢复首个现场/PSP，清BASEPRI，以EXC_RETURN进入Thread。
- 返回/失败：异常返回进入首任务，不恢复原启动C调用栈。
- 寿命/所有权：首任务TCB/栈保持有效。
- 边界：禁止Thread直接调用或普通BX到EXC_RETURN；向量直接映射/无前后序跳转须核对反汇编。

<a id="api-rtos_port_disable_interrupts"></a>
### `rtos_port_disable_interrupts`

类别：`port_lowlevel`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[port.c](../../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c)。

```c
void rtos_port_disable_interrupts(void);
```

- 参数：无参数。
- 上下文：掌握全局屏蔽协议的低层/启动代码。
- 行为/阻塞：CPSID I置PRIMASK，不改临界区深度。
- 返回/失败：void，不返回旧状态。
- 寿命/所有权：调用方安排恢复，不能等待被屏蔽的IRQ/任务。
- 边界：不是嵌套保存/恢复接口。

<a id="api-rtos_port_enable_interrupts"></a>
### `rtos_port_enable_interrupts`

类别：`port_lowlevel`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[port.c](../../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c)。

```c
void rtos_port_enable_interrupts(void);
```

- 参数：无参数。
- 上下文：掌握全局屏蔽协议的低层/启动代码。
- 行为/阻塞：CPSIE I清PRIMASK，不清BASEPRI也不改深度。
- 返回/失败：void，无条件开启PRIMASK，不恢复原值。
- 寿命/所有权：可能立即响应IRQ/切换，不能用于配对ENTER_CRITICAL。
- 边界：不能据“与disable配对”的注释推断保留进入前状态。

<a id="api-rtos_port_isr_priority"></a>
### `rtos_port_isr_priority`

类别：`port_observation`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[port.c](../../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c)。

```c
uint32_t rtos_port_isr_priority(void);
```

- 参数：无参数。
- 上下文：任意上下文只读异常号与寄存器。
- 行为/阻塞：IPSR<4返回0；外设IRQ读NVIC IP；其他系统异常读SHP。
- 返回/失败：返回原始编码，可与0x50比较；Thread/NMI/HardFault均返回0。
- 寿命/所有权：无资源变化，不阻塞。
- 边界：0不区分Thread、固定异常和逻辑IRQ0；结合IPSR解释。

<a id="api-rtos_port_get_interrupt_mask"></a>
### `rtos_port_get_interrupt_mask`

类别：`port_observation`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[port.c](../../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c)。

```c
uint32_t rtos_port_get_interrupt_mask(void);
```

- 参数：无参数。
- 上下文：任意上下文只读寄存器。
- 行为/阻塞：BASEPRI模式读BASEPRI，否则PRIMASK。
- 返回/失败：原始值，不是许可调用RTOS的布尔结果。
- 寿命/所有权：无资源变化。
- 边界：判断IRQ进展还需PRIMASK、当前异常、NVIC等。

<a id="api-rtos_port_set_interrupt_mask"></a>
### `rtos_port_set_interrupt_mask`

类别：`port_lowlevel`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[port.c](../../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c)。

```c
void rtos_port_set_interrupt_mask(uint32_t mask);
```

- 参数：mask：BASEPRI编码值或PRIMASK值，调用者保证硬件合法。
- 上下文：掌握端口协议的低层代码。
- 行为/阻塞：直接写所选寄存器并ISB，可加强/放松，不用BASEPRI_MAX且不改saved_mask或深度。
- 返回/失败：void，无参数校验，无旧值返回。
- 寿命/所有权：自行保存/恢复，不能破坏现有临界区。
- 边界：逻辑优先级5不等于编码0x50；不能据它使高IRQ获得内核调用许可。

<a id="api-rtos_port_get_critical_nesting"></a>
### `rtos_port_get_critical_nesting`

类别：`port_observation`。声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；实现：[port.c](../../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c)。

```c
uint32_t rtos_port_get_critical_nesting(void);
```

- 参数：无参数。
- 上下文：只读全局32位volatile观察。
- 行为/阻塞：直接读rtos_port_crit_nest，无临界区或阻塞。
- 返回/失败：全局深度，不是每任务深度或完整屏蔽状态。
- 寿命/所有权：与其他字段分开读不是一致快照。
- 边界：0不说明应用没有独立设置BASEPRI/PRIMASK。

## 函数式宏

<a id="api-RTOS_ASSERT"></a>
### `RTOS_ASSERT`

声明/实现：[rtos_config.h](../../RTOS/Core/Inc/rtos_config.h)。

```text
RTOS_ASSERT(cond)
```

- 参数与展开：cond应无必要业务副作用；开启时求值一次，假时调用assert_fail；关闭时不求值也不调用。
- 上下文/失败：继承调用点上下文；覆盖钩子若返回，后续代码继续。不是always-on参数校验。
- 所有权：不取得业务存储所有权，保护范围和副作用由上述展开决定。

<a id="api-RTOS_ASSERT_ISR_STATE_OK"></a>
### `RTOS_ASSERT_ISR_STATE_OK`

声明/实现：[rtos_config.h](../../RTOS/Core/Inc/rtos_config.h)。

```text
RTOS_ASSERT_ISR_STATE_OK(in_isr)
```

- 参数与展开：in_isr传入已求值布尔状态；开启时检查非ISR或编码优先级达到门槛，关闭时不求值。
- 上下文/失败：无错误码，关闭断言不使高IRQ系统调用安全。
- 所有权：不取得业务存储所有权，保护范围和副作用由上述展开决定。

<a id="api-RTOS_ASSERT_ISR_OK"></a>
### `RTOS_ASSERT_ISR_OK`

声明/实现：[rtos_config.h](../../RTOS/Core/Inc/rtos_config.h)。

```text
RTOS_ASSERT_ISR_OK()
```

- 参数与展开：开启时读取IPSR并委托STATE_OK；关闭时不读IPSR，不执行优先级检查。
- 上下文/失败：API入口的调试检查，不授权NMI/HardFault。
- 所有权：不取得业务存储所有权，保护范围和副作用由上述展开决定。

<a id="api-RTOS_PORT_ENTER_CRITICAL"></a>
### `RTOS_PORT_ENTER_CRITICAL`

声明/实现：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)。

```text
RTOS_PORT_ENTER_CRITICAL()
```

- 参数与展开：展开rtos_port_enter_critical，无参数。
- 上下文/失败：完整继承inline的上下文、配对、屏蔽和禁止实际阻塞契约。
- 所有权：不取得业务存储所有权，保护范围和副作用由上述展开决定。

<a id="api-RTOS_PORT_EXIT_CRITICAL"></a>
### `RTOS_PORT_EXIT_CRITICAL`

声明/实现：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)。

```text
RTOS_PORT_EXIT_CRITICAL()
```

- 参数与展开：展开rtos_port_exit_critical，无参数。
- 上下文/失败：完整继承inline契约；最外层退出可能立即切换任务。
- 所有权：不取得业务存储所有权，保护范围和副作用由上述展开决定。

<a id="api-RTOS_PORT_IN_ISR"></a>
### `RTOS_PORT_IN_ISR`

声明/实现：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)。

```text
RTOS_PORT_IN_ISR()
```

- 参数与展开：展开rtos_port_in_isr，无参数。
- 上下文/失败：仅报告IPSR是否非零，不报告API许可。
- 所有权：不取得业务存储所有权，保护范围和副作用由上述展开决定。

<a id="api-RTOS_PORT_DISABLE_INTERRUPTS"></a>
### `RTOS_PORT_DISABLE_INTERRUPTS`

声明/实现：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)。

```text
RTOS_PORT_DISABLE_INTERRUPTS()
```

- 参数与展开：展开rtos_port_disable_interrupts，无参数。
- 上下文/失败：直接置PRIMASK，不保存/返回旧值，也不跟踪嵌套。
- 所有权：不取得业务存储所有权，保护范围和副作用由上述展开决定。

<a id="api-RTOS_PORT_ENABLE_INTERRUPTS"></a>
### `RTOS_PORT_ENABLE_INTERRUPTS`

声明/实现：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)。

```text
RTOS_PORT_ENABLE_INTERRUPTS()
```

- 参数与展开：展开rtos_port_enable_interrupts，无参数。
- 上下文/失败：直接清PRIMASK，不恢复原值，不替代EXIT_CRITICAL。
- 所有权：不取得业务存储所有权，保护范围和副作用由上述展开决定。

<a id="api-RTOS_PORT_CRITICAL_TEST_HOOK"></a>
### `RTOS_PORT_CRITICAL_TEST_HOOK`

声明/实现：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)。

```text
RTOS_PORT_CRITICAL_TEST_HOOK(stage)
```

- 参数与展开：RTOS_HIL_CRITICAL_TEST时调用测试hook(stage)，阶段常量1..5；否则((void)0)，stage不求值。
- 上下文/失败：仅夹具注入点，生产应用不应启用或依赖；宏本身不取得业务存储所有权。
- 所有权：不取得业务存储所有权，保护范围和副作用由上述展开决定。

## 公开可见的全局变量

<a id="api-SystemCoreClock"></a>
### `SystemCoreClock`

```c
extern uint32_t SystemCoreClock;
```

声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；定义：[system_stm32f4xx.c](../../Core/Src/system_stm32f4xx.c)。

BSP维护的内核时钟Hz；端口启动时读取。直接改变量不会自动重配时钟树或SysTick，应通过完整BSP流程更新。

<a id="api-rtos_port_crit_nest"></a>
### `rtos_port_crit_nest`

```c
extern volatile uint32_t rtos_port_crit_nest;
```

声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；定义：[port.c](../../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c)。

全局临界区深度；仅端口更新，诊断只读，应用不得清零来“修复卡死”。不同于kernel.critical_nesting字段。

<a id="api-rtos_port_saved_mask"></a>
### `rtos_port_saved_mask`

```c
extern volatile uint32_t rtos_port_saved_mask;
```

声明：[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)；定义：[port.c](../../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c)。

最外层enter保存的BASEPRI或PRIMASK；按嵌套协议解释，不是实时寄存器，也不是每任务保存槽。

<a id="api-rtos_kernel"></a>
### `rtos_kernel`

```c
extern rtos_kernel_t rtos_kernel;
```

声明：[rtos_sched.h](../../RTOS/Core/Inc/rtos_sched.h)；定义：[rtos_sched.c](../../RTOS/Core/Src/rtos_sched.c)。

唯一内核状态实例；应用只读诊断，volatile不保证跨字段一致快照，不授权改写链表或current/next。

## 公共类型、枚举与ABI

来源：[rtos_types.h](../../RTOS/Core/Inc/rtos_types.h)、[rtos_task.h](../../RTOS/Core/Inc/rtos_task.h)、
[rtos_sched.h](../../RTOS/Core/Inc/rtos_sched.h)、[rtos_port.h](../../RTOS/Core/Inc/rtos_port.h)。
公开结构布局用于ABI和调试，不授权应用直接改写内核字段。

| 类型/常量 | 实际约定 |
|---|---|
| `rtos_bool_t`、`RTOS_TRUE/FALSE` | uint8_t，标准真假1/0；不能用它接收负的状态码 |
| `rtos_prio_t` | uint32_t；合法任务优先级0..MAX_PRIORITIES-1，越小越高；不是NVIC编码值 |
| `rtos_tick_t`、`RTOS_NO_WAIT/WAIT_FOREVER` | uint32_t；0不等待、0xffffffff永久；底层sched_block对0不是yield |
| `rtos_stack_t` | uint32_t；栈参数为字数，字节数=stack_size*sizeof(rtos_stack_t) |
| `rtos_task_func_t` | `void (*)(void *arg)`；arg借用；入口应循环或明确自删，直接return走内部exit_handler |
| `rtos_timer_cb_t` | `void (*)(void *arg)`；回调在timer服务任务执行，非硬件IRQ；详见服务契约 |
| `rtos_hook_t` | `void (*)(void)`；类型自身不指定钩子实际上下文 |
| `rtos_status_t` | OK=0，PARAM=-1，NO_MEM=-2，TIMEOUT=-3，ISR=-4，ALREADY=-5，NULL=-6，OVERFLOW=-7，DELETED=-8，SCHED=-9；每API只返回其实现子集 |
| `rtos_task_state_t` | READY=0、RUNNING=1、BLOCKED=2、SUSPENDED=3、DELAYED=4、DELETED=5、INITIALIZING=6。有限对象等待通常DELAYED，永久等待BLOCKED；只看state不能判断等待原因 |
| `rtos_notify_type_t` | NONE=0（当前notify不支持）、VALUE=1、BIT=2、INCREMENT=3 |
| `rtos_sched_state_t` | NOT_STARTED=0、RUNNING=1、SUSPENDED=2、STOPPED=3；当前没有正常stop API |
| `RTOS_PORT_STACK_ALIGN_MASK` | STACK_ALIGNMENT-1转换为rtos_stack_t，当前7，用于栈顶向下对齐 |
| `RTOS_PORT_EXC_RETURN_NO_FPU/WITH_FPU` | 0xfffffffd/0xffffffed；由异常帧决定，不是普通C函数指针 |
| `RTOS_VERSION_MAJOR/MINOR/PATCH/STRING` | 1/0/0及"1.0.0"；版本字符串不唯一标识源码/配置 |
| `RTOS_PORT_TEST_ENTER_MASKED/ENTER_SAVED/ENTER_COUNTED/EXIT_COUNTED/EXIT_RESTORED` | RTOS_HIL_CRITICAL_TEST下分别为1/2/3/4/5；仅夹具事务阶段，不是应用状态码 |

`rtos_tcb_t`是struct rtos_tcb的前向声明；sem/mutex/queue/event/timer也在types头前向声明，
各自完整布局位于对应头文件。TCB布局受配置影响，不可跨配置混用存储或手写固定汇编偏移。

| TCB字段组 | 所有者及可观察含义 |
|---|---|
| top_of_stack / stack_base / stack_size | 内核保存的sp、最低地址、栈字数；top_of_stack必须偏移0。运行中保存sp可能落后实时PSP |
| priority / base_priority / state / is_static | 有效/基础优先级、状态、内核是否回收TCB/栈；应用通过API修改 |
| next_ready / prev_ready / next_all | 前两字段被ready环、delay链或延期释放链复用；next_all为独立构造/存活地址登记链 |
| wait_node / wake_tick / blocked_on / wait_result | 内部等待协议；通知用blocked_on==(void*)tcb哨兵。唤醒/切换存在中间窗口，不能仅凭单字段判错 |
| notify_value / notify_state | 值与内部IDLE/PENDING/RECEIVED状态；通过notify/wait消费，应用不直接改写 |
| name / task_id / switch_count | 名称截断到MAX_TASK_NAME_LEN-1字节；ID是uint32计数，当前无耗尽拒绝，极限会回绕；切换计数也会回绕 |
| run_time_counter / stack_magic / perf_* | 条件编译字段；热路径统计未启用时相关0值不能解释为任务从未运行 |

`rtos_wait_node_t`是TCB内嵌侵入式协议节点，包括on_timeout、tcb、wake_tick、timed_out、
wait_obj、next/prev、transfer_buf、transfer_size、to_front、list_head、queue_bitmap、
queued_priority。内核负责登记/清理，应用不应自行构造或改写。on_timeout在仍持有对象等待
关系的临界区中捕获结果。transfer_buf所称“零拷贝”只指跳过中间环形缓存，队列直传仍执行memcpy。

`rtos_kernel_t`中的ready_bitmap/ready_heads、delay_head、current_tcb/next_tcb、idle_tcb、
sched_state/tick_count、switch_count、suspend_depth及perf字段均归内核协议所有。current与next
在PendSV提交前可不同，不应根据一次采样强制写成相等。kernel.critical_nesting字段不等同于
实际port维护的rtos_port_crit_nest。volatile不保证多个字段来自同一原子采样。

## 配置的实际作用与一致性

配置源：[rtos_config.h](../../RTOS/Core/Inc/rtos_config.h)。除ASSERT_ENABLED、USE_BASEPRI等
显式#ifndef项外，多数宏直接#define，命令行-D不能可靠覆盖。应在明确配置副本中统一设置并
记录全部翻译单元的配置；涉及对象布局的改动必须完整重编译。以下覆盖该头当前配置项。

| 配置（省略RTOS_CONFIG_前缀） | 默认值及实现边界 |
|---|---|
| TICK_RATE_HZ | 1000；不是通用ms单位；BSP须验证时钟除法及SysTick重载范围 |
| MAX_PRIORITIES | 32；单32位位图，配置必须适合1..32，不提供扩成>32的实现 |
| USE_TIME_SLICING | 1；控制tick同优先级轮转；显式yield有独立轮转逻辑 |
| USE_PREEMPTION | 1；影响sched_schedule部分分支；当前tick仍选择其他ready任务，不可据0声称完整协作式 |
| MINIMAL_STACK_SIZE | 128字；建议值，task_create/static的实际硬校验最小96字 |
| IDLE_TASK_STACK_SIZE | 128字；idle静态栈，须满足真实调用需求 |
| USE_IDLE_HOOK / USE_DELETE_HOOK | 1/0；控制调用点，不删除可覆盖弱符号本身 |
| MAX_TASK_NAME_LEN | 16含NUL；默认最多15字节名称，不是Unicode字符数量 |
| USE_STATIC_ALLOCATION / MAX_TASKS | 1/16；task_create仍heap、static仍外部存储，不能解释成16个通用静态任务池硬上限；MAX_TASKS还参与perf观察容量 |
| HEAP_SIZE | 32768字节；动态任务始终依赖该heap，并非只在USE_STATIC_ALLOCATION=0时存在 |
| MAX_SEMAPHORES / MAX_MUTEXES / MAX_QUEUES / MAX_EVENT_GROUPS | 各8；不是用户存储init接口的统一容量保证，实际限制须按各IPC实现核对；USB有独立OSAL容量 |
| USE_TIMERS | 1；rtos_init建立timer服务；关闭存根/错误见服务契约 |
| TIMER_TASK_STACK_SIZE / TIMER_TASK_PRIORITY | 256字/30；回调在服务任务执行，长耗时会影响其他软件定时器 |
| GENERATE_RUN_TIME_STATS | 1；控制TCB/kernel统计字段，不等于所有运行时长都在测量 |
| CHECK_FOR_STACK_OVERFLOW | 2；当前按非零启用TCB魔数和idle巡检，没有注释所暗示的方法1/2完整分支差异 |
| ASSERT_ENABLED | 1；0使条件不求值，优先级违约不再由此捕获，调用限制仍有效 |
| USE_PERF_MONITOR / PERF_USE_DWT | 1/1；性能结构/实现及DWT选择；见性能契约，不能从注释中的M0回退推断port支持M0 |
| PERF_TASK_REGISTRY_SIZE / PERF_POOL_REGISTRY_SIZE | MAX_TASKS+2=18 / 8；观察登记容量，不是任务构造统一上限 |
| PERF_STACK_WATERMARK | 1；创建时未用栈水印，历史观测不是最大栈严格证明 |
| PERF_HOTPATH_STATS | 0；控制pend/commit的统计钩子；相关时长/CPU/延迟为0先检查配置 |
| USE_USB / USB_DEVICE_MODE / USB_HOST_MODE | 1/1/0；具体函数/关闭存根见USB契约 |
| USB_OSAL_SEM_COUNT / USB_OSAL_MUTEX_COUNT / USB_OSAL_MQ_COUNT / USB_OSAL_THREAD_COUNT / USB_OSAL_TIMER_COUNT | 8/8/4/4/8；USB封装对象池，非应用IPC全局上限 |
| USB_THREAD_STACK_SIZE / USB_THREAD_PRIORITY / USB_MQ_DEPTH | 512字 / 16 / 16条；USB默认线程/队列属性，priority仍是任务逻辑值 |
| USB_ISR_TABLE_SIZE | 2；USB映射表容量，非NVIC总IRQ数 |
| USB_USE_MSC / USB_USE_HID | 1/1；F446复合设备集成，编译存在不证明物理主机互操作 |
| USB_MSC_RAM_DISK_SIZE / USB_MSC_READONLY | 32768字节 / 0；RAM盘及写保护，不跨复位持久化 |
| USB_HEAP_SIZE | 8192字节配置；实际分档布局和分配限制见USB契约，不推断等于通用heap可用字节，也不与libc malloc/rtos_heap混用 |
| STACK_ALIGNMENT | 8字节；port向下对齐栈顶，调用者保证有效容量与对齐 |
| USE_BASEPRI / MAX_SYSCALL_INTERRUPT_PRIORITY | 1 / (5<<4)=0x50；编码门槛不是逻辑优先级5；PRIMASK模式屏蔽全部可配置IRQ，但不放宽非法上下文契约 |

## 当前实现的核对重点

- yield的ISR路径、静态栈对齐守卫、USE_STATIC_ALLOCATION、方法1/2栈检查、USE_PREEMPTION=0等，
  注释与实际行为存在差别，应用应使用上面的逐项契约。
- block_permitted不检查外层临界区深度，应用仍必须遵守“不在临界区实际阻塞”。
  PendSV被外层屏蔽时，不能因守卫存在就假设误用会安全报错。
- ID及多项统计不饱和、会回绕；裸TCB地址不提供跨同址重建的代次句柄。
- 某翻译单元独立改配置会造成ABI不一致；核对全部编译输入，而非只看当前头。
- 本页为静态源码契约核对，不宣称新增实板覆盖；示例编译结果由AI总入口记录。
