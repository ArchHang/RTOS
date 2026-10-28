# IPC与内存公共API契约

基准：`87bfbe4a6efc50f9b828128c713a1b5af2900ef8`，STM32F446RE、32位Cortex-M4F。
本页32项函数逐项对照六个公共头文件及六个实现，并核对共用等待、任务登记和调度路径。
函数名是检索主键；每个`api-函数名`锚点对应一项契约。原型来自头文件，行为来自本基线实现。
通用入口见[AI项目说明](README.md)。本页属于源码审阅，不把文档或编译成功当成运行验证。

## 公共调用约定

- **生存期和同步**：控制块自然对齐、可访问，缓存实际容量充足，覆盖全部并发使用期；控制块与缓存不互相覆盖。IPC初始化标志不是地址有效性注册表，函数可能在进入临界区前读取它；不能用API探测已释放对象。应用不直接改等待链、计数或owner。
- **ISR**：“合法ISR”指系统调用级中断。F446默认逻辑优先级5–15，编码门槛0x50；0–4及NMI/HardFault不属于允许范围。ISR优先级断言只在RTOS_CONFIG_ASSERT_ENABLED开启时检查；关闭断言不放宽契约，也不保证违约时返回错误。mutex take/give另有独立ISR拒绝，其他接口不能类推。
- **阻塞与tick**：timeout单位tick，0为RTOS_NO_WAIT，1..0x7FFFFFFF为支持的有限等待，0xFFFFFFFF为RTOS_WAIT_FOREVER。当前调度器把0x80000000..0xFFFFFFFE也作为永久阻塞处理，而非有限长等待或参数错误。需阻塞时必须已有当前任务且调度器RTOS_SCHED_RUNNING。外层临界区或自行屏蔽PendSV/SysTick不一定被守卫识别，仍禁止在这些条件下阻塞。立即完成路径不因调度器暂停而自动失败。
- **初始化/销毁**：init允许未初始化存储，不代表deinit能接受垃圾字节。sem/queue/event通过任务登记状态阻止仍有等待者时重建；无等待者时允许重置，可能丢弃原状态。deinit不证明所有已开始调用和裸指针都已停止；先停止新访问并协调旧用户退出，才可释放或复用存储。
- **观察查询**：无锁getter不预留资源，不验证初始化。任务/合法ISR中的返回值只是观察；额外上下文与并发释放安全不由getter保证。不能由“无锁”推导NMI/高优先级ISR可任意访问正在变化或销毁的对象。
- **失败边界**：普通拒绝在合法对象状态下不转移资源归属；对象损坏、越界/重叠缓冲或并发释放等违约输入不具有必然报错或事务回滚保证。任务自身被删除后不会返回原阻塞调用。
- **完成与取消的先后**：deinit或挂起的取消语义针对仍登记为等待中的调用。若give、队列直传或事件set已经授予资源/记录快照、任务已RTOS_TASK_READY，随后挂起它不自动撤销此前已经完成的结果；不能把“尚未返回到业务代码”一律等同于“仍在等待链上”。
- **配置**：这些函数没有TIMERS/USB那样的关闭存根。RTOS_CONFIG_MAX_SEMAPHORES/RTOS_CONFIG_MAX_QUEUES/RTOS_CONFIG_MAX_EVENT_GROUPS不是相应自备控制块init中的强制总量限制；RTOS_CONFIG_MAX_MUTEXES实际限制登记表。当前实现的task_create始终从heap申请TCB及栈，task_create_static始终使用调用方存储；RTOS_CONFIG_USE_STATIC_ALLOCATION不切换这两条路径，也不禁用heap。

## 数据结构及事件模式

`rtos_sem_t`保存令牌计数、上限及等待位图；`rtos_mutex_t`保存owner、递归计数和等待链；
`rtos_queue_t`保存用户缓存、固定item_size、capacity、环形索引及两条等待链；
`rtos_event_t`保存事件位和等待链；`rtos_mempool_t`保存块范围、块长、空闲链及计数。
这些字段可供受控调试解释，不是应用可自由写入的扩展协议。

| 宏/类型 | 当前含义 |
|---|---|
| `rtos_event_bits_t` | uint32_t；公开说明保留高8位，应用使用低24位；实现并未屏蔽/拒绝高8位，不能宣称已执行24位验证 |
| `RTOS_EVENT_WAIT_ANY` | 0，任一等待位已置位即满足 |
| `RTOS_EVENT_WAIT_ALL` | 1，全部等待位已置位才满足 |
| `RTOS_EVENT_CLEAR_ON_EXIT` | 1U<<24，属于mode参数；满足时清除整个等待掩码，返回清位前快照；不是所有返回路径都清位 |

mode只接受bit0和bit24。事件数据与mode是不同字段，不把RTOS_EVENT_CLEAR_ON_EXIT的模式位当作业务事件位。

## 信号量

来源：[rtos_sem.h](../../RTOS/Core/Inc/rtos_sem.h)、[rtos_sem.c](../../RTOS/IPC/rtos_sem.c)。

<a id="api-rtos_sem_init"></a>

### rtos_sem_init

```c
rtos_status_t rtos_sem_init(rtos_sem_t *sem, uint32_t init_cnt, uint32_t max_cnt);
```

- **参数**：sem为可写自然对齐控制块，首次允许未初始化内容；init_cnt为初始令牌数；max_cnt>0且init_cnt<=max_cnt。
- **上下文/阻塞**：启动、任务或合法ISR；不阻塞，有ISR优先级断言。
- **返回**：RTOS_OK；NULL、零上限或初值超限为RTOS_ERR_PARAM；有登记等待者为RTOS_ERR_ALREADY。
- **状态/失败**：成功清零对象、建立计数与位图；无等待者时重init会重置现有计数，非保持状态的幂等操作。拒绝路径不改对象。
- **寿命/配置**：不分配堆，RTOS_CONFIG_MAX_SEMAPHORES不在此处限制数量。重建前排除旧调用者，不会撤销旧指针。

<a id="api-rtos_sem_take"></a>

### rtos_sem_take

```c
rtos_status_t rtos_sem_take(rtos_sem_t *sem, rtos_tick_t timeout);
```

- **参数**：sem已初始化且有效；timeout为tick，遵守本页公共取值范围。
- **上下文/阻塞**：任务；启动仅立即路径；合法ISR必须RTOS_NO_WAIT。无令牌且非零timeout时可能阻塞，不得持外层临界区。
- **返回**：RTOS_OK；NULL为RTOS_ERR_NULL；未初始化为RTOS_ERR_PARAM；ISR非零timeout为RTOS_ERR_ISR；RTOS_NO_WAIT无令牌/实际超时为RTOS_ERR_TIMEOUT；需阻塞但条件不允许为RTOS_ERR_SCHED；deinit或任务挂起中止等待为RTOS_ERR_DELETED。
- **状态/所有权**：count>0时减一；等待时登记TCB节点。give直接授予等待者，此后take成功不再额外减count。优先级小者先，同优先级依等待次序。
- **寿命/限制**：正常阻塞返回只读取TCB结果，不再访问已脱离的sem。没有令牌拥有者记录，删除任务不会自动补回它已经取得的令牌。

<a id="api-rtos_sem_give"></a>

### rtos_sem_give

```c
rtos_status_t rtos_sem_give(rtos_sem_t *sem);
```

- **参数**：sem为已初始化有效对象。
- **上下文/阻塞**：启动、任务或合法ISR；不等待，有优先级断言；可能唤醒任务并请求切换。
- **返回**：RTOS_OK；NULL为RTOS_ERR_NULL；未初始化为RTOS_ERR_PARAM；无等待者且count>=max_count为RTOS_ERR_OVERFLOW。
- **状态/失败**：有等待者时授予最高优先级等待者，count不增；无等待者时count加一。溢出保持count。
- **限制**：不验证give者曾take，不提供互斥所有权或继承。位图选择仍可扫描等待链，不保证完整操作固定周期。

<a id="api-rtos_sem_get_count"></a>

### rtos_sem_get_count

```c
uint32_t rtos_sem_get_count(const rtos_sem_t *sem);
```

- **参数**：sem为NULL或仍有效的控制块地址。
- **上下文/阻塞**：无锁、无ISR检查、不阻塞；按公共观察查询约定使用。
- **返回**：NULL为0，否则读取count；不检查初始化，deinit后可能仍返回旧计数。
- **状态/限制**：不改对象、不预留令牌。0不能区分NULL与无令牌；count>0不能代替take或证明对象可释放。

<a id="api-rtos_sem_deinit"></a>

### rtos_sem_deinit

```c
rtos_status_t rtos_sem_deinit(rtos_sem_t *sem);
```

- **参数**：sem曾成功init、等待链仍完整且存储有效；不能销毁未初始化垃圾字节。
- **上下文/阻塞**：启动、任务或合法ISR；有优先级断言，不等待，可唤醒全部等待者。
- **返回**：NULL为RTOS_ERR_NULL；满足前置条件返回RTOS_OK，没有单独未初始化/重复销毁错误码。
- **状态**：以RTOS_ERR_DELETED唤醒等待者，清wait_bitmap与初始化标志；保留count/max_count，不释放控制块。
- **寿命/限制**：先停止新用户并协调旧调用退出。重复销毁仅能用于第一次销毁后尚未释放/重用、等待链仍完整的对象，不是任意地址有效性检测。

## 互斥锁

来源：[rtos_mutex.h](../../RTOS/Core/Inc/rtos_mutex.h)、[rtos_mutex.c](../../RTOS/IPC/rtos_mutex.c)。

<a id="api-rtos_mutex_init"></a>

### rtos_mutex_init

```c
rtos_status_t rtos_mutex_init(rtos_mutex_t *mutex);
```

- **参数**：mutex为可写自然对齐控制块，首次允许未初始化内容。
- **上下文/阻塞**：启动、任务或合法ISR；不等待，有优先级断言。
- **返回**：NULL为RTOS_ERR_NULL；同地址已登记且仍有owner/等待者/非零递归数为RTOS_ERR_ALREADY；登记表满为RTOS_ERR_NO_MEM；成功RTOS_OK。
- **状态/失败**：首次清零、设置初始化标志并登记。同地址已登记且空闲时直接RTOS_OK，不再次清零或占新登记槽；拒绝保持对象。
- **寿命/配置**：RTOS_CONFIG_MAX_MUTEXES实际限制活跃登记数，deinit释放名额。应用不改owner/等待链；对象保持有效至协调注销结束。

<a id="api-rtos_mutex_take"></a>

### rtos_mutex_take

```c
rtos_status_t rtos_mutex_take(rtos_mutex_t *mutex, rtos_tick_t timeout);
```

- **参数**：mutex已初始化且有效；timeout为tick。
- **上下文/阻塞**：仅实际已有当前TCB的任务。任何ISR即使RTOS_NO_WAIT也由运行时返回RTOS_ERR_ISR。启动阶段无当前任务时禁止取锁；实现没有对current_tcb==NULL的独立拒绝，空闲路径甚至可能返回RTOS_OK并留下owner=NULL，不能将禁止约束当作运行时报错保证。竞争且非零等待可阻塞。
- **返回**：RTOS_OK；NULL为RTOS_ERR_NULL；未初始化为RTOS_ERR_PARAM；ISR为RTOS_ERR_ISR；递归数UINT32_MAX时再次take为RTOS_ERR_OVERFLOW；他人持锁且RTOS_NO_WAIT/超时为RTOS_ERR_TIMEOUT；需阻塞但条件不允许为RTOS_ERR_SCHED；等待被销毁/挂起中止为RTOS_ERR_DELETED。
- **状态/失败**：空闲时owner设当前任务、递归数1；自身持有时递归数加一。竞争时登记依赖并求传递优先级继承。give先交接owner再唤醒，成功返回者已持锁。溢出保持原所有权/计数。
- **限制**：继承不解除锁顺序死锁。删除持有任务可触发锁转交，但不保证被保护业务数据已经恢复一致；需业务设计恢复协议。

<a id="api-rtos_mutex_give"></a>

### rtos_mutex_give

```c
rtos_status_t rtos_mutex_give(rtos_mutex_t *mutex);
```

- **参数**：mutex已初始化，当前任务是owner且递归数>0。
- **上下文/阻塞**：仅实际任务；任何ISR运行时返回RTOS_ERR_ISR，不等待。
- **返回**：RTOS_OK；NULL为RTOS_ERR_NULL；未初始化、非owner或递归数0为RTOS_ERR_PARAM；ISR为RTOS_ERR_ISR。
- **状态/失败**：递归数减一；大于0继续持锁。归零时在同一临界区向最高优先级等待者转交owner、递归数1，或清owner，再重算优先级和调度。普通拒绝不改变锁。
- **限制**：最后一次give可使高优先级任务先运行；调用方不能把give之后的数据访问仍当作受该锁保护。

<a id="api-rtos_mutex_deinit"></a>

### rtos_mutex_deinit

```c
rtos_status_t rtos_mutex_deinit(rtos_mutex_t *mutex);
```

- **参数**：mutex曾成功init，结构完整、存储有效；销毁前协调持有者及其他用户。
- **上下文/阻塞**：启动、任务或合法ISR；使用ISR优先级断言，与take/give的运行时ISR拒绝不同；不等待。
- **返回**：NULL为RTOS_ERR_NULL，其余满足前置条件时RTOS_OK；不要求调用者为owner，无独立重复销毁错误码。
- **状态**：清owner，以RTOS_ERR_DELETED唤醒所有等待者，清初始化标志、移除登记槽并重算旧owner优先级。保留recursion_count，不释放存储。
- **寿命/限制**：可以撤销仍被持有的锁是当前实现行为，不是安全业务销毁流程；旧持有者随后give会因未初始化失败。不能对垃圾对象调用deinit。

## 消息队列

来源：[rtos_queue.h](../../RTOS/Core/Inc/rtos_queue.h)、[rtos_queue.c](../../RTOS/IPC/rtos_queue.c)。

<a id="api-rtos_queue_init"></a>

### rtos_queue_init

```c
rtos_status_t rtos_queue_init(rtos_queue_t *queue, void *storage, uint32_t item_size, uint32_t capacity);
```

- **参数**：queue为可写控制块，首次可未初始化；storage为独立可读写缓存，至少item_size*capacity字节。item_size>0，单位字节；capacity>0，单位条，乘积<=UINT32_MAX。
- **上下文/阻塞**：启动、任务或合法ISR；有优先级断言，不阻塞。
- **返回**：NULL、零大小/容量或乘积溢出为RTOS_ERR_PARAM；存在登记发送/接收等待者为RTOS_ERR_ALREADY；成功RTOS_OK。
- **状态/失败**：成功清零控制块、绑定缓存、count/head/tail归零。不清零storage。无等待者时重建会丢弃原排队内容，不逐条处理其中业务资源；拒绝不改对象。
- **寿命/配置**：接口无实际容量参数，不能验证虚报范围；控制块与storage不得重叠。RTOS_CONFIG_MAX_QUEUES不在此处限制自备队列数，无通用旧句柄代次校验。

<a id="api-rtos_queue_send"></a>

### rtos_queue_send

```c
rtos_status_t rtos_queue_send(rtos_queue_t *queue, const void *item, rtos_tick_t timeout, rtos_bool_t to_front);
```

- **参数**：queue已初始化且缓存有效；item至少有item_size可读字节，整个调用返回前有效、内容稳定，不与控制块、队列缓存或接收目标重叠；timeout为tick；to_front使用RTOS_FALSE尾插、RTOS_TRUE头插，不覆盖旧消息。
- **上下文/阻塞**：任务；启动仅立即路径；合法ISR必须RTOS_NO_WAIT。高优先级ISR仍禁止，关断言不保证拒绝。满且非零等待时可能阻塞。
- **返回**：RTOS_OK；NULL queue/item为RTOS_ERR_NULL；未初始化或检测到错误等待缓冲为RTOS_ERR_PARAM；ISR非零timeout为RTOS_ERR_ISR；RTOS_NO_WAIT且满/实际超时为RTOS_ERR_TIMEOUT；需阻塞但不允许为RTOS_ERR_SCHED；销毁/挂起中止等待为RTOS_ERR_DELETED。
- **状态/所有权**：有接收等待者时直接memcpy到其缓冲并唤醒，否则复制进环形缓存。阻塞时保存item指针，接收者可能代为复制入队。RTOS_OK只表示已接纳，不代表消费业务完成。
- **限制**：复制固定字节；发送指针只复制指针值，不管理所指内存。直传仍为一次memcpy。源码重试分支复用原timeout，不额外保证跨重试总预算；普通拒绝不构成指针消息的自动释放。

<a id="api-rtos_queue_recv"></a>

### rtos_queue_recv

```c
rtos_status_t rtos_queue_recv(rtos_queue_t *queue, void *item, rtos_tick_t timeout);
```

- **参数**：queue已初始化、缓存有效；item至少有item_size可写字节，直到返回均有效且不被并发访问，不与控制块/队列缓存或发送源重叠；timeout为tick。
- **上下文/阻塞**：任务；启动仅立即路径；合法ISR必须RTOS_NO_WAIT。空且非零等待时可阻塞，不能持有外层临界区。
- **返回**：RTOS_OK；NULL为RTOS_ERR_NULL；未初始化或等待缓冲不匹配为RTOS_ERR_PARAM；ISR非零timeout为RTOS_ERR_ISR；RTOS_NO_WAIT且空/超时为RTOS_ERR_TIMEOUT；不允许阻塞为RTOS_ERR_SCHED；销毁/挂起中止等待为RTOS_ERR_DELETED。
- **状态/输出**：有数据时复制出一条，必要时代阻塞发送者填补空位并保留其头插意图。等待时保存输出指针，发送者可直写后唤醒。正常RTOS_ERR_TIMEOUT/RTOS_ERR_DELETED路径不写业务输出。
- **限制**：仅在RTOS_OK时将输出当作新消息；错误时不能消费旧哨兵值。损坏对象不提供事务回滚保证。源码重试分支仍复用原timeout。

<a id="api-rtos_queue_get_count"></a>

### rtos_queue_get_count

```c
uint32_t rtos_queue_get_count(const rtos_queue_t *queue);
```

- **参数**：queue为NULL或仍有效的控制块。
- **上下文/阻塞**：无锁、无ISR/初始化检查，不阻塞，按公共观察约定使用。
- **返回/状态**：NULL为0，否则返回count，包括deinit后保留的旧count。不修改或预留消息。
- **限制**：不能用count>0替代recv返回值，查询后队列可以立即改变；不能证明对象有效或可释放。

<a id="api-rtos_queue_get_free"></a>

### rtos_queue_get_free

```c
uint32_t rtos_queue_get_free(const rtos_queue_t *queue);
```

- **参数**：queue为NULL或仍有效的控制块。
- **上下文/阻塞**：无锁、无ISR/初始化检查，不阻塞，按公共观察约定使用。
- **返回/状态**：NULL为0；capacity>count时为capacity-count，否则0。不预留空位；实现可能多次读取count。
- **限制**：返回空位不保证后续send成功，与另一次get_count不是同一原子快照。deinit旧字段仍可被读出。

<a id="api-rtos_queue_deinit"></a>

### rtos_queue_deinit

```c
rtos_status_t rtos_queue_deinit(rtos_queue_t *queue);
```

- **参数**：queue曾成功初始化，等待链仍完整，存储有效。
- **上下文/阻塞**：启动、任务或合法ISR；有优先级断言，不等待，可唤醒全部等待者。
- **返回**：NULL为RTOS_ERR_NULL；满足前置条件返回RTOS_OK，无独立重复销毁错误码。
- **状态**：以RTOS_ERR_DELETED唤醒发送/接收等待者，清初始化标志；保留storage、count及索引，不释放或清零缓存，不释放指针消息所指资源。
- **寿命/限制**：先协调停止新访问及旧调用，再销毁和清理业务资源。旧裸指针在同址重建后没有通用代次识别，不能把deinit当垃圾对象检测。

## 事件组

来源：[rtos_event.h](../../RTOS/Core/Inc/rtos_event.h)、[rtos_event.c](../../RTOS/IPC/rtos_event.c)。

<a id="api-rtos_event_init"></a>

### rtos_event_init

```c
rtos_status_t rtos_event_init(rtos_event_t *event);
```

- **参数**：event为可写自然对齐控制块，首次可未初始化。
- **上下文/阻塞**：启动、任务或合法ISR；有优先级断言，不等待。
- **返回**：NULL为RTOS_ERR_NULL；有登记等待者为RTOS_ERR_ALREADY；成功RTOS_OK。
- **状态/失败**：成功清零对象和所有事件位；无等待者时重建会丢弃旧状态，拒绝保持对象。
- **寿命/配置**：RTOS_CONFIG_MAX_EVENT_GROUPS不在此处强制总量；重建前协调旧用户，不能撤销旧指针。

<a id="api-rtos_event_set"></a>

### rtos_event_set

```c
rtos_event_bits_t rtos_event_set(rtos_event_t *event, rtos_event_bits_t bits);
```

- **参数**：event已初始化且有效；bits为置位掩码，0不新增位；应用使用低24位。
- **上下文/阻塞**：启动、任务或合法ISR；有优先级断言，不等待。临界区内可扫描并唤醒多个等待者。
- **返回**：返回遍历和消费清位后的完整event->bits；NULL或未初始化（含受保护复查失败）返回0，无独立错误码。
- **状态**：先OR bits，沿优先级等待链逐个判断；满足者先保存完整快照，再依RTOS_EVENT_CLEAR_ON_EXIT清等待掩码并唤醒。先消费会影响后续等待者。
- **限制**：不是无条件广播。返回最终剩余位，可不同于被唤醒者的快照。实现未屏蔽/拒绝高8位，不据此宣称公开接口已扩展为32位事件。

<a id="api-rtos_event_clear"></a>

### rtos_event_clear

```c
rtos_event_bits_t rtos_event_clear(rtos_event_t *event, rtos_event_bits_t bits);
```

- **参数**：event已初始化且有效；bits为清除掩码，应用用低24位。
- **上下文/阻塞**：启动、任务或合法ISR；有优先级断言，不等待。
- **返回**：完整清除后位值；NULL/未初始化返回0，0也可能是合法清空结果。
- **状态/限制**：执行bits &= ~mask，不唤醒任务。实现未屏蔽高8位，生命周期仍由外部协调。

<a id="api-rtos_event_wait"></a>

### rtos_event_wait

```c
rtos_event_bits_t rtos_event_wait(rtos_event_t *event, rtos_event_bits_t bits, uint32_t mode, rtos_tick_t timeout);
```

- **参数**：event已初始化且有效；bits非零，应用用低24位；mode为RTOS_EVENT_WAIT_ANY/RTOS_EVENT_WAIT_ALL，可OR RTOS_EVENT_CLEAR_ON_EXIT，其余模式位非法；timeout为tick。
- **上下文/阻塞**：任务；启动仅立即路径；合法ISR仅RTOS_NO_WAIT。需要阻塞时检查调度状态，但无独立状态输出。
- **返回**：满足时为清位前完整快照；RTOS_NO_WAIT未满足时为当时完整快照；超时为超时发生时的完整快照。NULL/未初始化、bits=0、非法mode、ISR非零等待、不可阻塞、deinit/挂起取消均返回0。
- **状态/输出**：满足且要求RTOS_EVENT_CLEAR_ON_EXIT时清整个等待掩码；RTOS_EVENT_WAIT_ANY检查任一位，RTOS_EVENT_WAIT_ALL检查全部位。等待参数与结果存TCB节点；超时在对象仍受保护时抓快照，阻塞返回不再读取event。
- **限制**：非零结果不等于满足，必须按原bits/mode判断；0不能区分合法零、错误、超时或取消。返回类型不是rtos_status_t。

<a id="api-rtos_event_get"></a>

### rtos_event_get

```c
rtos_event_bits_t rtos_event_get(const rtos_event_t *event);
```

- **参数**：event为NULL或仍有效的控制块。
- **上下文/阻塞**：无锁、无ISR/初始化检查、不阻塞，按公共观察查询约定使用。
- **返回/状态**：NULL为0，否则返回完整bits；deinit后可能保留旧位。无清位、无等待、不预留条件。
- **限制**：不能用单次get判断别的任务已经完成，不能当作对象有效性或可释放证明。

<a id="api-rtos_event_deinit"></a>

### rtos_event_deinit

```c
rtos_status_t rtos_event_deinit(rtos_event_t *event);
```

- **参数**：event曾成功初始化，结构完整、存储有效。
- **上下文/阻塞**：启动、任务或合法ISR；有优先级断言，不等待，可唤醒全部等待者。
- **返回**：NULL为RTOS_ERR_NULL；满足前置条件返回RTOS_OK。
- **状态**：以RTOS_ERR_DELETED中止等待，清初始化标志；等待者按当前接口得到0。保留bits，不释放控制块。
- **寿命/限制**：RTOS_OK不证明所有并发执行者已退出；垃圾对象和释放后指针不受保护，无通用代次撤销机制。

## 固定块池

来源：[rtos_mem.h](../../RTOS/Core/Inc/rtos_mem.h)、[rtos_mempool.c](../../RTOS/Memory/rtos_mempool.c)。

<a id="api-rtos_mempool_init"></a>

### rtos_mempool_init

```c
rtos_status_t rtos_mempool_init(rtos_mempool_t *pool, void *base, uint32_t block_size, uint32_t block_count);
```

- **参数**：pool可写；base为独立缓存起点，满足_Alignof(void*)，F446为4字节；block_size为单块字节数>0且<=UINT32_MAX-3；block_count为块数>0。实际缓存至少max(align4(block_size),sizeof(void*))*block_count字节。
- **上下文/阻塞**：启动、任务或合法ISR；有优先级断言，不等待，但在临界区遍历并写入所有块。
- **返回**：NULL、零参数、尺寸/乘积/地址范围溢出或起点未对齐为RTOS_ERR_PARAM；成功RTOS_OK。
- **状态**：块长上调至4字节对齐且至少容纳指针，建立空闲链；不清所有payload。随后尝试登记性能表，登记满仍返回RTOS_OK。
- **寿命/限制**：不验证实际容量或控制块/缓存重叠。活跃池重init无BUSY守卫，不安全。没有deinit/unregister；启用性能监视时，pool控制块须存活至相关登记已清除，例如受控重新初始化perf。不能在函数栈上临时创建后遗留登记。

<a id="api-rtos_mempool_alloc"></a>

### rtos_mempool_alloc

```c
void *rtos_mempool_alloc(rtos_mempool_t *pool);
```

- **参数**：pool已正确初始化，结构及缓存均有效。
- **上下文/阻塞**：启动、任务或合法ISR；有优先级断言，不等待，取空闲链首。
- **返回**：NULL pool或池空为NULL；成功返回一块起点，可用容量为pool->block_size。
- **状态/所有权**：摘链首、free_count减一；不清零payload，可能保留旧next指针字节。业务获得一块的归还责任。
- **限制**：无初始化标志或地址登记校验，随机非NULL对象可被解引用；不记录多个读者、引用或旧借用代次。

<a id="api-rtos_mempool_free"></a>

### rtos_mempool_free

```c
rtos_status_t rtos_mempool_free(rtos_mempool_t *pool, void *block);
```

- **参数**：pool为有效已初始化池；block为同池仍在借出的完整块起点，所有用户已停止访问。
- **上下文/阻塞**：启动、任务或合法ISR；范围检查后执行优先级断言，不等待，临界区内扫描空闲链。
- **返回**：任一NULL为RTOS_ERR_NULL；范围外、非块首、池尺寸/容量边界或空闲链状态非法为RTOS_ERR_PARAM；当前空闲链已有此块为RTOS_ERR_ALREADY；成功RTOS_OK。
- **状态/失败**：成功用该块首部写next并入链，free_count加一；之后不得再访问旧内容。普通拒绝不加入链。
- **限制**：释放最坏O(block_count)。地址已复用后不能区分旧指针与新借用；范围防护不能代替所有权协议。

<a id="api-rtos_mempool_get_free"></a>

### rtos_mempool_get_free

```c
uint32_t rtos_mempool_get_free(const rtos_mempool_t *pool);
```

- **参数**：pool为NULL或仍有效的控制块。
- **上下文/阻塞**：无锁、无ISR检查，不阻塞，按公共观察查询约定使用。
- **返回/状态**：NULL为0，否则读取free_count，不校验初始化，不预留块。
- **限制**：>0不保证之后alloc成功；不能证明控制块可释放，性能表仍可能保存pool地址。

## 通用堆

来源：[rtos_heap.h](../../RTOS/Core/Inc/rtos_heap.h)、[rtos_heap.c](../../RTOS/Memory/rtos_heap.c)。

<a id="api-rtos_heap_init"></a>

### rtos_heap_init

```c
rtos_status_t rtos_heap_init(void);
```

- **参数/上下文**：无参数，仅受控启动阶段独占初始化，通常由rtos_init编排。无并发初始化或ISR守卫，不在运行中拿它重置堆。
- **返回**：已初始化或首次构造成功为RTOS_OK；配置小于sizeof(header_t)+16字节时为RTOS_ERR_PARAM。
- **状态**：首次在内部静态RTOS_CONFIG_HEAP_SIZE缓存建立首块，清分配/释放累计计数；再次调用直接RTOS_OK，不清现存分配或累计值。
- **配置/限制**：独立于C库malloc；RTOS_CONFIG_USE_STATIC_ALLOCATION=1不禁用heap，也不把task_create改为静态分配；静态任务存储须显式使用task_create_static。无销毁堆/收回所有分配的公共操作。

<a id="api-rtos_heap_alloc"></a>

### rtos_heap_alloc

```c
void *rtos_heap_alloc(uint32_t size);
```

- **参数**：size为请求字节数>0；实现按8向上对齐，另加8字节header，最小总块16字节。
- **上下文/阻塞**：已init后的启动/任务或合法ISR；有优先级断言，不作调度等待，但在临界区扫描，推荐任务上下文。
- **返回**：未初始化、size=0、对齐及头部算术域超限、无足够连续块或扫描遇到已检测损坏时为NULL；成功为8字节对齐payload，容量至少size。
- **状态/所有权**：first-fit扫描物理块，可能拆分尾块、更新空闲账本及分配计数；不清payload。应用负责使用及一次最终归还。
- **限制**：扫描包含已占用与空闲块，最坏随物理块总数增长。不阻塞不等于固定周期；heap不记录还有哪些任务读取这块数据，不能自动判定安全释放时间。

<a id="api-rtos_heap_free"></a>

### rtos_heap_free

```c
void rtos_heap_free(void *ptr);
```

- **参数**：ptr为NULL，或本heap仍在占用的原始返回指针，全部用户已停止访问。不得传其他分配器地址、内部地址或过期别名。
- **上下文/阻塞**：NULL直接返回；其余为启动/任务或合法ISR，有优先级断言；不等待。
- **返回**：void；NULL、未初始化、范围外、魔数不符或当前已标空闲时静默返回，无错误码。
- **状态**：成功标空闲、记账、释放累计值加一；只尝试合并紧邻下一空闲块一次，清被合并header魔数，不清payload。
- **限制**：不查找前驱，不保证所有相邻空闲块合并。仅原始有效指针是支持的输入，魔数检查不是任意指针探测器。相同地址重新分配后，旧指针可能误释放新分配。

<a id="api-rtos_heap_get_free"></a>

### rtos_heap_get_free

```c
uint32_t rtos_heap_get_free(void);
```

- **参数/上下文**：无参数；无锁、无ISR检查、不阻塞，按公共观察查询约定使用。
- **返回**：当前空闲payload账目总和，不含现存块头；初始化前0。
- **状态/限制**：不预留内存；不是最大连续可申请块，并发会使快照立即过期。正常堆协议是统计有意义的前提。

<a id="api-rtos_heap_get_max_alloc"></a>

### rtos_heap_get_max_alloc

```c
uint32_t rtos_heap_get_max_alloc(void);
```

- **参数/上下文**：无参数；任务或合法ISR，有优先级断言。临界区扫描全部物理块，不等待，推荐非热路径任务调用。
- **返回**：未初始化为0；正常时为最大单块空闲payload容量，扣header并向下对齐至8字节。已检测损坏使扫描提前停止，可能返回已见最大值，不证明堆健康。
- **状态/限制**：不改块、不预留空间。查询与alloc是独立临界区，不能保证下一次申请成功；不能用空闲总和代替该上限。

<a id="api-rtos_heap_get_alloc_count"></a>

### rtos_heap_get_alloc_count

```c
uint32_t rtos_heap_get_alloc_count(void);
```

- **参数/上下文**：无参数；无锁、无ISR检查，不阻塞，按公共观察查询约定使用。
- **返回**：首次heap初始化以来成功分配次数，初始0；uint32_t自然回绕，非饱和计数。
- **状态/限制**：不修改状态，重复heap_init不清零。不是在用块数，不含其他分配器或失败申请，与另一查询不构成一致快照。

<a id="api-rtos_heap_get_free_count"></a>

### rtos_heap_get_free_count

```c
uint32_t rtos_heap_get_free_count(void);
```

- **参数/上下文**：无参数；无锁、无ISR检查，不阻塞，按公共观察查询约定使用。
- **返回**：成功释放次数，初始0；uint32_t自然回绕。被检查拒绝的free不增加。
- **状态/限制**：不修改状态，重复heap_init不清零。与alloc_count之差不能直接当作长期精确泄漏证明，需处理回绕及一致采样。

## 状态码及实现边界

函数返回值使用头文件中的完整状态名。只有函数实际返回`rtos_status_t`时才适用；
事件位、计数和指针返回不借用这套错误码。

- 直传仍拷贝数据；完整queue调用还包含校验、复制、等待链及调度成本。
- heap_free只合并下一块一次；本基线没有前向合并、完整相邻合并或自动碎片整理。
- 固定池空闲块首部按指针节点读写。本契约适用32位F446 ABI，不能仅凭存在C源码推断任意64位宿主、对齐或别名模型都已适配。
- 普通heap/mempool的范围及当前重复释放防护，不具备旧借用代次鉴别、多任务引用跟踪或任务删除自动释放。

共用路径：[等待与唤醒](../../RTOS/Core/Src/rtos_wait.c)、[阻塞允许条件](../../RTOS/Core/Private/rtos_internal.h)、
[任务登记及中止等待](../../RTOS/Core/Src/rtos_task.c)、[有限等待实现](../../RTOS/Core/Src/rtos_sched.c)、
[ISR断言配置](../../RTOS/Core/Inc/rtos_config.h)、[性能池登记](../../RTOS/Services/rtos_perf.c)。
