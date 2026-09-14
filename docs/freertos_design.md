# FreeRTOS 内核设计剖析与复杂问题排障大全

> **分析基准**：FreeRTOS Kernel V10.4.x（LTS）+ GCC/ARM_CM4F 移植（与本项目 STM32F446 同架构同编译器）
> **文档目标**：对照本工程 [rtos_design.md](rtos_design.md) 的自研内核，逐模块剖析 FreeRTOS 的真实实现——完整数据结构、逐步骤的代码走读、ASCII 图示、设计动机与代价；随后给出互斥锁、优先级反转、数据竞争、程序崩溃等复杂问题的系统化定位方法与解法
> **阅读方式**：第 1~12 章是设计剖析（先读第 2 章链表——它是后面一切的地基）；第 13~22 章是排障手册（可按症状直接索引）；第 23 章是排障工具箱
> **代码引用约定**：均给出真实函数名与文件出处（tasks.c / queue.c / list.c / timers.c / event_groups.c / heap_4.c / portable/GCC/ARM_CM4F/port.c）；片段有删节时标注 **[简化]**，字段顺序与源码一致

---

## 目录

- [第一章 设计哲学与整体架构](#第一章-设计哲学与整体架构)
- [第二章 list.c：整个内核的地基](#第二章-listc整个内核的地基)
- [第三章 TCB 与任务生命周期](#第三章-tcb-与任务生命周期)
- [第四章 调度器：决策、切换与空闲任务](#第四章-调度器决策切换与空闲任务)
- [第五章 延时链表与 tick：双链对滚](#第五章-延时链表与-tick双链对滚)
- [第六章 queue.c：一个文件统治所有 IPC](#第六章-queuec一个文件统治所有-ipc)
- [第七章 优先级继承的完整路径](#第七章-优先级继承的完整路径)
- [第八章 任务通知与事件组](#第八章-任务通知与事件组)
- [第九章 软件定时器：守护任务架构](#第九章-软件定时器守护任务架构)
- [第十章 内存管理：五种堆与 heap_4](#第十章-内存管理五种堆与-heap_4)
- [第十一章 Cortex-M 移植层与临界区](#第十一章-cortex-m-移植层与临界区)
- [第十二章 FreeRTOS 与自研内核逐项对照](#第十二章-freertos-与自研内核逐项对照)
- [第十三章 优先级反转、继承失效与死锁](#第十三章-优先级反转继承失效与死锁)
- [第十四章 数据竞争：临界区用错的形态](#第十四章-数据竞争临界区用错的形态)
- [第十五章 ISR 优先级域错误：随机崩溃之王](#第十五章-isr-优先级域错误随机崩溃之王)
- [第十六章 栈溢出：定位与防御](#第十六章-栈溢出定位与防御)
- [第十七章 程序崩溃：HardFault 分流表](#第十七章-程序崩溃hardfault-分流表)
- [第十八章 IPC 问题：丢失唤醒与虚假唤醒](#第十八章-ipc-问题丢失唤醒与虚假唤醒)
- [第十九章 STM32 平台专属问题](#第十九章-stm32-平台专属问题)
- [第二十章 newlib 与 printf 三连坑](#第二十章-newlib-与-printf-三连坑)
- [第二十一章 定时器与回调问题](#第二十一章-定时器与回调问题)
- [第二十二章 任务删除与悬空句柄](#第二十二章-任务删除与悬空句柄)
- [第二十三章 排障工具箱与配置基线](#第二十三章-排障工具箱与配置基线)
- [结语](#结语)

---

# 第一部分：设计剖析

# 第一章 设计哲学与整体架构

## 1.1 一句话定位

FreeRTOS 是一个**以任务为唯一中心、以侵入式链表为唯一黏合剂、以 queue.c 一个文件统一全部 IPC** 的微内核。它的全部复杂度集中在两处：链表操作的绝对正确、PendSV 切换的绝对可靠；其余一切都是这两个原语的应用题。

```
+------------------------------------------------------------+
| 用户任务         vTaskDelay / xQueueSend / xSemaphoreTake   |
+------------------------------------------------------------+
| API 层          tasks.c       —— 任务管理/调度/tick/空闲    |
|                 queue.c       —— 队列/二值/计数信号量/互斥锁|
|                 timers.c      —— 软件定时器(守护任务)       |
|                 event_groups.c—— 事件标志组                 |
+------------------------------------------------------------+
| 万物基础         list.c —— 4 个函数的侵入式双向环形链表      |
+------------------------------------------------------------+
| 移植层           portable/GCC/ARM_CM4F/port.c              |
|                 PendSV / SVC / SysTick / BASEPRI           |
|                 heap_4.c(可换 heap_1~3/5)                   |
+------------------------------------------------------------+
```



## 1.2 源码文件地图（V10.4，量级供心里有数）

| 文件 | 行数约（V10.4.6） | 职责 | 依赖 list.c 的程度 |
|---|---|---|---|
| list.c | ~230 | 链表 4 函数 | —（它自己） |
| tasks.c | ~5300 | TCB/就绪表/延时链/调度/tick/空闲/通知 | ★★★ 一切状态 |
| queue.c | ~2900 | 队列+信号量+互斥锁+队列锁定 | ★★ 等待链 |
| timers.c | ~1100 | 定时器守护任务+命令队列 | ★★ 活跃链 |
| event_groups.c | ~760 | 事件组 | ★ 等待链（无序） |
| stream_buffer.c / message_buffer.c | ~1700/~900 | 流/消息缓冲（本文未展开，底层复用任务通知索引 0） | ★ |
| croutine.c | ~380 | 协程（遗留，新项目不推荐） | ★ |
| port.c (CM4F) | ~900 | 上下文切换/临界区/SysTick/优先级自检 | 无（只碰 TCB 偏移 0） |

值得注意的**依赖单向性**：tasks.c 不 include queue.c，queue.c 反过来调 tasks.c 的内部函数（`vTaskPlaceOnEventList` 等）。分层是"tasks 在下、queue 在上"——这与自研内核"IPC 调 rtos_internal.h 的调度服务"（rtos_design.md rtos_internal.h）完全同构。

## 1.3 五条贯穿设计

**① 一切皆链表**。就绪、延时、延时溢出、挂起、等待终止、每对象的等待链、定时器活跃链、定时器溢出链——所有状态都是"把某个 ListItem 挂进某条 List"，状态迁移 = 摘一条链 + 挂另一条链。内存零额外分配（侵入式），但读代码必须追踪"这个 item 现在在哪条链上"。

**② 一个 queue.c 通吃四种 IPC**。队列、二值/计数信号量、互斥锁共用 xQUEUE 结构与收发路径，互斥锁只是"itemSize=0 且 take/give 附带优先级继承的特化队列"。一处优化全体受益，一处 bug 全体继承。

**③ tick 回绕结构性免疫**。双延时链对滚（第五章），49.7 天翻转零特殊处理——对照自研内核已知限制 L1，这是最值得借鉴的一处。

**④ 决策分散、执行唯一**。调度决策散布在 tasks.c 十几处（`prvAddTaskToReadyList` / `xTaskRemoveFromEventList` / `xTaskIncrementTick`……各自就地判断要不要 yield），切换执行统一收口在 PendSV——与自研内核"决策点唯一（rtos_sched_schedule）、执行点分离（挂起 PendSV）"正好相反。两种拓扑都能自洽：FreeRTOS 靠"yield 请求可自然合并"（PendSV 挂多次只切一次），自研靠"临界区内多次 schedule 合并"。

**⑤ 静态可裁剪**。configUSE_* 全条件编译，最小配置可压到 ~6KB Flash；代价是配置面庞大、宏组合的相互作用需要经验（第 23 章给出推荐基线）。

## 1.4 与自研内核的镜像总览

| 维度 | FreeRTOS | 自研内核（rtos_design.md） |
|---|---|---|
| 状态存放 | ListItem 挂入全局 List（链表即状态） | TCB->state 枚举 + 各链表 |
| 超时定位 | xEventListItem.pxContainer | wait_node.list_head |
| 同级轮转 | pxIndex 游标顺带完成 | ready_list_rotate 主动搬尾 |
| tick 回绕 | 双链对滚免疫 | 单链表（L1 债务） |
| IPC 组织 | queue.c 四合一 | sem/mutex/queue/event 四模块 |
| 删除持锁任务 | 互斥锁悬空（22 章） | mutex_release_all 自动移交 |
| ISR 域防御 | vPortValidateInterruptPriority | 无（靠 HardFault 事后诊断） |

镜像关系说明两者解的是同一组问题，解的空间不同——第十二章逐项展开。

---

# 第二章 list.c：整个内核的地基

## 2.1 结构定义（完整，一字未减）

```c
struct xLIST {                          /* List_t */
    volatile UBaseType_t uxNumberOfItems;  /* 链上节点数(不含哨兵) */
    ListItem_t * volatile pxIndex;         /* ★游标: 最近一次遍历返回的节点 */
    MiniListItem_t xListEnd;               /* 哨兵: xItemValue = portMAX_DELAY */
};

struct xMINI_LIST_ITEM {                /* MiniListItem_t —— 哨兵专用瘦身版 */
    TickType_t xItemValue;                 /* 恒为 portMAX_DELAY(升序插入天然沉底) */
    struct xLIST_ITEM * volatile pxNext;
    struct xLIST_ITEM * volatile pxPrevious;
};

struct xLIST_ITEM {                     /* ListItem_t */
    TickType_t xItemValue;                 /* ★排序键: 延时链=唤醒时刻; 队列等待链=反序优先级; 事件组=打包的等待条件 */
    struct xLIST_ITEM * volatile pxNext;
    struct xLIST_ITEM * volatile pxPrevious;
    void * pvOwner;                        /* 拥有者(通常是 TCB) */
    struct xLIST * volatile pxContainer;   /* ★本节点当前挂在哪条链上(NULL=自由) */
};
```

**xItemValue 的多路复用**（与自研 wait_node 字段复用 rtos_design.md 2.3 同一性质的紧凑设计）：

| 挂的链 | xItemValue 存什么 | 排序效果 |
|---|---|---|
| 就绪链 | （不关心） | vListInsertEnd 无序插入 |
| 延时链 | 唤醒的绝对 tick | 升序 → 链首最早醒 |
| 队列/信号量等待链 | configMAX_PRIORITIES - 优先级 | **反序技巧**：升序存储=按优先级降序 → 链首=最高优先级 |
| 事件组等待链 | 打包的等待条件（8 章） | 无序，靠扫描 |
| 挂起链/终止链 | （不关心） | 尾插 |

## 2.2 环形链 + 哨兵：一张图

```
          ┌──────────────────────────────────────────────┐
          │                                              │
          ▼                                              │
     ┌─────────┐    next    ┌──────┐    next    ┌──────┐  │
     │ xListEnd│ ─────────▶ │Item A│ ─────────▶ │Item B│──┘
     │(哨兵)   │ ◀───────── │      │ ◀───────── │      │
     └─────────┘    prev    └──────┘    prev    └──────┘
       ▲   │
       │   └─ List.pxIndex 初始指向哨兵
       └─ 永远在环上; xItemValue=MAX → 有序插入时沉底(天然链尾)
```



哨兵的三重价值：
1. **插入/删除无空链特判**——链表永远"非空"（至少有哨兵），边界代码消失；
2. **pxIndex 的锚点**——游标绕一圈回到哨兵时跳过它，遍历逻辑恒定；
3. **有序插入的终止条件**——`pxIterator->pxNext->xItemValue <= xValueOfInsertion` 走到哨兵必然停止（哨兵值=MAX 恒不小于任何插入值）。

**两个文档化的防御层**（常被忽略）：

- **完整性校验字节**：定义 `configUSE_LIST_DATA_INTEGRITY_CHECK_BYTES = 1` 后，list.c 的每个插入/删除动作都会用 `listTEST_LIST_INTEGRITY` 校验链表与节点的魔数——内存被踩（栈溢出/野指针写）时第一时间在链表操作处断言，而不是等到链表撕裂后的随机崩溃。这是 15/16 章两类"随机崩溃"的前置哨兵。
- **vListInsert 里的著名注释**：V10.4 源码在有序插入循环上方直接内嵌了一段"崩溃排查指南"，官方按命中率列出五大死因——**① 栈溢出；② Cortex-M 中断优先级配错（数值大=优先级低的反直觉陷阱）；③ 在临界区/调度器挂起期调用了非 FromISR API；④ 在初始化前或调度器启动前使用了队列/信号量（如启动前中断已开）；⑤ 支持中断嵌套的 port 中 tick 中断优先级未压到 syscall 域**。这份清单与本手册 15/16/17 章的分流表互为印证——FreeRTOS 作者把用户最常踩的坑直接写进了最可能崩溃的函数里。

## 2.3 uxListInsert：有序插入（核心代码）

```c
void uxListInsert( List_t * const pxList, ListItem_t * const pxNewListItem )
{
    ListItem_t * pxIterator;
    const TickType_t xValueOfInsertion = pxNewListItem->xItemValue;

    if( xValueOfInsertion == portMAX_DELAY )      /* MAX 是哨兵专属值: 直插哨兵前 */
    {
        pxIterator = pxList->xListEnd.pxPrevious;
    }
    else
    {
        for( pxIterator = ( ListItem_t * ) &( pxList->xListEnd );
             pxIterator->pxNext->xItemValue <= xValueOfInsertion;
             pxIterator = pxIterator->pxNext )
        { /* 空循环体: 纯走链 */ }
    }

    pxNewListItem->pxNext = pxIterator->pxNext;          /* 标准双向插入四步 */
    pxNewListItem->pxNext->pxPrevious = pxNewListItem;
    pxNewListItem->pxPrevious = pxIterator;
    pxIterator->pxNext = pxNewListItem;

    pxNewListItem->pxContainer = ( void * ) pxList;      /* ★自记所在链 */
    ( pxList->uxNumberOfItems )++;
}
```

走链条件是 `<=`——**同值节点排在已有同值之后**（FIFO）。这就是自研 delay_list_insert（rtos_design.md 3.4）的 FreeRTOS 原版：插入侧 O(n) 换 tick 侧 O(到期数)。

注意循环从哨兵出发而非真实首节点：`pxIterator->pxNext` 从链首开始参与比较，因此**可以插到首节点之前**——这是循环初始化的微妙之处，也是空链（只有哨兵）天然可用的原因。

## 2.4 vListInsertEnd：尾插（核心代码）

```c
void vListInsertEnd( List_t * const pxList, ListItem_t * const pxNewListItem )
{
    ListItem_t * const pxIndex = pxList->pxIndex;

    pxNewListItem->pxNext = pxIndex;
    pxNewListItem->pxPrevious = pxIndex->pxPrevious;
    pxIndex->pxPrevious->pxNext = pxNewListItem;
    pxIndex->pxPrevious = pxNewListItem;

    pxNewListItem->pxContainer = ( void * ) pxList;   /* 自记所在链 */
    ( pxList->uxNumberOfItems )++;
}
```

"插到 pxIndex 之前"= **逻辑队尾**。为什么？因为 pxIndex 指向"上次被遍历返回的节点"（2.6），它刚刚被消费过——新节点排在它后面（即它的 pxPrevious 侧），下轮遍历要转完一圈才轮到，等效 FIFO 尾插。就绪链用它。

## 2.5 uxListRemove：摘除（核心代码）

```c
UBaseType_t uxListRemove( ListItem_t * const pxItemToRemove )
{
    List_t * const pxList = pxItemToRemove->pxContainer;   /* ★顺 pxContainer 找到宿主链 */

    pxItemToRemove->pxNext->pxPrevious = pxItemToRemove->pxPrevious;   /* 双向缝合 */
    pxItemToRemove->pxPrevious->pxNext = pxItemToRemove->pxNext;

    if( pxList->pxIndex == pxItemToRemove )                /* ★游标修正: 若摘的正是游标节点 */
    {
        pxList->pxIndex = pxItemToRemove->pxPrevious;      /*   退到前驱, 遍历连续性保持 */
    }

    pxItemToRemove->pxContainer = NULL;                    /* 摘除即自由 */
    ( pxList->uxNumberOfItems )--;

    return pxList->uxNumberOfItems;
}
```

**pxIndex 修正**是容易被忽略的正确性细节：若正在被遍历的节点被删除，游标退到其前驱——下轮 GetOwnerOfNextEntry 从前驱的 next（即被删节点的后继）继续，既不跳过也不重复。这使"遍历中删除节点"安全（事件组的 set-bits 扫描正依赖此性质，8 章）。

`return 链剩余长度`——tasks.c 的 taskRESET_READY_PRIORITY 用返回值 0 判断"该优先级链已空，该清位图了"。

## 2.6 listGET_OWNER_OF_NEXT_ENTRY：游标遍历（宏，完整）

```c
#define listGET_OWNER_OF_NEXT_ENTRY( pxTCB, pxList )                           \
{                                                                              \
    List_t * const pxConstList = ( pxList );                                   \
    ( pxConstList )->pxIndex = ( pxConstList )->pxIndex->pxNext;  /* 游标前进 */\
    if( ( void * ) ( pxConstList )->pxIndex                                    \
        == ( void * ) &( ( pxConstList )->xListEnd ) )                         \
    {                                                                          \
        ( pxConstList )->pxIndex = ( pxConstList )->pxIndex->pxNext; /* 跳哨兵 */\
    }                                                                          \
    ( pxTCB ) = ( pxConstList )->pxIndex->pvOwner;                             \
}
```

调度器每次选任务（vTaskSwitchContext → taskSELECT_HIGHEST_PRIORITY_TASK）都对最高优先级就绪链调用它：

```
就绪链(优先级3): [哨兵] ↔ T2 ↔ T5 ↔ T8 ↔ (回哨兵)
                        pxIndex 初始 = 哨兵

第1次调用: pxIndex 走到 T2 → 返回 T2 → T2 运行     ← 时间片给了 T2
第2次调用: pxIndex 走到 T5 → 返回 T5 → 切换到 T5    ← 轮转! 无需专门代码
T5 被删:  pxIndex 修正(2.5) → 下次走到 T8, 不重不漏
```

**同级轮转免费融合进"选任务"这个必然动作**——对比自研 ready_list_rotate（tick 里主动搬队尾）：FreeRTOS 轮转发生在**每次切换**而非每 tick，且新就绪任务的尾插位置（pxIndex 之前）保证它要等一圈。

## 2.7 pxContainer 的双重语义（对应自研 list_head）

| 问题 | pxContainer 如何回答 |
|---|---|
| "在不在某条链上？" | `pxContainer != NULL` |
| "在哪条链上？" | 直接读出 List_t* |

超时路径（5.4）、任务删除（3.4）、事件组扫描——全部靠它。这是 FreeRTOS 版的自研 `wait_node.list_head`（rtos_design.md 2.3"超时清理的钥匙"），语义一字不差。

---

# 第三章 TCB 与任务生命周期

## 3.1 TCB 结构（V10.4，按分组节选）

```c
typedef struct tskTaskControlBlock {
    volatile StackType_t *pxTopOfStack;   /* ★偏移0: PendSV 汇编第一访问, 位置与汇编约定一致 */

    /* ---- 双 ListItem: 理解全内核的钥匙 ---- */
    ListItem_t xStateListItem;            /* 恰挂一条"状态链": 就绪/延时/延时溢出/挂起/终止 */
    ListItem_t xEventListItem;            /* 至多挂一条"事件链": 某队列/事件组的等待链 */

    UBaseType_t uxPriority;               /* 当前优先级(可能被继承临时抬高) */
    UBaseType_t uxBasePriority;           /* 基础优先级(归还的目标) */
    StackType_t *pxStack;                 /* 栈起始地址(删除时 vPortFree 用) */
    char pcTaskName[ configMAX_TASK_NAME_LEN ];

    /* ---- 优先级继承支持 ---- */
    UBaseType_t uxMutexesHeld;            /* ★持有互斥锁计数: 递归锁放行 + 继承有效性判定 */

    /* ---- 任务通知(轻量 IPC, 8章) ---- */
    volatile uint32_t ulNotifiedValue[ configTASK_NOTIFICATION_ARRAY_ENTRIES ];
    volatile uint8_t  ucNotifyState[ configTASK_NOTIFICATION_ARRAY_ENTRIES ];  /* NOT_WAITING/WAITING/RECEIVED */

    /* ---- 统计 ---- */
    configRUN_TIME_COUNTER_TYPE ulRunTimeCounter;
    UBaseType_t uxTCBNumber;
    ...
} tskTCB;
```

**双 ListItem = 自研"双链登记不变式"的 FreeRTOS 版**（rtos_design.md 3.5 表）：

| | 自研内核 | FreeRTOS |
|---|---|---|
| 状态侧 | state 枚举 + 挂就绪链/延时链 | xStateListItem 挂 5 种链之一 |
| 等待侧 | wait_node 挂对象等待链 | xEventListItem 挂对象等待链 |
| 带超时阻塞 | 同时挂两条 | 同时挂两条（State→延时链，Event→对象链） |

## 3.2 xTaskCreate 全流程（动态路径逐步骤）

```
1. pvPortMalloc( sizeof(TCB) + stack_size×4 )        ← heap_4(10章); 失败→vApplicationMallocFailedHook
2. pxPortInitialiseStack: 伪造异常返回帧(3.3)
3. prvInitialiseNewTask:
   a. 填 TCB: 优先级/名字拷贝/task_id 分配
   b. vListInitialiseItem(&xStateListItem); vListInitialiseItem(&xEventListItem)
   c. listSET_LIST_ITEM_OWNER(两 item, pxNewTCB)
   d. ★listSET_LIST_ITEM_VALUE(&xEventListItem,
        configMAX_PRIORITIES - uxPriority)            ← 优先级反序技巧(2.1表)
   e. 染料: 栈填充 0xA5A5A5A5(高水位检测用, 16章)
4. prvAddNewTaskToReadyList:
   taskENTER_CRITICAL();
   {
     uxCurrentNumberOfTasks++;
     if( pxCurrentTCB == NULL )  { pxCurrentTCB = 新TCB; }     ← 首个任务
     else if( 新优先级 > 当前优先级 && 调度器已运行 ) taskYIELD(); ← 创建即抢占
     prvAddTaskToReadyList( pxNewTCB ):
       taskRECORD_READY_PRIORITY(prio)               ← 置位 uxTopReadyPriority
       vListInsertEnd( &pxReadyTasksLists[prio], &xStateListItem )
   }
   taskEXIT_CRITICAL();
```

注意 **e 步的优先级反序**只在创建时写一次；后续优先级变化（继承/归还/`vTaskPrioritySet`）必须同步更新这个值，但**只更新值、不重插事件链**（重插的复杂度不可控，官方有意为之，详见 7.2 与社区案例 7.7）。

## 3.3 pxPortInitialiseStack：逐行（CM4F，与 V10.4.6 源码一致）

```c
StackType_t * pxPortInitialiseStack( StackType_t * pxTopOfStack,
                                     TaskFunction_t pxCode,
                                     void * pvParameters )
{
    /* 模拟"任务曾被中断"的栈帧, 异常返回即可'恢复'到任务入口 */
    pxTopOfStack--;
    *pxTopOfStack = portINITIAL_XPSR;            /* xPSR = 0x01000000: Thumb位=1, 必需 */
    pxTopOfStack--;
    *pxTopOfStack = ( ( StackType_t ) pxCode ) & portSTART_ADDRESS_MASK;  /* PC = 入口
                                                  (强制 bit0=0: PC 由异常硬件装载, 不允许 Thumb 位) */
    pxTopOfStack--;
    *pxTopOfStack = ( StackType_t ) portTASK_RETURN_ADDRESS;  /* LR = 出口陷阱(3.5),
                                                  默认 prvTaskExitError, 可用
                                                  configTASK_RETURN_ADDRESS 覆盖
                                                  (调试器栈回溯场景下自定义) */
    pxTopOfStack -= 5;                           /* R12, R3, R2, R1(占位) */
    *pxTopOfStack = ( StackType_t ) pvParameters;/* R0 = 参数(ARM ABI 入参) */
    /* 以上为异常硬件帧 8 字; CM4F 移植还要在软件帧里保存每任务自己的 EXC_RETURN: */
    pxTopOfStack--;
    *pxTopOfStack = portINITIAL_EXC_RETURN;      /* EXC_RETURN = 0xFFFFFFFD(Thread/PSP/基本帧) */
    pxTopOfStack -= 8;                           /* R11..R4(callee-saved 占位) */
    return pxTopOfStack;
}
```

栈从高地址向低地址构建；返回的栈顶交给 PendSV 恢复序列——首次"异常返回"后 CPU 精确落在任务入口、R0 装着参数、xPSR 的 Thumb 位已置（否则 INVSTATE 立即触发，17 章分流表第 2 行）。

**CM4F 移植的三个易漏细节**：① 软件帧里单独保存 `portINITIAL_EXC_RETURN`——因为 PendSV 把 r14 与 r4-r11 一并保存（4.4），每任务的帧类型（基本/含 FPU 扩展）由它自描述；② LR 填 `portTASK_RETURN_ADDRESS` 而默认是 `prvTaskExitError`——这是出口陷阱（3.5），也可被 `configTASK_RETURN_ADDRESS` 覆盖以改善调试器栈回溯；③ PC 经 `portSTART_ADDRESS_MASK` 强制清 bit0——该字将由硬件作为异常返回的 PC 装载，与函数指针的 Thumb 位约定相反。**自研内核同构**（rtos_design.md 4.3"伪造一次中断"）——ARMv7-M 启动任务的唯一正解。

## 3.4 vTaskDelete：普通删除与自删除的两条路径

```c
void vTaskDelete( TaskHandle_t xTaskToDelete )  [简化]
{
    taskENTER_CRITICAL();
    {
        pxTCB = prvGetTCBFromHandle( xTaskToDelete );   /* NULL → 当前任务 */

        /* ① 摘状态链(无论哪种状态) */
        if( listIS_CONTAINED_WITH_A_LIST( &pxTCB->xStateListItem ) )
            ( void ) uxListRemove( &pxTCB->xStateListItem );

        /* ② ★摘事件链: 若在等队列/信号量/事件组, 一并摘除(防悬空) */
        if( listIS_CONTAINED_WITH_A_LIST( &pxTCB->xEventListItem ) )
            ( void ) uxListRemove( &pxTCB->xEventListItem );

        if( pxTCB != pxCurrentTCB )              /* ③A 删别人: 立刻释放 */
        {
            --uxCurrentNumberOfTasks;
            prvDeleteTCB( pxTCB );               /* vPortFree(栈) + vPortFree(TCB) */
        }
        else                                      /* ③B 自删: 不能释放自己正跑着的栈! */
        {
            vListInsertEnd( &xTasksWaitingTermination, &pxTCB->xStateListItem );
            ++uxDeletedTasksWaitingCleanUp;
            taskYIELD_WITHIN_API? → 切走, 永不回来
        }
    }
    taskEXIT_CRITICAL();
}
```

自删 → 挂 `xTasksWaitingTermination` → **空闲任务回收内存**（4.7）——用 idle 的时序而非引用计数解决 use-after-free。**与自研 defer_free_tcb 方案完全同构**（rtos_design.md 5.2"自删任务延迟回收"），两个独立实现又收敛了。

**两个已知缺口**（22 章详述）：
- ②步只摘链，**不释放它持有的互斥锁**——`pxMutexHolder` 仍指向已 free 的 TCB，下一个 take 的继承路径解引用野指针；
- 句柄悬空问题——FreeRTOS 无任何机制阻止你删除后继续使用句柄。

## 3.5 任务函数 return 了会怎样：prvTaskExitError

任务入口不是无限循环、执行完 return——返回地址是初始化时填的 `prvTaskExitError`（V10.4.6 真实代码）：

```c
static void prvTaskExitError( void )
{
    volatile uint32_t ulDummy = 0;

    /* 任务函数不允许返回——没有可返回的去处。若任务想退出,
     * 应当调用 vTaskDelete( NULL )。
     * 下面这行断言是"人工强制失败"的绊线: uxCriticalNesting 的值域
     * 是 {0xAAAAAAAA(调度器启动前哨兵), 0..N(运行中嵌套计数)},
     * 永远不可能等于 ~0UL —— 到达这里必触发 configASSERT。 */
    configASSERT( uxCriticalNesting == ~0UL );
    portDISABLE_INTERRUPTS();
    while( ulDummy == 0 )
    {
        /* 挂死(可断点捕获); ulDummy 只为消"不可达代码"编译警告 */
    }
}
```

**症状**：任务"消失"但系统不崩、configASSERT 命中这里（uxCriticalNesting == ~0UL 恒假）、栈帧 PC 显示 prvTaskExitError。**修复**：任务入口必须是 `for(;;)` 或 `while(1)`；需要退出就 `vTaskDelete(NULL)`——return 出任务在 FreeRTOS 是未定义路径。生态位的另一解：ESP-IDF 在移植层加了任务函数包装器（`CONFIG_FREERTOS_TASK_FUNCTION_WRAPPER`），任务 return 会打印"FreeRTOS task should not return. Aborting now!"并 abort——把同一陷阱从"静默挂死"升级为"响亮失败"。

## 3.6 vTaskSuspend / vTaskResume

```
vTaskSuspend( xTask ):
  摘状态链 + 摘事件链(若在等) → 挂 xSuspendedTaskList
  ★摘事件链但不清 xEventListItem 的 owner——恢复后重新等
vTaskResume( xTask ):
  摘挂起链 → prvAddTaskToReadyList(就地判断 yield)
vTaskResumeFromISR: 若调度器挂起 → 挂 xPendingReadyList(见 4.8)
```

挂起一个**正持有互斥锁或正持锁等锁**的任务是经典自锁源（13 章查点 2）。

---

# 第四章 调度器：决策、切换与空闲任务

## 4.1 就绪表：一个变量 + 一组链

```c
PRIVILEGED_DATA static List_t pxReadyTasksLists[ configMAX_PRIORITIES ];
PRIVILEGED_DATA static volatile UBaseType_t uxTopReadyPriority = 0;
```

约定：**数值大 = 优先级高**（与自研相反！）；idle = 0 = 最低。

**uxTopReadyPriority 是个"双面人"——它的身份随 `configUSE_PORT_OPTIMISED_TASK_SELECTION` 切换**：

- **通用 C 版**（=0）：它是**标量**，只记"当前最高就绪优先级"这一个数。清位时机用 uxListRemove 的返回值（链空则降标）。查找时从标量逐级下探找非空链——O(优先级数) 最坏。
- **CLZ 优化版**（=1，CM4F 默认开）：它是**位图**（每 bit 代表一个优先级是否有就绪任务），`taskRECORD_READY_PRIORITY` 用 `portRECORD_READY_PRIORITY` 置位（`|= 1<<prio`）、`taskRESET_READY_PRIORITY` 清位——与自研 ready_bitmap 完全同构，查找用 `31 - __CLZ()` 一条指令。

所以"比自研 32 位全量位图省内存"的说法只在通用版成立；CLZ 版两者数据结构等价，差异仅在优先级方向约定（FreeRTOS 高值=高优先级配 CLZ 找最高置位；自研 0=最高配 CTZ 找最低置位，指令效率相同）。位宽即优先级上限（32 位整型 ≤ 32 优先级；自研显式 32 桶同限）。

## 4.2 两种任务选择：通用循环 vs CLZ 优化

```c
#if ( configUSE_PORT_OPTIMISED_TASK_SELECTION == 1 )     /* CM4 开此优化 */
  #define taskSELECT_HIGHEST_PRIORITY_TASK( pxTopTCB )                    \
  {                                                                       \
      UBaseType_t uxTopPriority;                                          \
      portGET_HIGHEST_PRIORITY( uxTopPriority, uxTopReadyPriority );      \
      /* ↑ uxTopPriority = 31 - __CLZ( uxTopReadyPriority )  1条指令 */   \
      configASSERT( 链非空 );                                              \
      listGET_OWNER_OF_NEXT_ENTRY( *( pxTopTCB ), &pxReadyTasksLists[uxTopPriority] );\
  }                                                                       \
  #define taskRESET_READY_PRIORITY( uxPriority )                          \
  {  if( listCURRENT_LIST_LENGTH( &pxReadyTasksLists[uxPriority] ) == 0 ) \
         portRESET_READY_PRIORITY( uxPriority, uxTopReadyPriority ); }     \
      /* ↑ 清位图位: uxTopReadyPriority &= ~(1UL << uxPriority) */

#else /* 通用版: uxTopReadyPriority 是"搜索上界", 逐级下探找非空链 */
  #define taskSELECT_HIGHEST_PRIORITY_TASK( pxTopTCB )                    \
  {                                                                       \
      UBaseType_t uxTopPriority = uxTopReadyPriority;                     \
      while( listCURRENT_LIST_LENGTH( &pxReadyTasksLists[ uxTopPriority ] ) == 0 )\
      { configASSERT( uxTopPriority > 0 ); --uxTopPriority; }             \
      ... GetOwnerOfNextEntry ...                                          \
      uxTopReadyPriority = uxTopPriority;                                  \
  }
#endif
```

CLZ 版：位图（无符号整型按优先级置位）+ `31-__CLZ` 一条指令找最高置位——**与自研 ctz 版互为镜像**（自研 0=最高配 ctz 找最低位，FreeRTOS 高值=高优先级配 CLZ 找最高位，指令效率相同）。注意位宽即优先级上限（32 位整型 ≤ 32 优先级；自研显式 32 桶同限）。

## 4.3 vTaskSwitchContext：切换的 C 侧

```c
void vTaskSwitchContext( void )  [简化]
{
    if( uxSchedulerSuspended != 0 )          /* 挂起期: 只记账不切换 */
    {
        xYieldPending = pdTRUE;
        return;
    }
    traceTASK_SWITCHED_OUT();                /* Tracealyzer 钩子 */
    taskCHECK_FOR_STACK_OVERFLOW( pxCurrentTCB );  /* ★借切换点查栈(16章) */
    taskSELECT_HIGHEST_PRIORITY_TASK();      /* 4.2 的宏: 含游标轮转 */
    traceTASK_SWITCHED_IN();
}
```

**在必然路径上顺路做事**：栈检查（16 章）与 trace 钩子都白嫖"切换必然发生"这个调度不变量。自研内核在同一点挂 perf 记账（rtos_design.md 4.2"每秒白嫖 1000 次"）——同一设计品味。

## 4.4 PendSV 汇编：逐行（CM4F port.c，与 V10.4.6 源码一致）

```asm
xPortPendSVHandler:                      /* naked, 优先级=configKERNEL_INTERRUPT_PRIORITY(典型0xFF最低) */
    mrs   r0, psp                        /* r0 = 被打断任务硬件帧底(PSP) */
    isb
    ldr   r3, =pxCurrentTCB              /* r3 = &pxCurrentTCB */
    ldr   r2, [r3]                       /* r2 = 当前任务 TCB */
    tst   r14, #0x10                     /* EXC_RETURN.bit4: 0=该任务有FPU上下文 */
    it    eq
    vstmdbeq r0!, {s16-s31}              /* FPU任务: 补存s16-s31(惰性扩展帧之外的高位) */
    stmdb r0!, {r4-r11, r14}             /* ★一条指令存 callee-saved + EXC_RETURN(帧自描述) */
    str   r0, [r2]                       /* ★TCB->pxTopOfStack = 新栈顶(偏移0的约定) */
    /* ---- 调 C 决策: 用 BASEPRI 关 syscall 域中断保护决策窗口 ---- */
    stmdb sp!, {r0, r3}                  /* r0/r3 是 bl 的易损寄存器, 暂存到 MSP */
    mov   r0, #configMAX_SYSCALL_INTERRUPT_PRIORITY
    msr   basepri, r0
    dsb
    isb
    bl    vTaskSwitchContext             /* C侧决策(跑在MSP): 选任务+轮转+栈溢出检查 */
    mov   r0, #0
    msr   basepri, r0                    /* 开闸 */
    ldmia sp!, {r0, r3}                  /* 取回 r0/r3 */
    /* ---- 恢复新任务 ---- */
    ldr   r1, [r3]                       /* pxCurrentTCB 已被决策更新 */
    ldr   r0, [r1]                       /* r0 = 新任务 pxTopOfStack */
    ldmia r0!, {r4-r11, r14}             /* 恢复 callee-saved + 该任务的 EXC_RETURN */
    tst   r14, #0x10                     /* 新任务的帧类型: 是否有 FPU 上下文 */
    it    eq
    vldmiaeq r0!, {s16-s31}
    msr   psp, r0
    isb
    bx    r14                            /* 异常返回: 硬件弹8字基本帧, 新任务原地继续 */
```



四条设计铁律，自研 port 逐条同构（rtos_design.md 4.2）：

1. **PendSV 最低优先级**（由 `configKERNEL_INTERRUPT_PRIORITY` 设置，典型 0xFF）——任何 ISR 都能打断它 → 切换代码天然独占内核态，C 辅助函数不需要再加锁；
2. **EXC_RETURN 随栈保存**——`stmdb r0!, {r4-r11, r14}` 一条指令同时完成 callee-saved 保存与帧自描述；CM0 无 FPU 的移植才可以省掉它（EXC_RETURN 恒定），这是 FreeRTOS 论坛里官方确认过的优化空间；
3. **两级现场**——硬件存 8 字基本帧（+惰性 FPU 扩展），PendSV 只补 callee-saved——上下文切换的 O(callee) 而非 O(全部寄存器)；
4. **PSP 隔离 + MSP 决策**——任务跑 PSP，异常处理跑 MSP；`bl vTaskSwitchContext` 期间用 `stmdb sp!, {r0, r3}` 把易损寄存器暂存到 MSP，并用 BASEPRI 把决策窗口罩进临界区——**这是与自研内核"schedule 由调用方在临界区内调用"的拓扑差异**：FreeRTOS 把临界区收进 PendSV 内部，自研把它放在决策调用者一侧。

## 4.5 SVC 启动第一个任务（逐行，与 V10.4.6 源码一致）

```asm
vPortSVCHandler:                    /* 由 prvPortStartFirstTask 的 "svc 0" 触发 */
    ldr   r3, =pxCurrentTCB         /* 取选好的第一个任务 */
    ldr   r1, [r3]
    ldr   r0, [r1]                  /* r0 = 其初始化栈顶(3.3 伪造的帧) */
    ldmia r0!, {r4-r11, r14}        /* 恢复 callee-saved + EXC_RETURN(0xFFFFFFFD) */
    msr   psp, r0                   /* PSP = 任务栈 */
    isb
    mov   r0, #0
    msr   basepri, r0               /* 确保BASEPRI干净 */
    bx    r14                       /* ★异常返回进第一个任务——它从未运行过,
                                       但硬件帧让它以为刚从中断返回 */

prvPortStartFirstTask:
    ldr r0, =0xE000ED08             /* VTOR */
    ldr r0, [r0]                    /* 向量表基址 */
    ldr r0, [r0]                    /* 表首项 = 复位时的 MSP */
    msr msp, r0                     /* MSP 回到上电值(抛弃main的调用栈) */
    mov r0, #0
    msr control, r0                 /* ★清 CONTROL.FPCA: 消除调度器启动前
                                       可能残留的"FPU在用"标记, 避免首任务
                                       的SVC栈帧被无谓预留扩展帧空间 */
    cpsie i                         /* 开中断 */
    cpsie f
    dsb
    isb
    svc 0                           /* 触发SVC → 上面的恢复序列 */
```

两个细节：**EXC_RETURN 不是硬编码立即数，而是从任务栈里恢复的**——`ldmia r0!, {r4-r11, r14}` 把初始化时写入的 `portINITIAL_EXC_RETURN`（0xFFFFFFFD）装进 r14 再 `bx r14`，与 PendSV 的恢复路径同构；首个任务若后续用了 FPU，其扩展帧由惰性入栈在首次中断时自然建立。另外 `msr control, r0` 清 FPCA 位是 CM4F 特有的——启动前若 main 里用过 FPU，残留标记会让 SVC 栈帧白留 26 字。**自研同构**（rtos_design.md 4.4"SVC 的必要性"：MSP 已被 main 弄脏、PSP 尚无合法值，唯有异常返回能原子进入"用 PSP 跑 Thread 模式"）。

## 4.6 taskYIELD 家族与切换请求合并

| 宏 | 场景 | 动作 |
|---|---|---|
| taskYIELD() | 任务上下文 | 挂起 PendSV（ICSR bit28） |
| portYIELD_FROM_ISR(x) | ISR 尾部 | `if(x) 挂 PendSV` |
| portYIELD_WITHIN_API() | 内核 API 内部 | 同 taskYIELD |
| taskYIELD_IF_USING_PREEMPTION() | 内核内部条件 yield | 挂起式 |

**合并语义**：PendSV 挂多次只执行一次；一个 tick 里唤醒 10 个任务的 10 个 yield 请求合并成一次切换。决策方各异、执行方唯一——自研用"临界区内 schedule 多次 pend 自然合并"达成同效（rtos_design.md 3.3 好处 1）。

## 4.7 空闲任务 prvIdleTask：三件事

```c
static portTASK_FUNCTION( prvIdleTask, pvParameters )  [简化]
{
    for( ;; )
    {
        /* ① 回收自删任务的内存(3.4 ③B 的下半场), 由 prvCheckTasksWaitingTermination 完成 */
        while( uxDeletedTasksWaitingCleanUp > 0 ) { ... 摘 xTasksWaitingTermination → prvDeleteTCB ... }

        /* ② 同级让位 */
        #if ( configUSE_PREEMPTION == 0 )
            taskYIELD();                        /* 非抢占配置: 每轮循环都让位 */
        #elif ( configIDLE_SHOULD_YIELD == 1 )
            if( listCURRENT_LIST_LENGTH( &pxReadyTasksLists[ 0 ] ) > 1 )
                taskYIELD();                    /* 抢占配置: 仅当优先级0上还有别人 */
        #endif

        /* ③ 低功耗: tickless 空闲 */
        #if ( configUSE_TICKLESS_IDLE != 0 )
            xExpectedIdleTime = prvGetExpectedIdleTime();   /* 预估可睡时长 */
            if( xExpectedIdleTime >= configEXPECTED_IDLE_TIME_BEFORE_SLEEP )
            {
                vTaskSuspendAll();              /* 挂起后再精确重估一次 */
                xExpectedIdleTime = prvGetExpectedIdleTime();
                if( xExpectedIdleTime >= configEXPECTED_IDLE_TIME_BEFORE_SLEEP )
                    portSUPPRESS_TICKS_AND_SLEEP( xExpectedIdleTime );  /* 停tick睡眠+补偿 */
                ( void ) xTaskResumeAll();
            }
        #endif
    }
}
```

`configIDLE_SHOULD_YIELD=1` 是关键配置：否则同在优先级 0 的用户任务可能长期饿死（空闲任务不主动让位时靠时间片轮转兜底，轮转关了就饿死）；非抢占配置下 idle 每轮循环都让位。tickless 的真实路径是 **`prvGetExpectedIdleTime` 预估 → 挂起调度器复核 → `portSUPPRESS_TICKS_AND_SLEEP` 停 tick 睡眠并在唤醒后补偿丢失的 tick**——整个逻辑内嵌在 idle 任务里，不需要也不存在"vApplicationSleep"这个钩子（低功耗的进入/退出动作应由用户在 portSUPPRESS_TICKS_AND_SLEEP 的实现里完成）。**自研 idle 同构**（回收+让位，rtos_design.md 5.2），差异是自研默认 WFI（rtos_sched.c 的 idle_hook 弱定义）。

## 4.8 vTaskSuspendAll / xTaskResumeAll：调度器挂起的完整账本

```c
void vTaskSuspendAll( void )
{
    ++uxSchedulerSuspended;             /* 无临界区: 只有一个写者(当前任务) */
}

BaseType_t xTaskResumeAll( void )  [简化]
{
    taskENTER_CRITICAL();
    {
        --uxSchedulerSuspended;
        if( uxSchedulerSuspended == 0 )
        {
            /* ① 结转挂起期间"就绪了却加不进就绪表"的任务 */
            while( listCURRENT_LIST_LENGTH( &xPendingReadyList ) > 0 )
            {
                pxTCB = listGET_OWNER_OF_HEAD_ENTRY( &xPendingReadyList );
                ( void ) uxListRemove( &pxTCB->xEventListItem );   /* 借用event item挂此链 */
                ... 摘其状态链 ...
                prvAddTaskToReadyList( pxTCB );
            }
            /* ② 结转挂起期间积压的 tick */
            while( xPendedTicks > 0 )
            {
                if( xTaskIncrementTick() != pdFALSE ) xYieldPending = pdTRUE;
                --xPendedTicks;
            }
            /* ③ 有欠账的切换请求 → 现在切 */
            if( xYieldPending ) taskYIELD_IF_USING_PREEMPTION();
        }
    }
    taskEXIT_CRITICAL();
    ...
}
```

**挂起期间的三类延迟记账**（FreeRTOS 挂起机制的全部难点）：

| 挂起期间发生的事 | 记在哪 | 恢复时怎么还 |
|---|---|---|
| tick 中断 | xPendedTicks 计数 | 逐个补跑 xTaskIncrementTick |
| 队列收发唤醒 | cRxLock/cTxLock 锁定计数 | prvUnlockQueue 补发唤醒（6.7） |
| 事件组/notify/ResumeFromISR 的直接唤醒 | xPendingReadyList（借 event item 串挂） | ① 步批量移入就绪表 |

自研内核没有公开的挂起 API——**能不挂就不挂**是两边共同的纪律（挂起期间所有时序假设全部失效，13/18 章的坑多源于用户滥用此 API）。

---

# 第五章 延时链表与 tick：双链对滚

## 5.1 为什么需要两条延时链

单链表 + 无符号 tick 的死穴：`wake_tick = now + timeout` 在 `now + timeout` 越过 0xFFFFFFFF 时回绕成小值——有序插入会把"最晚醒"插到链首（tick=1100 醒的任务排在 tick=100 醒的任务前面），唤醒顺序全乱。自研内核正卡在这里（L1，修复方向是 signed 差值比较）。

FreeRTOS 的解法不是修比较，而是**让回绕任务根本不进本周期链**：

```c
PRIVILEGED_DATA static List_t xDelayedTaskList1;
PRIVILEGED_DATA static List_t xDelayedTaskList2;
PRIVILEGED_DATA static List_t * volatile pxDelayedTaskList;          /* 本周期链 */
PRIVILEGED_DATA static List_t * volatile pxOverflowDelayedTaskList;  /* 跨翻转链 */

#define taskSWITCH_DELAYED_LISTS()                                   \
{                                                                    \
    List_t * pxTemp;                                                 \
    pxTemp = pxDelayedTaskList;                                      \
    pxDelayedTaskList = pxOverflowDelayedTaskList;                   \
    pxOverflowDelayedTaskList = pxTemp;                              \
    xNumOfOverflows++;  /* 可选统计 */                               \
    prvResetNextTaskUnblockTime();                                   \
}
```

## 5.2 入链决策：prvAddCurrentTaskToDelayList

```c
static void prvAddCurrentTaskToDelayList( TickType_t xTicksToWait )  [简化]
{
    TickType_t xTimeToWake;
    const TickType_t xConstTickCount = xTickCount;

    /* 当前任务先从就绪表摘除(由调用方做) */
    listSET_LIST_ITEM_VALUE( &pxCurrentTCB->xStateListItem, xConstTickCount + xTicksToWait );
    /* ↑ xItemValue 存的是"唤醒的绝对 tick" */

    xTimeToWake = xConstTickCount + xTicksToWait;
    if( xTimeToWake < xConstTickCount )            /* ★无符号加法回绕检测: 和比加数还小 */
    {
        /* 睡过了 tick 上限 → 跨翻转, 挂溢出链 */
        vListInsert( pxOverflowDelayedTaskList, &pxCurrentTCB->xStateListItem );
    }
    else
    {
        vListInsert( pxDelayedTaskList, &pxCurrentTCB->xStateListItem );
        /* 顺带维护 uxTaskUnblockTime(下次到期时刻的缓存, 微优化) */
        ...
    }
}
```

**不变式**：本周期链上所有 wake ≤ 0xFFFFFFFF 本周期会到期；溢出链上所有 wake 已回绕成"下周期的小值"。两条链各自内部升序——**每条链内部永远无需处理回绕**。

## 5.3 xTaskIncrementTick 完整流程（逐段注解）

```c
BaseType_t xTaskIncrementTick( void )  [核心结构完整]
{
    TCB_t * pxTCB;
    TickType_t xItemValue;
    BaseType_t xSwitchRequired = pdFALSE;

    /* 段1: 挂起期记账, 不处理 */
    if( uxSchedulerSuspended == pdTRUE ) { ++xPendedTicks; return pdFALSE; }

    /* 段2: 计数 + 对滚 */
    xConstTickCount = xTickCount + 1;
    xTickCount = xConstTickCount;
    if( xConstTickCount == ( TickType_t ) 0U )          /* 0xFFFFFFFF→0: 翻转! */
    {
        taskSWITCH_DELAYED_LISTS();                     /* 两条链指针互换, 一条C语句 */
    }

    /* 段3: 唤醒到期任务(只看本周期链链首) */
    /* 先用 xNextTaskUnblockTime 早退: 连"摘链首"的判断都省了 */
    if( xConstTickCount >= xNextTaskUnblockTime )
    {
        for( ;; )
        {
            if( listLIST_IS_EMPTY( pxDelayedTaskList ) ) { xNextTaskUnblockTime = portMAX_DELAY; break; }
            pxTCB = listGET_OWNER_OF_HEAD_ENTRY( pxDelayedTaskList );
            xItemValue = listGET_LIST_ITEM_VALUE( &( pxTCB->xStateListItem ) );
            if( xConstTickCount < xItemValue ) { xNextTaskUnblockTime = xItemValue; break; }  /* 缓存下次到期 */

            /* 摘状态链(延时链) */
            ( void ) uxListRemove( &( pxTCB->xStateListItem ) );

            /* ★超时定位: 还挂在事件链上? → 这次唤醒是"超时"而非"对象唤醒" */
            if( listLIST_ITEM_CONTAINER( &( pxTCB->xEventListItem ) ) != NULL )
            {
                ( void ) uxListRemove( &( pxTCB->xEventListItem ) );  /* 摘! 防悬空 */
            }
            /* pxTCB 因超时离开互斥锁等待 → 继承重新评估(7.4) */

            prvAddTaskToReadyList( pxTCB );

            /* 抢占/轮转判断: 更高优先级就绪 → 请求切换 */
            if( pxTCB->uxPriority > pxCurrentTCB->uxPriority )
                xSwitchRequired = pdTRUE;
            /* (同级 + 时间片开启时同样请求, 细节从简) */
        }
    }

    /* 段4: 时间片轮转(抢占开启 && 同级就绪数>1 && 轮转开启 → 请求切换) */
    #if ( ( configUSE_PREEMPTION == 1 ) && ( configUSE_TIME_SLICING == 1 ) )
        ... if( 同优先级就绪数 > 1 ) xSwitchRequired = pdTRUE; ...
    #endif

    /* 段5: tick 钩子 */
    #if ( configUSE_TICK_HOOK == 1 )  vApplicationTickHook(); ...

    return xSwitchRequired;   /* SysTick包装层据此决定 portYIELD_FROM_ISR */
}
```

段 3 是**自研 3.7 阶段三的 FreeRTOS 原版**：升序延时链保证到期者集中在链首、摘链首即定位、经 pxContainer 摘事件链防悬空。差异在"判决传达"：

| | 自研内核 | FreeRTOS |
|---|---|---|
| 超时判决 | wait_result=ERR_TIMEOUT 写入 TCB（信箱模式，醒来即读） | **无标志位**——任务醒来后重查队列状态 + xTaskCheckForTimeOut 判定（重查模式） |

重查模式的一个微妙后果：**"被 give 唤醒但醒来前数据又被别人抢走"与"超时"在醒来视角不可区分**——都表现为"队列还是空"。FreeRTOS 用重试循环吞掉这个歧义（6.5），语义上"醒来≠成功"，成功只有重查成功。自研的 wake_highest 直接传递所有权，"唤醒即成功"语义更强——各有代价。

## 5.4 回绕翻转瞬间的图解（tick 用 3 位示意，即从 7 翻到 0）

```
tick=6 时:  本周期链(升序):  [A(wake=7)] [B(wake=7)]
            溢出链(升序):    [D(wake=1')] [E(wake=3')]   ← ' 表示下周期值(入链时已回绕)

tick 6→7:   段3 先跑: A/B 到期被摘 → 本周期链清空
tick 7→0:   段2 翻转检测命中 → taskSWITCH_DELAYED_LISTS() 交换指针:
            本周期链(新) = 旧溢出链:  [D(wake=1)] [E(wake=3)]   ← 新周期里它们最早到期
            溢出链(新)   = 旧本周期链(空):  []
```



三个关键问题的正面回答：

- **翻转瞬间本周期链上还有任务会被落下吗？** 不会——**翻转瞬间本周期链必然已空**。凡 wake 落在本周期的任务，其到期时刻 ≤ 0xFFFFFFFF（本周期最大 tick），在计数推进到翻转点的过程中已被逐拍摘除；而 wake 跨过翻转点的任务在插入瞬间就被判进溢出链（5.2 的 `xTimeToWake < xConstTickCount` 无符号和小于加数 ⟺ 加法越界）。两个方向的归属都在插入瞬间判定，翻转时刻不需要任何"收尾清理"。
- **"周期归属"的判定可靠吗？** 可靠——无符号加法的回绕检测（`xTimeToWake < xConstTickCount`）是精确的：和比加数还小，当且仅当加法越过了类型上限。
- **两条链会不会同时为空导致唤醒停摆？** idle 任务恒在就绪表兜底；全空时 tick 处理只做计数自增（段 3 的 `xNextTaskUnblockTime` 早退）。

结论：**wake 的周期归属在插入瞬间判定，翻转只是给两条链换个身份**——不变式在翻转前后都成立。这就是"49.7 天免疫"的全部构造。

## 5.5 xTaskDelay 与 vTaskDelayUntil

```
vTaskDelay(n):  prvAddCurrentTaskToDelayList( n ); 从就绪表摘; yield
vTaskDelayUntil(&prev, inc):
    xTimeToWake = prev + inc;
    if( now - prev < inc )  /* 周期未满: 睡 (xTimeToWake - now) */
    else 已过点: 不睡(或睡1tick), 防累积漂移
    prev = xTimeToWake;
```

周期任务（控制环/采样）用 DelayUntil 防漂移——用 tick 差值比较（无符号）判断"是否已过点"，与自研 delay_until（rtos_design.md 5.6）同构。

---

# 第六章 queue.c：一个文件统治所有 IPC

## 6.1 xQUEUE 结构（完整）

```c
typedef struct QueueDefinition
{
    int8_t * pcHead;                     /* 存储区起始(信号量/互斥锁: NULL) */
    int8_t * pcTail;                     /* 存储区结束 */
    int8_t * pcWriteTo;                  /* 下一个写入位置(环形) */
    union                                 /* 读指针: 非互斥锁=pcReadFrom; 互斥锁=信号量数据 */
    {
        QueuePointers xQueue;            /*   {pcReadFrom} */
        SemaphoreData_t xSemaphore;      /*   {pxMutexHolder} */
    } u;
    List_t xTasksWaitingToSend;          /* 队满时的发送者等待链(优先级降序) */
    List_t xTasksWaitingToReceive;       /* 队空时的接收者等待链(优先级降序) */
    volatile UBaseType_t uxMessagesWaiting;  /* 队内元素数(信号量: 计数) */
    UBaseType_t uxLength;                /* 容量(信号量: max) */
    UBaseType_t uxItemSize;              /* 元素字节数(信号量/互斥锁: 0) */
    volatile int8_t cRxLock;             /* ★接收锁定计数(6.7) */
    volatile int8_t cTxLock;             /* ★发送锁定计数 */
    uint8_t ucQueueType;                 /* 队列/二值/计数/互斥/递归互斥(创建时烙印) */
} Queue_t;
```

## 6.2 四种对象，一种结构

| 对象 | uxLength | uxItemSize | 数据区 | 独有行为 |
|---|---|---|---|---|
| 队列 | N | itemSize | 有 | prvCopyDataToQueue/FromQueue |
| 二值信号量 | 1 | 0 | 无 | give 计数封顶 1 |
| 计数信号量 | max | 0 | 无 | — |
| 互斥锁 | 1 | 0 | 无 | **take 触发继承、give 触发归还；ISR 禁用** |
| 递归互斥锁 | 1 | 0 | 无 | 同持有者重入放行 |

创建函数全部是 `xQueueGenericCreate(len, size, type)` 的宏糖——**类型烙在 ucQueueType 上**，收发路径据此走特化分支。

## 6.3 队列存储的环形布局

```
pcHead    pcWriteTo(下一个写位)         pcTail
  │          │                            │
  ▼          ▼                            ▼
  ┌────┬────┬────┬────┬────┬────┬────┬────┐
  │ D0 │ D1 │ 空 │ 空 │ ...                │  uxLength = 8
  └────┴────┴────┴────┴────┴────┴────┴────┘
  ▲          ▲
  pcReadFrom(下一个读位)          
  uxMessagesWaiting = 2

写: memcpy(pcWriteTo, item, uxItemSize); pcWriteTo += uxItemSize;
    if( pcWriteTo >= pcTail ) pcWriteTo = pcHead;   ← 环回
读: pcReadFrom -= uxItemSize; if( pcReadFrom < pcHead ) pcReadFrom = pcTail - uxItemSize;
    memcpy(buf, pcReadFrom, uxItemSize);
```

读指针是**回退式**（先减再读）——这使"读最后一个写过的元素"（xQueuePeek/优先唤醒直传场景）成为 O(1)。自研队列的 head/tail 递增 + 掩码方案等价（rtos_design.md 8.1 环形缓冲）。

## 6.4 xQueueGenericSend 的完整路径（四条分支）

```c
BaseType_t xQueueGenericSend( xQueue, pvItemToQueue, xTicksToWait, xCopyPosition )  [简化]
{
    taskENTER_CRITICAL();
    {
        if( ( uxMessagesWaiting < uxLength ) || ( xCopyPosition == queueOVERWRITE ) )
        {   /* ── 快路径: 有空位(或覆盖写) ── */
            prvCopyDataToQueue( pxQueue, pvItemToQueue, xCopyPosition );
                /* 内含互斥锁特化分支(7.3): give 时归还继承优先级 */

            /* 唤醒一个等待接收者: */
            if( xTaskRemoveFromEventList( &pxQueue->xTasksWaitingToReceive ) != pdFALSE )
            {
                /* 被唤醒者优先级更高 → 本次调用要 yield */
                queueYIELD_IF_USING_PREEMPTION();
            }
            taskEXIT_CRITICAL();
            return pdPASS;
        }
        else if( xTicksToWait == 0 )
        {   /* ── 满且不等待 ── */
            taskEXIT_CRITICAL();
            return errQUEUE_FULL;
        }
    }
    taskEXIT_CRITICAL();
    /* ── 慢路径: 满且愿意等 ── */
    vTaskSuspendAll();  /* + prvLockQueue —— 锁定(6.7) */
    {
        taskENTER_CRITICAL();
        {
            ... 再查一次满(此刻可能已被中断塞进数据) ...
            if( 仍满 )
            {
                vTaskPlaceOnEventList( &pxQueue->xTasksWaitingToSend, xTicksToWait );
                    /* 挂发送等待链(优先级序) + 挂延时链(超时) —— 双链登记! */
            }
            else { ... 回快路径 ... }
        }
        taskEXIT_CRITICAL();
    }
    xTaskResumeAll();  /* 解锁 + 若已就绪则 yield */
    portYIELD_WITHIN_API();   /* 让出 CPU, 等被唤醒或超时 */

    /* ── 醒来后: 循环回到函数开头重查(重查模式, 5.3) ── */
}
```

四条分支对应：**快路径（有空位） / 零等待失败 / 阻塞慢路径 / 醒来重查**。注意结构：先临界区快查，慢路径前先 `vTaskSuspendAll + 锁队列`——这是队列锁定机制的登场前奏。

## 6.5 xQueueGenericReceive（信号量 take 的本体）

```c
BaseType_t xQueueGenericReceive( xQueue, pvBuffer, xTicksToWait, xJustPeek )  [简化]
{
    for( ;; )
    {
        taskENTER_CRITICAL();
        {
            if( uxMessagesWaiting > 0 )
            {   /* ── 快路径: 有数据/有计数 ── */
                prvCopyDataFromQueue( pxQueue, pvBuffer );   /* 信号量: 仅减计数 */
                if( xJustPeek == pdTRUE ) ... else ...
                /* 唤醒一个等待发送者(队满时睡下的): */
                if( xTaskRemoveFromEventList( &pxQueue->xTasksWaitingToSend ) != pdFALSE )
                    queueYIELD_IF_USING_PREEMPTION();
                taskEXIT_CRITICAL();
                return pdPASS;
            }
            if( xTicksToWait == 0 ) { taskEXIT_CRITICAL(); return errQUEUE_EMPTY; }
            /* 互斥锁: 首次记录超时基准 */
        }
        taskEXIT_CRITICAL();

        /* ── 慢路径: 锁队列 + 阻塞或超时判定 ── */
        vTaskSuspendAll();  prvLockQueue( pxQueue );
        taskENTER_CRITICAL();
        {
            if( xTaskCheckForTimeOut( &xTimeOut, &xTicksToWait ) == pdFALSE )
            {
                if( 仍空 )
                {
                    #if configUSE_MUTEXES
                        if( ucQueueType == queueQUEUE_IS_MUTEX && 有持有者 )
                            vTaskPriorityInherit( pxMutexHolder );   /* ★7.2: 继承点 */
                    #endif
                    vTaskPlaceOnEventList( &pxQueue->xTasksWaitingToReceive, xTicksToWait );
                    prvUnlockQueue( pxQueue );
                    taskEXIT_CRITICAL();
                    xTaskResumeAll();
                    portYIELD_WITHIN_API();     /* 切走, 睡眠 */
                }
                else { 解锁; 重试(循环回顶部快查); }
            }
            else
            {   /* ── 超时: 5.3 段3 已把我从两条链摘干净, 此处只是确认失败 ── */
                解锁; taskEXIT_CRITICAL(); xTaskResumeAll();
                return errQUEUE_EMPTY;
            }
        }
    }
}
```

**重查模式全貌**：醒来（无论被唤醒还是超时）→ 循环回顶部 → 临界区重查队列状态 → 有数据就成功（哪怕本是超时醒的）、没数据且 `xTaskCheckForTimeOut` 报真超时才失败。与自研"wait_result 信箱直接判决"的差异在 5.3 已述——**FreeRTOS 的成功定义是"重查成功"，天然容忍唤醒后数据被抢**。

## 6.6 xTaskRemoveFromEventList：唤醒的核心（完整）

```c
BaseType_t xTaskRemoveFromEventList( const List_t * const pxEventList )  [简化]
{
    TCB_t * pxUnblockedTCB;
    BaseType_t xReturn;

    pxUnblockedTCB = ( TCB_t * ) listGET_OWNER_OF_HEAD_ENTRY( pxEventList );
        /* ↑ 等待链按"反序优先级值"升序 → 链首=最高优先级等待者 */
    ( void ) uxListRemove( &( pxUnblockedTCB->xEventListItem ) );   /* 摘事件链 */

    if( uxSchedulerSuspended == pdFALSE )
    {
        ( void ) uxListRemove( &( pxUnblockedTCB->xStateListItem ) ); /* 摘延时链(提前醒) */
        prvAddTaskToReadyList( pxUnblockedTCB );

        if( pxUnblockedTCB->uxPriority > pxCurrentTCB->uxPriority )
            xReturn = pdTRUE;      /* 告诉调用方: 该 yield 了 */
        else
            xReturn = pdFALSE;
    }
    else
    {   /* 调度器挂起: 摘事件链但转入 pending 待结转 */
        vListInsertEnd( &xPendingReadyList, &( pxUnblockedTCB->xEventListItem ) );
        xReturn = pdTRUE;          /* 保守: 恢复后再判 */
    }
    return xReturn;
}
```

**先到者全摘、后到者扑空**——与自研 3.7 阶段四的竞争裁定结构完全一致：唤醒方摘两条链（事件+延时），超时方（tick）也摘两条链（延时+事件）。两个修改方都被临界区串行化，不存在"两边都摘到"或"都没摘到"的中间态。

## 6.7 队列锁定：cRxLock/cTxLock 的数值语义

**先厘清两种"延迟"的边界**——这是文档与口碑里最容易混淆的地方：

| 机制 | 保护什么窗口 | 谁触发 |
|---|---|---|
| **队列锁定**（cRxLock/cTxLock） | 队列 API **内部**的短窗口：从 `vTaskSuspendAll + prvLockQueue`（决定阻塞）到 `prvUnlockQueue + xTaskResumeAll`（真正让出）之间 | 队列 API 自己（xQueueGenericSend/Receive 慢路径） |
| **挂起期结转**（xPendingReadyList） | **用户**调用 vTaskSuspendAll 的整个区间 | xTaskRemoveFromEventList 检测到调度器挂起 |

**队列锁定要解决的问题**：决定阻塞到真正让出之间，调度器已挂起、任务已挂上事件链但尚未 yield——此时若 ISR 来 give/send，**不能去动事件链**（任务正被半登记，摘除它会与正在进行的登记交错），**也不能丢这个唤醒事件**。

**方案**：锁定期间 ISR 侧操作不碰等待链，只把计数器加一；解锁时按累计数补发：

| 计数值 | 含义 |
|---|---|
| queueUNLOCKED (=0) | 正常态 |
| queueLOCKED_UNMODIFIED (= -1) | 已锁定、期间尚无事件 |
| ≥ 0 的正数 N | 锁定期间发生了 N 次收/发，**每次对应一个应补发的唤醒** |

```c
static void prvUnlockQueue( Queue_t * const pxQueue )  [核心完整]
{
    taskENTER_CRITICAL();
    {
        int8_t cTxLock = pxQueue->cTxLock;

        while( cTxLock > queueLOCKED_UNMODIFIED )      /* 挂起期间发过 N 次 */
        {
            if( listCURRENT_LIST_LENGTH( &pxQueue->xTasksWaitingToReceive ) > 0 )
            {
                ( void ) xTaskRemoveFromEventList( &pxQueue->xTasksWaitingToReceive ); /* 补唤醒 */
            }
            else break;
            --cTxLock;
        }
        pxQueue->cTxLock = queueUNLOCKED;

        /* cRxLock 同理: 补唤醒 xTasksWaitingToSend 的等待者 */
        ...
    }
    taskEXIT_CRITICAL();
}
```

ISR 侧的对应检查（xQueueGenericSendFromISR / xQueueReceiveFromISR）：`if( pxQueue->cTxLock == queueUNLOCKED ) { xTaskRemoveFromEventList(...) } else { ++pxQueue->cTxLock; }`——Tx 计数对应"发过、欠接收者一个唤醒"，Rx 计数对应"收过、欠发送者一个唤醒"。

**为什么两套机制并存**：队列的唤醒语义与数据强耦合（give 可能同时写了数据、唤醒了读者），锁定窗口内必须**整体延后**（连事件链都不能碰）；而用户挂起区间里的直接唤醒（事件组/通知/ResumeFromISR）无数据耦合，可以**先摘事件链**、只把"进就绪表"推迟到 xPendingReadyList。xTaskRemoveFromEventList 内部对 `uxSchedulerSuspended != 0` 的检查同时服务这两条路径（锁定窗口里队列本身也处于调度器挂起态，补唤醒时经它自动转入 pending 链）。



**用户侧纪律**：SuspendAll 区间内**不做任何依赖队列唤醒时序的逻辑**——18 章反模式实例。

## 6.8 信号量 give 与互斥锁 give 的分岔

```
xQueueGenericSend → prvCopyDataToQueue:
  ucQueueType == 普通队列   → memcpy 进环形区, 计数++
  == 信号量(二值/计数)      → uxMessagesWaiting++ (封顶 uxLength)
  == 互斥锁                 → ★vTaskPriorityDisinherit(当前持有者)  /* 7.3 归还继承 */
                              清 pxMutexHolder, 计数=1(锁变"可用")
```

信号量 give 从 ISR 可调（xSemaphoreGiveFromISR）；互斥锁 give 从 ISR **禁止**——`xQueueGiveFromISR` 内部用断言双重拦截：`configASSERT( pxQueue->uxItemSize == 0 )`（只许信号量）加 `configASSERT( !( 是互斥锁 && 有持有者 ) )`。原因：互斥锁的 owner 语义（"释放者必须是持有者"）与优先级继承（对"任务"才有意义）在中断上下文里都不成立——FreeRTOS 在 API 层堵死（自研同构，rtos_usb 系列文档明示）。

---

# 第七章 优先级继承的完整路径

## 7.1 数据基础

| 字段 | 位置 | 作用 |
|---|---|---|
| uxPriority / uxBasePriority | TCB | 当前/基础；不等即"正被继承" |
| uxMutexesHeld | TCB | 持锁计数：递归放行 + "归还时是否还有人帮我撑优先级"判定 |
| pxMutexHolder | xQUEUE(u.xSemaphore) | 指向持有者 TCB——继承的操作对象 |

## 7.2 vTaskPriorityInherit：take 时的提升（逐步数值推演）

场景：任务 L（优先级数值 2=低）持锁 M；任务 H（优先级数值 5=高）`xSemaphoreTake(M, 100)`。FreeRTOS 方向约定：**数值大 = 优先级高**——"持有者优先级低于我"即 `2 < 5`：

```c
void vTaskPriorityInherit( TaskHandle_t const pxMutexHolder )  [核心完整]
{
    TCB_t * const pxMutexHolderTCB = ( TCB_t * ) pxMutexHolder;

    if( pxMutexHolder != NULL )
    {
        if( pxMutexHolderTCB->uxPriority < pxCurrentTCB->uxPriority )  /* 持有者数值更小=更低: 才需提升 */
        {
            /* 同步更新事件链节点的反序值——但有个前提: 该值没被别的东西占用 */
            if( ( listGET_LIST_ITEM_VALUE( &( pxMutexHolderTCB->xEventListItem ) )
                  & taskEVENT_LIST_ITEM_VALUE_IN_USE ) == 0UL )
            {
                listSET_LIST_ITEM_VALUE( &( pxMutexHolderTCB->xEventListItem ),
                    ( TickType_t ) configMAX_PRIORITIES - ( TickType_t ) pxCurrentTCB->uxPriority );
            }

            /* 持有者在就绪表 → 摘旧桶挂新桶; 否则只改字段 */
            if( listIS_CONTAINED_WITHIN( &pxReadyTasksLists[ pxMutexHolderTCB->uxPriority ],
                                         &pxMutexHolderTCB->xStateListItem ) )
            {
                if( uxListRemove( &pxMutexHolderTCB->xStateListItem ) == 0 )
                    taskRESET_READY_PRIORITY( pxMutexHolderTCB->uxPriority );
                pxMutexHolderTCB->uxPriority = pxCurrentTCB->uxPriority;   /* ★提升: 2→5 */
                prvAddTaskToReadyList( pxMutexHolderTCB );
            }
            else
            {
                pxMutexHolderTCB->uxPriority = pxCurrentTCB->uxPriority;
            }
        }
    }
}
```

`taskEVENT_LIST_ITEM_VALUE_IN_USE = 0x80000000UL` 这个守卫位是事件组与互斥锁的停火协议：**若持有者正阻塞在某事件组上，其事件链节点的 xItemValue 装的是打包的等待条件（8.2），不是反序优先级**——继承路径不得覆盖它。守卫位置位则跳过更新（反正事件组是无序链，优先级序对它无意义）。

另一个有意为之的简化：**只更新值、不重插事件链**。若持有者本身正阻塞在另一对象的等待链上，其在新优先级下的链上位置失真——官方在论坛里确认过不重插的理由：重插的耗时与链长相关（不可控的 O(n)），而唤醒顺序的短暂失真被容忍。后果见 7.7 的真实案例。

数值推演：

```
初始: L(基2) 持 M, H(基5) 运行
H: xSemaphoreTake(M) → 锁被持有, 持有者L(2) < H(5)
  → vTaskPriorityInherit(L): L 提升到 5, 就绪表搬家 [2号桶→5号桶]
  → H 挂 M 的等待链(反序值 = MAX-5), 挂延时链(超时100)
  → H 让出 CPU
此刻: L 以优先级5 运行(沾了 H 的光) → 它很快干完活
L: xSemaphoreGive(M) → prvCopyDataToQueue 互斥锁分支:
  → vTaskPriorityDisinherit(L, ...):
      L 持锁数减完(uxMutexesHeld 0) 且 L 的 uxBasePriority(2) 与当前(5)不等
      → L 归位到 2, 就绪表搬回 [5号桶→2号桶]
  → xTaskRemoveFromEventList(M 等待链): 唤醒链首 H
  → H(5) > L(2) → yield → H 抢占运行
H 醒来: 重查循环 → 拿到锁, vTaskPriorityInherit? 
  (H 自己成为持有者, M 的等待链已空, 无继承对象) → H 正常运行
```

## 7.3 vTaskPriorityDisinherit：give 时的归还

```c
BaseType_t xTaskPriorityDisinherit( TaskHandle_t pxMutexHolder, ... )  [简化]
{
    if( pxMutexHolderTCB != pxCurrentTCB ) ... /* give 者=持有者(API已保证) */
    --pxMutexHolderTCB->uxMutexesHeld;
    if( pxMutexHolderTCB->uxMutexesHeld == 0U )          /* ★只有"还完了最后一把锁"才归位 */
    {
        if( pxMutexHolderTCB->uxPriority != pxMutexHolderTCB->uxBasePriority )
        {
            /* 持多把锁时不归位——因为内核没有"我还持有哪些锁、它们各自的
               等待者最高优先级"的账本, 无法算出该归位到哪。这是官方承认的
               有意简化(GitHub issue FreeRTOS/FreeRTOS#1168), 后果见 13.1.3 */
            就绪表搬家回 uxBasePriority;
            pxMutexHolderTCB->uxPriority = pxMutexHolderTCB->uxBasePriority;
        }
    }
    ...
}
```

**所有权移交不在 give 里**：give 只归还优先级 + 清 owner；被唤醒的等待者在自己的重查循环里重新 take 成功、成为新 owner。对比自研"give 内先移交所有权再唤醒"（rtos_design.md 7.3 所有权预移交——消除"唤醒后锁又被第三者抢走"的窗口）：FreeRTOS 用重试循环吞掉该窗口——被抢走就再睡一次，语义保守但路径简单。

## 7.4 超时路径：vTaskPriorityDisinheritAfterTimeout

H 带超时等锁、超时先到（5.3 段 3 摘 H 的两条链之后），take 的超时分支调用：

```c
void vTaskPriorityDisinheritAfterTimeout( TaskHandle_t const pxMutexHolder,
                                          UBaseType_t uxHighestPriorityWaitingTask )  [核心完整]
{
    /* uxHighestPriorityWaitingTask = 这把锁剩余等待者中的最高优先级
       (由调用方从等待链链首取; 链空则传 0) */

    /* 目标优先级 = max(持有者基础优先级, 本锁剩余等待者最高优先级) */
    if( pxTCB->uxBasePriority < uxHighestPriorityWaitingTask )
        uxPriorityToUse = uxHighestPriorityWaitingTask;
    else
        uxPriorityToUse = pxTCB->uxBasePriority;

    if( pxTCB->uxPriority != uxPriorityToUse )
    {
        /* ★守卫: 仅当持有者恰好只持这一把锁才调整(uxOnlyOneMutexHeld) */
        if( pxTCB->uxMutexesHeld == uxOnlyOneMutexHeld )
        {
            ... 就绪表搬家(若就绪) ...
            pxTCB->uxPriority = uxPriorityToUse;
            listSET_LIST_ITEM_VALUE( &( pxTCB->xEventListItem ),
                configMAX_PRIORITIES - uxPriorityToUse );   /* 反序值同步 */
        }
    }
}
```

与 7.3 的两点差异：**它做部分重算**（考虑本锁剩余等待者，不是简单归位到 base），但**账本只覆盖这一把锁**——若持有者还持有别的锁、那些锁上有更高优先级的等待者，它们不在计算范围内；且守卫 `uxMutexesHeld == 1` 意味着多锁场景下此函数**直接不动**。两层简化叠加的结果是"归还偏保守（偏高）"而非"冲掉继承"——13.1.3 的详细分析。自研的 `rtos_internal_mutex_waiter_left`（rtos_design.md 7.4）走"遍历持有者所有锁的剩余等待者取最高"的完整重算路线——**正确性更强**，代价是每把锁要登记持有者。FreeRTOS 官方在 issue #1168 讨论中承认了该简化的存在与文档缺失。

## 7.5 递归互斥锁

```
xSemaphoreTakeRecursive: pxMutexHolder == pxCurrentTCB → uxMutexesHeld++ 直接成功
xSemaphoreGiveRecursive: --uxMutexesHeld; 减到 0 才走 7.3 真释放
```

## 7.6 链式反转为何无解（本实现）

A(基2) 持 M1，又去 take M2（被 C 持）；H(基5) 等 M1：
- H 等 M1 → A 提升到 5（一级）；
- 但 A 在等 M2 → **C 不会**被提升——继承只传一跳；
- M(基3) 抢占 C → 反转在第二层复活。

FreeRTOS 与自研（L10）均未解——业界方案是**优先级天花板**：锁创建时绑定天花板优先级 P（≥ 所有可能使用者），take 前无条件把任务提到 P（不必等竞争发生），give 后归位。天花板协议死锁自由且反转自由，代价是"未竞争也付费"。需要时在应用层实现：每把锁配一个静态优先级表，take 前后手动 set/restore。

## 7.7 内核自身的已知缺陷与社区真实案例

优先级继承是 FreeRTOS 里缺陷密度最高的区域。以下案例全部来自 FreeRTOS 官方仓库的 issue/论坛记录（均可溯源），它们同时也是第 13 章排障方法的实证：

**案例 1：归还优先级的"多锁盲区"（[FreeRTOS/FreeRTOS#1168](https://github.com/FreeRTOS/FreeRTOS/issues/1168)）**
用户 Hawk777 报告：`xSemaphoreGive` → `prvCopyDataToQueue` → `xTaskPriorityDisinherit` 只在**不再持有任何互斥锁**时才归还优先级。他的真实 bug：give 一把锁后立即再 take，期望锁被交接给更高基础优先级的等待者，结果交接没发生——因为 give 方还持着另一把不相干的锁，优先级没降、没有触发切换。官方成员 paulbartell 确认并推动文档更新。**衍生讨论**（kstribrnAmzn）：超时路径（7.4）同样只看本锁等待者，理想行为应考虑持有者所有锁的全部等待者。

**案例 2：vTaskPrioritySet 的事件链值失配（[FreeRTOS-Kernel#1364](https://github.com/FreeRTOS/FreeRTOS-Kernel/issues/1364)）**
`vTaskPrioritySet` 修改任务优先级时，把事件链节点的反序值直接按 `uxNewPriority` 写，**无视任务当前的实际（可能被继承抬高的）优先级**——`uxPriority=20, uxNewPriority=1` 时事件值写成 31，而正确的反序应是 32-20=12。后果：该任务下次阻塞时挂到等待链错误位置，低优先级任务永远排在它前面 → 事实上的唤醒饿死。修复建议（PR #1380）：按 `pxTCB->uxPriority` 而非 `uxNewPriority` 计算。这是 3.2 节"反序值只在创建时写一次"的连锁代价。

**案例 3：同任务重复 take 同一互斥锁触发断言（[FreeRTOS-Kernel#1360](https://github.com/FreeRTOS/FreeRTOS-Kernel/issues/1360)）**
新版内核（main 分支）中 `vTaskPriorityDisinheritAfterTimeout` 的 `configASSERT( pxTCB != pxCurrentTCB )` 可被"take mutex1 → 被继承 → take mutex2 → give mutex1 → 再 take mutex2 并超时"的序列触发——超时路径以为在处理"别人"的继承，实际持有者就是自己。属于冷门但真实的边角。

**案例 4：悬空任务句柄导致 Store 访问错误（社区项目 [cornucopia-machines#573](https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/573)）**
`xTaskNotify` 写 `pxTCB->ulNotifiedValue[0]` 时崩溃在地址 0x40——TCB 指针实为 NULL（句柄已悬空但非 NULL：TCB 被 free 后内存复用、首字被清零）。根因：短生命周期任务把句柄注册进消息表后退出，idle 回收 TCB，迟到的 MQTT ACK 触发通知 → 野指针写。这正是 22 章"句柄悬空"的实战标本。

这四个案例的共性：**缺陷全部藏在"组合路径"里**——多锁、优先级变更+继承、超时+继承、删除+通知。单点路径早已收敛，组合路径是测试与评审的重点区。

---

# 第八章 任务通知与事件组

## 8.1 任务通知：绕过一切结构的快路

数据基础（3.1 TCB 节选）：每任务内嵌 `ulNotifiedValue[]`（32 位值）与 `ucNotifyState[]`（三态：NOT_WAITING / WAITING / RECEIVED）。

**等待侧 xTaskNotifyWait**（语义完整）：

```
taskENTER_CRITICAL();
  if( ucNotifyState == RECEIVED )            /* 通知先到: 直接消费 */
      { 取走值; state = NOT_WAITING; 返回成功 }
  else if( timeout == 0 )                     /* 不等: 失败 */
      返回失败
  else
      {
        ucNotifyState = WAITING;              /* ★挂牌"我在等" */
        prvAddCurrentTaskToDelayList( timeout );  /* 只挂延时链! 无事件链 */
        taskEXIT_CRITICAL(); portYIELD_WITHIN_API();   /* 睡 */
      }
醒来(被通知 or 超时):
  重查 ucNotifyState: RECEIVED → 成功(值已在 TCB 里)
                    仍 WAITING → 超时, state 复位 NOT_WAITING
```

**通知侧 xTaskNotify**（语义完整）：

```
taskENTER_CRITICAL();
  ★ucNotifyState = taskNOTIFICATION_RECEIVED;   /* 无条件置位: 无论目标当时在不在等 */
  按 action 修改目标 ulNotifiedValue (NO_ACTION/SET_BITS/INCREMENT/SET_VALUE/...)
  if( 目标原状态 == taskWAITING_NOTIFICATION )   /* 它真在等 → 立即唤醒 */
  {
      ( void ) uxListRemove( &pxTCB->xStateListItem );   /* 摘延时链 —— 无事件链可摘! */
      prvAddTaskToReadyList( pxTCB );
      if( 更高优先级 ) 请求 yield
  }
  ( ISR 版: 调度器挂起时挂 xPendingReadyList, 同 4.8 )
```

**RECEIVED 无条件置位**是"通知先到、wait 后到也不丢"的全部机制：任务还没调用 wait 时通知到达，状态被置 RECEIVED、值已写入 TCB；之后 wait 的第一个分支（RECEIVED → 直接消费）立即返回——不需要任何"登记-匹配"逻辑。这也是 `ulTaskNotifyTake` 的已知陷阱：它返回**当前通知值**而不校验"值是否属于本次等待"——前一次操作残留的计数值会被误当成本次事件（真实案例见 7.7 案例 4 的衍生讨论；需要按值校验请用 `xTaskNotifyWait` 并配合清位参数）。

**为什么快**（官方原文："Unblocking an RTOS task with a direct notification is 45% faster and uses less RAM than unblocking a task using an intermediary object such as a binary semaphore"，官方基准注脚为 GCC -O2）：零队列结构、零事件链插入/摘除、零拷贝——通知状态与值都在目标 TCB 里，通知方直达。**语义限制**（官方文档明示）：不广播（单目标）；一个任务同时只能阻塞在数组的一个索引上——"等任意多个来源之一"的选择性等待做不到——超出限制再上信号量/队列/事件组。另外注意 **Stream/Message Buffer 占用通知数组索引 0**——用了流缓冲就别再裸用索引 0。

**自研对照**：rtos_task_notify/notify_wait（rtos_design.md 5.7）同宗——含"超时瞬间通知不丢失"的边界处理（state=RECEIVED 保留给下次 wait），FreeRTOS 的 RECEIVED 挂牌同样实现了这个语义：超时醒来发现 RECEIVED（通知恰在超时后到达）→ 重查按成功处理。

## 8.2 事件组：把等待条件打包进链表节点

数据基础：`uxEventBits`（当前位）+ `xTasksWaitingForBits`（**无序**等待链——因为满足条件因人而异，必须全扫描，不能像队列只唤醒链首）。

等待条件打包（event_groups.c 的位约定）：

```c
#define eventCLEAR_EVENTS_ON_EXIT_BIT   0x01000000UL
#define eventUNBLOCKED_DUE_TO_BIT_SET   0x02000000UL   /* 唤醒方回写的"成功"标志 */
#define eventWAIT_FOR_ALL_BITS          0x04000000UL
#define eventEVENT_BITS_CONTROL_BYTES   0xFF000000UL   /* 高8位是控制位, 用户位只有低24位 */
/* xEventListItem.xItemValue = 等待的位 | 以上打包标志(经 taskEVENT_LIST_ITEM_VALUE_IN_USE 额外标位) */
```

**xEventGroupWaitBits 流程**（语义完整）：

```
vTaskSuspendAll();                       /* ★不是临界区! 见下文 SetBits 的同理选择 */
  if( prvTestWaitCondition( 当前bits, 等待bits, ANY/ALL ) )
      { 满足: 按 CLEAR_ON_EXIT 清位; 返回当前 bits }
  else if( timeout == 0 ) 返回当前 bits
  else
      vTaskPlaceOnUnorderedEventList( &等待链,
              等待bits | 打包标志, timeout );       /* "无序"版: item 值=条件 */
xTaskResumeAll(); 若未切换则 portYIELD_WITHIN_API();   /* 睡 */
醒来:
  uxReturn = uxTaskResetEventItemValue();            /* 读回自己的 item 值 */
  if( uxReturn 含 eventUNBLOCKED_DUE_TO_BIT_SET )    /* set-bits 唤醒的证据 */
      { 返回 (值 & ~CONTROL_BYTES) —— 满足时刻的 bits 快照 }
  else 超时 → 返回当前 bits(另查一次条件)
```

**xEventGroupSetBits 流程**（唤醒方，核心语义）：

```
vTaskSuspendAll();                /* ★不用临界区: 扫描是O(n), 必须保住中断响应 */
  uxEventBits |= 设置位;
  pxListItem = 链首;  while( pxListItem != 链尾 ):
      pxNext = pxListItem->pxNext;          /* 先存后继——本节点可能马上被摘 */
      解包"等待bits + ANY/ALL" → prvTestWaitCondition
      满足者:
          若其 CLEAR_ON_EXIT 置位: uxBitsToClear |= 其等待位    /* 累计, 不立即清 */
          vTaskRemoveFromUnorderedEventList( pxListItem,
                  uxEventBits | eventUNBLOCKED_DUE_TO_BIT_SET );
                  /* ★item 值被整体改写为"当前事件位 | UNBLOCKED 标志"——
                     不再是等待条件, 而是给唤醒者的"判决书+位快照" */
      pxListItem = pxNext;
  uxEventBits &= ~uxBitsToClear;            /* 扫描结束后统一清位一次 */
xTaskResumeAll();
```

三个值得单独点名的设计：

1. **SuspendAll 而非临界区**——扫描整链是 O(n)，用关中断保护会把中断延迟放大到链长量级；挂起调度器只停任务切换，中断照常。这是"临界区只装指针操作"原则（14.4）在内核内部的身体力行。
2. **判决书 = 位快照 + UNBLOCKED 标志**——`vTaskRemoveFromUnorderedEventList` 把 item 值改写为"当前事件位 | UNBLOCKED"，等待者醒来经 `uxTaskResetEventItemValue()` 读回：有标志 → 是事件唤醒且拿到的是满足时刻的位快照；无标志 → 超时。与自研 rtos_event 的回传机制（rtos_design.md 第九章）同构。
3. **ClearOnExit 的"先唤醒全部、最后统一清"**——扫描期间先把所有满足者都唤醒（它们拿到的都是清位前的快照），扫描完再统一清位。否则第一个被唤醒者的清位会改变后续等待者的判定（论坛上真实误解案例的起点）。

**UNBLOCKED_DUE_TO_BIT_SET = FreeRTOS 版的 wait_result 信箱**：与自研 rtos_event（rtos_design.md 第九章 + 2.3 表 event 组的 transfer_buf 回传满足位值）机制同构。**扫描中删除节点**依赖"先存后继再摘"（pxNext 预取），与 uxListRemove 的 pxIndex 修正（2.5）配套。

## 8.3 三种"等待侧"结构对照

| | 队列/信号量 | 事件组 | 任务通知 |
|---|---|---|---|
| 等待侧结构 | 事件链（优先级序） | 事件链（无序，值=条件） | **无链**，TCB 三态 |
| 唤醒选择 | 链首=最高优先级 | 全扫描满足者 | 直达单目标 |
| 成功/超时判定 | 重查队列（5.3） | UNBLOCKED 标志 | state==RECEIVED |
| 自研对应 | wake_highest + wait_result | event 模块（位组复用） | notify（state 三态） |

---

# 第九章 软件定时器：守护任务架构

## 9.1 架构与数据结构

```
任何上下文 ──xTimerStart/Stop/Reset/ChangePeriod──▶ 命令入队 xTimerQueue
                                                     (Daemon 唯一消费者)
prvTimerTask(守护任务, 任务名 "Tmr Svc", 优先级 configTIMER_TASK_PRIORITY):
    循环: 算下一到期 → 以"到期间隔"为超时阻塞等命令队列
        ├─ 命令到: prvProcessReceivedCommands(约十种 tmrCOMMAND_*)
        └─ 到期到: prvProcessExpiredTimer(逐个回调)
```

```c
struct tmrTimerControl {                  /* Timer_t (沿用旧命名以兼容内核感知调试器) */
    const char * pcTimerName;             /* 调试用 */
    ListItem_t xTimerListItem;            /* 挂活跃链(升序到期)或溢出链 */
    TickType_t xTimerPeriodInTicks;       /* 周期 */
    void * pvTimerID;                     /* 回调参数(同一回调服多个定时器时区分用) */
    TimerCallbackFunction_t pxCallbackFunction;
    uint8_t ucStatus;                     /* 位域: tmrSTATUS_IS_ACTIVE(0x01)
                                             / STATICALLY_ALLOCATED(0x02) / AUTORELOAD(0x04) */
};
static List_t xActiveTimerList1, xActiveTimerList2;    /* ★对滚双链, 同延时链方案 */
```

**定时器链对滚**：延时链的双链方案（第五章）在 timers.c 原样复用——`prvSampleTimeNow` 检测 tick 翻转、交换两条活跃链指针（任务侧等价物是 `taskSWITCH_DELAYED_LISTS`）。**同一招式两处复用**是这套设计的复利。

**守护任务的真实身份**：函数名 `prvTimerTask`，任务名默认 `"Tmr Svc"`（可用 `configTIMER_SERVICE_TASK_NAME` 覆盖）——GDB 里看到的 "Tmr Svc" 就是它。它在 `vTaskStartScheduler` 阶段随 `xTimerCreateTimerTask` 创建（需要 `configUSE_TIMERS=1`）。

## 9.2 命令处理（约十种）与链表操作

| 命令 | 链表动作 |
|---|---|
| Start / StartFromISR | 摘（若在链）→ 设到期=now+period → 有序插入活跃链 |
| Stop / StopFromISR | 摘 |
| Reset / ResetFromISR | 同 Start（重新起算） |
| ChangePeriod / ChangePeriodFromISR | 摘 → 改 period → 插 |
| Delete | 摘 + 释放（动态分配时） |
| PendFuncCall / PendFuncCallFromISR | 不是定时器命令——把任意函数指针挂到守护任务执行（中断延迟工作移交的官方通道） |

命令结构 `DaemonTaskMessage_t{命令ID, Timer_t*, 附加值}`——**对定时器的一切操作都坍缩成消息**，天然支持任意上下文（含 ISR）、天然串行化（无并发问题）。`xTimerPendFunctionCall` 值得单列：它是"把 ISR 里的长处理扔到任务上下文"的官方通道，比自建队列+任务的脚手架省事得多。

## 9.3 守护任务主循环的时序语义

```
prvProcessTimerOrBlockTask( 下一到期时刻 xNextExpireTime ):
  若链空: 无限期阻塞等命令
  否则:   以 (xNextExpireTime - now) 为超时等命令   ← 阻塞时长=恰好睡到到期
醒来两条路:
  a) 队列有命令: prvProcessReceivedCommand → 回到循环重算下一到期
  b) 无命令即超时=到期: prvProcessExpiredTimer:
        摘链首定时器 → 调用其回调 → 若 AutoReload: 重插(now+period)
        循环处理同 tick 到期的后续定时器
```

**回调上下文 = 守护任务**——21 章头号坑的根源：回调阻塞则全系统定时器冻结。

**自研对照**（rtos_design.md 第十章）：命令队列、守护任务、三阶段临界区纪律——两个独立实现收敛到同一架构（"回调须串行+时间驱动"的局部最优）。差异：自研定时器回调运行于定时器服务任务（同守护）；FreeRTOS 多了对滚双链（自研单链+重插修正）。

---

# 第十章 内存管理：五种堆与 heap_4

## 10.1 五种堆选型表

| 堆 | 策略 | 合并 | ISR 安全 | 适用 |
|---|---|---|---|---|
| heap_1 | 只分配不释放 | — | 否 | 任务永不删除 |
| heap_2 | first-fit | **无合并** | 否 | 已弃用(碎片) |
| heap_3 | 包装 newlib malloc | 取决于newlib | 锁取决于实现 | 想用系统堆 |
| heap_4 | first-fit + **地址序双向合并** | ✔ | 否 | **默认推荐** |
| heap_5 | heap_4 + 多内存区 | ✔ | 否 | 分散 RAM |

## 10.2 heap_4 详解

```c
typedef struct A_BLOCK_LINK {
    struct A_BLOCK_LINK * pxNextFreeBlock;   /* 空闲链next(按地址升序) */
    size_t xBlockSize;                       /* 含头部的块大小 */
} BlockLink_t;
/* 每块开销: 2×sizeof(void*)/size_t = 8字节(32位) */

static void prvInsertBlockIntoFreeList( BlockLink_t * pxBlockToInsert )  [核心完整]
{
    BlockLink_t * pxIterator;
    uint8_t * puc;

    /* 走到地址比"我"大的第一个空闲块(地址序链) */
    for( pxIterator = &xStart;
         pxIterator->pxNextFreeBlock < pxBlockToInsert;
         pxIterator = pxIterator->pxNextFreeBlock ) { }

    /* 前向合并: 我紧接前一个空闲块? */
    puc = ( uint8_t * ) pxIterator;
    if( ( puc + pxIterator->xBlockSize ) == ( uint8_t * ) pxBlockToInsert )
    {
        pxIterator->xBlockSize += pxBlockToInsert->xBlockSize;
        pxBlockToInsert = pxIterator;            /* 吞并成一体 */
    }

    /* 后向合并: 后一个空闲块紧接我? */
    puc = ( uint8_t * ) pxBlockToInsert;
    if( ( puc + pxBlockToInsert->xBlockSize ) == ( uint8_t * ) pxIterator->pxNextFreeBlock )
    {
        if( pxIterator->pxNextFreeBlock != pxEnd )
        {
            pxBlockToInsert->xBlockSize += pxIterator->pxNextFreeBlock->xBlockSize;
            pxBlockToInsert->pxNextFreeBlock = pxIterator->pxNextFreeBlock->pxNextFreeBlock;
        }
        else pxBlockToInsert->pxNextFreeBlock = pxEnd;
    }
    else
        pxBlockToInsert->pxNextFreeBlock = pxIterator->pxNextFreeBlock;

    pxIterator->pxNextFreeBlock = pxBlockToInsert;
}
```

**地址序双向合并**对比自研（first-fit + 前向合并，L7 无后向）——heap_4 的空闲链按**地址**排序使相邻判定 O(1)（地址相加即判邻接），代价是插入要按地址走链（不是按大小）。自研按大小序插入快但合并只能单向——同一 tradeoff 的两端。

分配（pvPortMalloc 核心）：`vTaskSuspendAll()` 包裹 → first-fit 走地址序链 → 大块分裂（剩余 ≥ 最小块 2×头）→ `xTaskResumeAll()`。

**为什么用 SuspendAll 而非 ENTER_CRITICAL**：malloc 较长（走链+对齐+可能分裂），关中断代价高；挂起调度器保留中断响应、只防任务竞争。**代价：malloc/free 不是 ISR 安全的**（ISR 里调 malloc 会撕裂空闲链——FreeRTOS 文档明示，实际工程用静态池或队列转交任务侧分配）。

## 10.3 栈高水位

创建时染料 0xA5A5A5A5（3.2 步 e）→ `uxTaskGetStackHighWaterMark(h)` 从栈底扫描首个非染料字 → 历史最深剩余字数。**发布前全任务普查一遍，剩余 <25% 加栈**——与自研 PERF_STACK_WATERMARK 同构（rtos_design.md 12.3）。

---

# 第十一章 Cortex-M 移植层与临界区

## 11.1 BASEPRI 临界区（与自研完全同构）

```c
#define portDISABLE_INTERRUPTS()  vPortRaiseBASEPRI()
/*   BASEPRI = configMAX_SYSCALL_INTERRUPT_PRIORITY<<4: 屏蔽数值≥阈值的中断 */
#define portENTER_CRITICAL()      vPortEnterCritical()   /* 屏蔽+uxCriticalNesting++ */
#define portEXIT_CRITICAL()       vPortExitCritical()    /* 计数到0才真正开中断 */
```

- 阈值典型 5（0~4 留给硬实时中断，临界区屏蔽不了它们——**这正是 15 章悲剧的伏笔**）；
- 嵌套计数支持临界区重入，最外层退出才开闸——自研同一机制（rtos_design.md 4.5）；
- PendSV/SysTick 恒设 0xFF 最低优先级——切换与 tick 永远让路。

## 11.2 FromISR 的屏蔽保存/恢复

```c
#define portSET_INTERRUPT_MASK_FROM_ISR()   ulPortRaiseBASEPRI()  /* 返回旧值 */
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(x) vPortSetBASEPRI(x)   /* 恢复入口值 */
```

ISR 用"栈上保存旧值"而非全局计数——嵌套中断各自恢复各自入口的 BASEPRI。自研用全局嵌套计数等价（一个在栈、一个在全局，殊途同归）。

## 11.3 vPortValidateInterruptPriority：独门防御

```c
void vPortValidateInterruptPriority( void )  [简化]
{
    uint32_t ulCurrentInterrupt;
    uint8_t ucCurrentPriority;

    __asm volatile( "mrs %0, ipsr" : "=r"( ulCurrentInterrupt ) );  /* 当前激活的中断号 */

    if( ulCurrentInterrupt >= portFIRST_USER_INTERRUPT_NUMBER )  /* 16: 用户中断起点 */
    {
        ucCurrentPriority = pcInterruptPriorityRegisters[ ulCurrentInterrupt ];
        /* ① 优先级数值必须 >= configMAX_SYSCALL_INTERRUPT_PRIORITY */
        configASSERT( ucCurrentPriority >= ucMaxSysCallPriority );
        /* ② 优先级分组不得越位: AIRCR.PRIGROUP 必须 <= 硬件优先级位数允许的分组 */
        configASSERT( ( portAIRCR_REG & portPRIORITY_GROUP_MASK ) <= ulMaxPRIGROUPValue );
    }
}
/* 由 portASSERT_IF_INTERRUPT_PRIORITY_INVALID() 宏嵌进每个 FromISR API(configASSERT开启时) */
```

两个断言分别堵两类事故：**①** 调用 FromISR API 的中断配成 0~4——临界区内闯入、撕裂链表（15 章的"随机崩溃之王"）；**②** 优先级分组配错——STM32 上若分组不是"全抢占位"（NVIC_PRIORITYGROUP_4），`ucCurrentPriority >= ucMaxSysCallPriority` 的比较语义就失效（子优先级位混进数值比较）。`ucMaxSysCallPriority` 与 `ulMaxPRIGROUPValue` 都是在 `xPortStartScheduler` 里**用硬件实测出来的**（11.4），不是查表猜的。

它捕获的是 **15 章的"随机崩溃之王"**：调用 FromISR API 的中断配成 0~4——临界区内闯入、撕裂链表。自研内核无此防御，同款 bug 靠 hardfault_debug.md 事后追凶——**这是 FreeRTOS 值得抄进自研的第二处**（第一处：双链对滚）。

## 11.4 启动时序（xPortStartScheduler，与 V10.4.6 源码一致）

```
1. configASSERT( configMAX_SYSCALL_INTERRUPT_PRIORITY )   ← 为0则BASEPRI防线全盘失守
2. 优先级位数自检(configASSERT_DEFINED 时):
   向首个用户中断的优先级寄存器写 0xFF, 读回看哪几位"粘住了"
   → 实测出硬件优先级位数 → 算出 ucMaxSysCallPriority(阈值按有效位截断)
     与 ulMaxPRIGROUPValue(合法分组上限)
   → 若有 __NVIC_PRIO_BITS / configPRIO_BITS, 再断言其与实测一致
   (写坏的寄存器恢复原值)
3. PendSV 与 SysTick 压到最低优先级(SHPR3 |= configKERNEL_INTERRUPT_PRIORITY)
4. vPortSetupTimerInterrupt(): SysTick 装载 = configSYSTICK_CLOCK_HZ/configTICK_RATE_HZ - 1, 启动
5. uxCriticalNesting = 0    ← 从启动前哨兵 0xAAAAAAAA 归位
6. vPortEnableVFP() + FPCCR |= ASPEN|LSPEN   ← 使能FPU+惰性压栈(CM4F)
7. prvPortStartFirstTask(): MSP 回复位值 → 清CONTROL.FPCA → cpsie i/f → svc 0
   → vPortSVCHandler 恢复首个任务(4.5)
(此后 SysTick 每 tick → xPortSysTickHandler → xTaskIncrementTick → 需要时 pend PendSV)
```

第 2 步的**硬件实测**是教科书级的防御性设计：不假设"这本芯片是 4 位优先级"，而是写一遍读一遍让硬件自己回答——不同料号的优先级位数不同（3/4/5 位都有），写死的假设在移植时是静默炸弹。

## 11.5 中断优先级配置规则（15 章的预防针）

```
① configMAX_SYSCALL_INTERRUPT_PRIORITY ≠ 0（=0 会禁用BASEPRI全盘失守）
② NVIC 分组 = NVIC_PRIORITYGROUP_4（4位抢占0位子优先级；
   含子优先级的分组会拆坏"数值≥5"的语义）
③ 凡调用 FromISR API 的中断: HAL_NVIC_SetPriority 的抢占数值 ≥ 5
④ 不调任何 FreeRTOS API 的中断: 可用 0~4（真硬实时）
```

---

# 第十二章 FreeRTOS 与自研内核逐项对照

| 维度 | FreeRTOS V10.4 | 自研内核 | 评注 |
|---|---|---|---|
| 链表 | 侵入式环形+哨兵+游标 | 侵入式(就绪双向/延时单向/等待双向) | 同族；哨兵+游标是 FreeRTOS 两件法宝 |
| 就绪表 | uxTopReadyPriority+CLZ | ready_bitmap+ctz | 互为镜像（优先级方向相反） |
| 同级轮转 | 游标顺带（每切换轮一次） | rotate 主动（每tick轮一次） | 语义差异：FreeRTOS 轮转粒度=切换次数 |
| 延时链 | **双链对滚，回绕免疫** | 单链，L1 | **FreeRTOS 胜，建议移植** |
| 超时定位 | pxContainer 摘事件链 | list_head 摘等待链 | 完全同构 |
| 超时判决 | 重查模式（醒来重查对象状态） | 信箱模式（wait_result 预写） | 自研语义更强（唤醒即成功）；FreeRTOS 容错性更高 |
| 唤醒与所有权 | 唤醒后重试获取 | give 内预移交 | 自研消除唤醒-获取窗口 |
| IPC 组织 | queue.c 四合一 | 四模块分立 | 一个重用强、一个职责清 |
| 优先级继承 | 三函数，多锁归还简化 | 重算式（遍历等待者） | **自研正确性更强**（13.1.3） |
| 删除持锁 | 悬空（22章坑） | mutex_release_all 自动移交 | **自研胜** |
| 自删回收 | idle 回收 waiting_termination | idle 回收 pending_free_list | 同构收敛 |
| ISR 域防御 | vPortValidateInterruptPriority | 无 | **FreeRTOS 胜，建议移植** |
| 调度器挂起 | 完整（三套延迟记账） | 无公开API | FreeRTOS 功能全但坑深（6.7/18章） |
| 栈检查 | 切换点两方法+染料 | stack_magic+染料 | 同构 |
| 任务通知 | TCB 三态直达 | 同 | 同宗 |
| 事件组 | item 打包条件+回写标志 | 位组复用+回传满足位 | 同构（字段复用双璧） |
| 定时器 | 守护任务+对滚双链 | 守护任务+单链重插 | 架构同构；对滚再胜一筹 |
| 堆 | heap_4 地址序双向合并 | first-fit 前向合并(L7) | heap_4 抗碎片更优 |
| PendSV/SVC | 见 4.4/4.5 | rtos_design.md 4.2/4.4 | 逐指令等价（唯一正解） |

**总评**：FreeRTOS 在"回绕免疫、ISR 域防御、双向合并堆、配置生态"上领先；自研在"继承正确性、删除善后、唤醒-获取原子性、文档-代码一致性"上领先；其余核心机制两个独立实现高度收敛——互为对方正确性的交叉验证。

---

# 第二部分：复杂问题定位与解决

# 第十三章 优先级反转、继承失效与死锁

## 13.1 优先级反转三级跳

### 13.1.1 经典反转（未用互斥锁）

```
L(优先级数值2=低)  持资源 R(二值信号量"保护")  运行
H(优先级数值5=高)  take(R) 失败 → 阻塞
M(优先级数值3)     就绪 → 抢占 L 长期运行
结果: H 等 L 释放, L 被 M 压制, R 迟迟不释 → H 被 M 间接反转
```



**历史注脚（1997 火星探路者，细节全部可考）**：VxWorks 上三个任务——高优先级总线管理任务 `bc_dist`、低优先级气象数据采集任务（ASI/MET）、中优先级通信任务。共享"信息总线"用互斥量保护，但 **VxWorks 的互斥量创建时继承开关是个布尔参数，而这把锁被关掉了**。H 等 L 的锁期间，M 抢占 L → 看门狗发现 bc_dist 超期未跑 → 整系统复位。JPL 在地面复刻机上开 VxWorks 的事件追踪（context switch/同步对象/中断全记录），凌晨复现成功；修复方式是**利用 VxWorks 自带的 C 解释器远程上传补丁**，把那把互斥量的继承参数打开——他们幸好发射时没关掉这个调试通道。这个故事的三个工程教训：① 继承必须是默认而非选配；② 可观测性（trace）比代码正确性更难事后补；③ 留一条远程修复通道在关键任务里值回票价。

**诊断**：`uxTaskGetSystemState` 快照：H=eBlocked 且等待对象是信号量；L=eReady/eRunning 但优先级低；M 长期 eRunning——三角关系确认。

**解法**：共享资源一律互斥锁（xSemaphoreCreateMutex），**二值信号量无 owner 无从继承**（6.8）。grep 代码中 `CreateBinary` 出现在共享资源上下文的每一处。

### 13.1.2 继承已开却"看似失效"：四查点

1. **保护资源的其实是 Binary 不是 Mutex**（命中率最高）——继承只存在于 `ucQueueType == queueQUEUE_IS_MUTEX`（6.2 表）。
2. **持锁任务被 vTaskSuspend**——继承提升的是就绪优先级，挂起态的锁主救不了 H（3.6）。持锁期间禁 suspend。
3. **只继承一跳**（7.6 链式反转）——反转在第二层复活。诊断：快照里 H 等的锁主 A 本身 eBlocked 在另一把锁上。解法见 7.6 天花板或重构锁层次。
4. **中断里的"隐性低优先级"**——ISR 触发 give 唤醒 L，但 ISR 不受任务优先级约束，H 要等 ISR 跑完。这是中断延迟不是反转，别误诊：检查 ISR 是否过长/未置 yield。

### 13.1.3 幽灵优先级：归还的"多锁盲区"（真实行为是"降得太迟"而非"降得太狠"）

**症状**：低优先级任务偶发以高优先级长期运行、时间片反常；或"give 完锁期望的高优先级接管"没发生。

**根因（与直觉相反的方向）**：FreeRTOS 的归还路径不是"降得太狠"，而是**降得太迟**。两条规则叠加：

1. `xTaskPriorityDisinherit`（give 路径，7.3）：只在 `uxMutexesHeld == 0`（还完最后一把锁）时才归还——持任何一把剩余锁时**完全不降**；
2. `vTaskPriorityDisinheritAfterTimeout`（超时路径，7.4）：只在 `uxMutexesHeld == 1`（恰好只剩这一把）时才调整，且只考虑**本锁**的剩余等待者。

后果：任务持多把锁时，只要还有一把没还，它就继续以历史最高继承优先级运行——**反转没有被消除，只是被推迟**了（直到最后一把锁释放）。GitHub issue #1168 的真实案例正是这个：give 后立即再 take 的同任务，因还持着另一把锁而没让位给本应接管的高优先级等待者。这不是正确性 bug（不会死锁），是**时序悲观念**——但足以让"严格按优先级到账"的假设落空。

**诊断**：给每把锁编号打日志（take/give 配对 + 任务名+优先级变化）；对比 uxBasePriority 与 uxPriority 长期不等且其持有锁已无等待者的任务——那是"挂账未清"的证据。

**规避**：多锁任务严格栈式解锁（take 顺序 A→B，give 顺序 B→A）；能合并就合并锁；对"必须立即让位"的语义在应用层补一次 taskYIELD；或切自研内核语义（重算式继承，rtos_design.md 7.4）——两边文档互证的差异点。

## 13.2 死锁：ABBA 的诊断三板斧

```
任务1: take(MutexA)✓ → take(MutexB) 阻塞(任务2持有)
任务2: take(MutexB)✓ → take(MutexA) 阻塞(任务1持有)
继承把两者互相抬到对方优先级 → 环闭合, 无人前进
```

**板斧一：双快照对比**。间隔 1s 抓两次 `uxTaskGetSystemState`：两次都 eBlocked 且 `ulRunTimeCounter` 零增长的任务集合 = 僵尸嫌疑（需开 configGENERATE_RUN_TIME_STATS）；对每个僵尸找到其等待的队列句柄（vTaskGetInfo 的 TaskStatus_t 关联）→ 画"任务→锁"有向图，**有环即死锁**。

**板斧二：超时自曝**。所有 take 一律带有限超时（哪怕 10s）+ 失败时打印（任务名、锁名/句柄、持有的另一把锁）——死锁自动浮出。这是**开发期就该埋的雷管**，不是事后补的。

**板斧三：配置断言验尸**。configASSERT 常开时死锁常伴链表断言（`uxListRemove` 内 pxContainer==NULL 断言、`configASSERT( pxUnblockedTCB )`）——崩溃现场即证据现场。

**解决与预防**：
- **全局锁序**：所有互斥锁编号，任何任务只能升序获取（拿了 3 再要 1 = 评审即毙）；
- 超时 + 补偿回滚（放弃时逆序释放已持锁，稍后重试）；
- 架构级：双锁需求审视——多数 ABBA 的解是合并粗粒度锁或改队列传递所有权（FreeRTOS 官方 pipe 模式）。

# 第十四章 数据竞争：临界区用错的形态

## 14.1 多字节共享无保护（缝合怪）

```c
/* 任务A 写 */  g_count_64 = big;         /* 两条STR */
/* 任务B/ISR 读 */ if( g_count_64 > TH )  /* 两条LDR: 可能读到新旧高低半字缝合值 */
```

**指纹**：读数偶发跳变、出现不可能值（如 0x00000001FFFF0000 型缝合）、概率与中断频率正相关。反汇编确认是否多条指令。

**解**：写侧 taskENTER_CRITICAL（防任务+可屏蔽 ISR）或 FromISR 屏蔽宏（ISR 侧）；读侧同。**volatile 只管编译器可见性/重排，不管原子性**——嵌入式最高频概念混淆，没有之一。64 位量在 32 位 MCU 上永远需要保护（或用序列锁/双读校验）。

## 14.2 TOCTOU（检查与行动之间被打）

```c
if( uxQueueMessagesWaiting( q ) > 0 )   /* 检查: 有 */
    xQueueReceive( q, &v, 0 );          /* 行动: 恰被ISR消化 → 返回空 */
```

检查与行动不是原子的。**解**：以 xQueueReceive 返回值为唯一真相（它内部临界区原子完成检查+行动，6.5 快路径）；外层检查只作提示；或 timeout=1 tick 给回填机会。自研同病同解（rtos_usb 系列的"以阻塞 API 返回值为准"原则）。

## 14.3 FromISR 三件套漏 yield

```c
void EXTI0_IRQHandler( void ) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR( sem, &woken );
    /* ★漏: portYIELD_FROM_ISR( woken ); */
}
```

**指纹**：被唤醒的高优先级任务迟迟不跑、要等下个 tick——**吞吐正常延迟抖动**（死锁是吞吐为零，此为鉴别点）。机理：6.6 的返回 pdTRUE 被 API 内部用作"请求切换"，但请求要在 ISR 尾部由 portYIELD_FROM_ISR 落实——漏掉则推迟到自然切换。

**解**：三件套纪律——`&woken` 形参 + 尾部 yield + ISR 优先级域合规（15 章）。

## 14.4 临界区时间黑洞

taskENTER_CRITICAL 内 printf/长循环/Flash 擦除 → 全部 syscall 域中断延迟飙升：串口丢字节、USB 断流、电机环抖动。**诊断**：逻辑分析仪量中断响应延迟；或 perf 统计（自研）对比。**原则**：临界区内只碰指针与计数；数据搬运、IO、慢速外设操作一律在外。

# 第十五章 ISR 优先级域错误：随机崩溃之王

## 15.1 机理（为什么"随机"）

```
任务执行到内核临界区内(就绪链正被改, BASEPRI=0x50)
  → 0~4 级 ISR 闯入(BASEPRI 屏蔽不了它!)
  → ISR 调 xQueueSendFromISR → 改就绪链(与撕到一半的链表交错)
  → 链表损坏
  → 稍后任意位置的链表断言/HardFault/调度错乱
```

**崩溃点与肇事点分离**（肇事在中断 N，崩溃在中断 M 或任意任务）——"随机"的全部来源。11.3 的 vPortValidateInterruptPriority 会在 ISR 第一次触发时就断言现行——把随机变必现，这就是它的价值。

## 15.2 症状指纹表

| 观察 | 指向 |
|---|---|
| 随机 HardFault，地址与任何代码对不上 | 域错误或栈溢出（16 章），先查域 |
| configASSERT 失败在 uxListRemove/list 宏 | 链表撕裂 → 域错误强嫌疑 |
| 崩溃概率与某外设中断频率正相关（开外设才崩） | 该外设的 ISR 优先级/FromISR 用法 |
| Debug 不崩 Release 崩 | 时序敏感（域错误经典） |
| **vPortValidateInterruptPriority 断言命中** | 确定性证据，直接锁定 ISR |

## 15.3 诊断流程（四步）

```
① 核对 configMAX_SYSCALL_INTERRUPT_PRIORITY 非 0
② 核对 NVIC 分组 NVIC_PRIORITYGROUP_4（4位抢占0位子优先级）
③ 逐个 grep HAL_NVIC_SetPriority: 调 FromISR API 的中断,
   抢占数值必须 ≥ configMAX_SYSCALL_INTERRUPT_PRIORITY
④ 检查 ISR 内是否全部用 *FromISR 变体（任务版 API 在 ISR 调用 = 逆向灾难）
```

**预防**：configASSERT 常开（至少 Debug 构建常开）。自研用户注意：rtos_config.h 的 MAX_SYSCALL=5 同构，USB 中断已配 6（rtos_design.md 13.5.1），**每新增一个用内核 API 的中断就跑一遍这张表**。

# 第十六章 栈溢出：定位与防御

## 16.1 发病机理与症状对照

栈向下生长越界 → 践踏**相邻内存**（链接顺序决定受害者）：

| 跃迁物 | 症状 |
|---|---|
| 相邻任务 TCB | 优先级/链表指针错乱 → 调度怪异、链表断言 |
| 相邻任务 pxTopOfStack | 该任务恢复时 PC 飞天 → HardFault INVSTATE |
| 函数指针/跳转表 | 间接调用跳到数据地址（bit0=0）→ INVSTATE |
| 局部大数组下溢 | 只坏数据不崩 → **功能偶发错乱（最阴险）** |

与 15 章域错误的鉴别：**栈溢出与特定函数调用路径相关（栈深时）、与中断频率弱相关；域错误正相反**。

## 16.2 两道防线

```c
/* FreeRTOSConfig.h */
#define configCHECK_FOR_STACK_OVERFLOW  2     /* 方法2: 最强 */

/* 检查点: 每次上下文切换(4.3) —— 白嫖调度不变量 */
void vApplicationStackOverflowHook( TaskHandle_t xTask, char * pcTaskName )
{
    /* pcTaskName 直接点名肇事任务 —— 定位到此结束 */
    log( "OVERFLOW: %s", pcTaskName );
    复位或挂起;
}
```

- **方法 1**：pxTopOfStack 越出 [栈底, 栈顶] 区间——抓"已经爆了"；
- **方法 2**：栈底 16 字染料 0xA5A5A5A5（创建时，3.2 步 e）被改动即报——抓"**碰到过底**"的未遂事件。必须用方法 2：溢出瞬间栈又收回（深度函数返回了）的话方法 1 扑空，染料的伤痕是永久的。

## 16.3 尺子：高水位普查

```c
UBaseType_t remain = uxTaskGetStackHighWaterMark( hTask );  /* 单位: 字 */
/* 从栈底扫首个非 0xA5 字 → 历史最深剩余 */
```

发布前全任务普查，剩余 < 25% 加栈。**已知大胃王**：printf 家族（newlib vfprintf 500~1500 字节，浮点翻倍）、局部大数组、递归。ISR 嵌套也在任务栈上压帧（硬件 8 字 + 可能 FPU 26 字/层）——高频深嵌套中断的任务多留 100+ 字。

**定位流程**：开方法 2 + Hook → 复现 → Hook 点名 → 高水位量深度 → 增量（printf 换极简格式化/局部数组转 static/递归转迭代）。自研对照：stack_magic + 染料水位（rtos_design.md 2.2/12.3），ft8 测试（main.c）是"SP 低于栈底"活体标本。

# 第十七章 程序崩溃：HardFault 分流表

寄存器级诊断（CFSR 逐位、栈帧归因、任务/ISR 判定）见 [hardfault_debug.md](hardfault_debug.md)——Cortex-M 层与 RTOS 无关，两边通用。此处给**结合 FreeRTOS 特性**的分流：

| CFSR 特征 | 首要怀疑 | 验证动作 |
|---|---|---|
| IMPRECISERR, PC 不可信 | 15 章域错误 / 16 章栈践踏写 | 查 ISR 优先级表 + 开栈 Hook |
| INVSTATE, 栈帧 PC bit0=0 | 跳进数据区：栈毁的函数指针 | 高水位普查全任务 |
| PRECISERR + BFAR 指外设 | ISR 访问未开时钟外设 | RCC 使能顺序 |
| IBUSERR @0x0 附近 | 空函数指针：悬空任务句柄再用（22 章） | 句柄审计 |
| UFSR.DIVBYZERO | DIV_0_TRP 开启下的除零 | 栈帧 PC 直接读 |
| 无 CFSR / 栈帧烂 | 栈溢出毁 EXC_RETURN | 方法 2 Hook |

**FreeRTOS 专属线索**：崩溃时 `pxCurrentTCB` 与被毁 TCB 的关系；`uxCriticalNesting != 0` 时崩溃（临界区内出事 → 强指 15 章闯入）；链表断言先于 HardFault（configASSERT 常开的红利）。

# 第十八章 IPC 问题：丢失唤醒与虚假唤醒

## 18.1 调度器挂起期间的丢失唤醒

```c
/* 反模式: */
vTaskSuspendAll();
{
    /* 区间内: ISR 的队列 give 正常计数(cTxLock 锁定记账, 6.7),
       但唤醒推迟到 ResumeAll —— 用户在这区间手写"轮询 flag + 手工等待"
       的逻辑时, 时序假设全灭 */
    while( my_flag == 0 ) { ... }   /* 死等: 谁来置? 唤醒全被记账冻结 */
}
xTaskResumeAll();
```

**纪律**：SuspendAll 区间只做纯数据操作（不许检查-阻塞逻辑）；需要原子区间的 IPC 语义本身用队列的原子性表达。4.8 表的三类延迟记账是 FreeRTOS 挂起机制的全部坑源——**能不挂就不挂**。

## 18.2 虚假唤醒与"醒来≠成功"

FreeRTOS 的重查模式（6.5）意味着：**被 give 唤醒后数据可能又被第三者抢走**——醒来发现队列空、循环再睡。这不是 bug 是契约。

**用户侧错误姿势**（把唤醒当成功）：

```c
/* 反模式: */
xSemaphoreTake( sem, portMAX_DELAY );
use( *shared_data );     /* 假设醒来时条件仍成立 —— 中间可能被打 */
```

**正解**：醒来后**重查业务条件**（不只信号量，还有数据本身）：

```c
for( ;; ) {
    if( xSemaphoreTake( sem, pdMS_TO_TICKS(100) ) == pdTRUE ) {
        taskENTER_CRITICAL();
        if( data_valid ) { use(...); data_valid = 0; taskEXIT_CRITICAL(); break; }
        taskEXIT_CRITICAL();
    } else continue / 处理超时;
}
```

多读者/多写者的队列场景同理：xQueueReceive 返回 pdTRUE 也要校验内容语义（幂等设计）。

## 18.3 顺序问题：等待链优先级序的副作用

队列等待链按优先级唤醒（6.6 链首）——**同优先级 FIFO、跨优先级抢跳**。若业务依赖"先等先得"（公平性），如流水线传感器→算法→执行器全同优先级时天然保序；一旦中间某级优先级抬高，低优先级前端的数据可能被高优先级后端"抄近道"——**数据依赖与优先级倒挂**。诊断：给消息打序号，消费端校验单调性。解法：关键路径统一优先级，或改队列链（前级输出直接作为后级输入的专用队列，不经共享队列）。

# 第十九章 STM32 平台专属问题

## 19.1 SysTick 双主之争（HAL 时基冲突）

**症状**：上 FreeRTOS 后 `HAL_Delay()` 卡死 / HAL 驱动超时逻辑失效 / 在 HAL 内部死循环。

**根因**：CubeMX 默认 HAL 用 SysTick 计时（HAL_IncTick）；FreeRTOS port 也要 SysTick（11.4 步 3/4 把它占为最低优先级并接管节拍）。SysTick 中断一被 BASEPRI 屏蔽，uwTick 停走，`HAL_Delay` 的 `while(HAL_GetTick()-t<ms)` 永不满足。

**解**（CubeMX 标准姿势）：`SYS → Timebase Source → TIM6`（或任一基本定时器）→ 生成 `stm32f4xx_hal_timebase_tim.c` → HAL_IncTick 挂 TIM6，SysTick 归 FreeRTOS 独享。TIM6 中断优先级也应在 syscall 域（若其 ISR 会调内核 API——通常不会）。

**自研同构**：本工程自研内核同样占 SysTick（rtos_design.md 3.6 ④）；main.c 的 HAL_Delay 未走阻塞路径侥幸无事——但**任何新引入的 HAL 超时 API（如 HAL_UART_Transmit 的 timeout）都踩同一雷**，原则同：HAL 时基独立成 TIM。

## 19.2 时钟与 USB 的 48MHz

USB OTG FS 的 CLK48 必须精确 48MHz（PLLQ）。跑 FreeRTOS 后若动态调频（tickless/低功耗降压），CLK48 跟着 HSE/PLL 变 → USB 枚举失败/随机断流。**诊断**：断流与功耗模式切换时间点相关。**解**：调频前停 USB；或 CLK48 独立源（F446 仅有 PLLQ/ PLLSAI，注意约束）。本工程 rtos_design.md 13.5.1 的 CLK48=PLLQ 固定配置即规避此坑。

## 19.3 低功耗与 tickless

configUSE_TICKLESS_IDLE 下 WFI 前必须：预测唤醒点（含定时器到期）、停 SysTick、补偿丢失 tick（portSUPPRESS_TICKS_AND_SLEEP 钩子）。STM32 Stop 模式还丢 PLL/HSE——唤醒后要重新锁相再继续跑，漏一步 = 唤醒后逻辑时钟全乱。**建议**：初学者不用 Stop 级，只用 WFI 级 tickless（自研 idle_hook 默认 WFI 同级，零风险）。

# 第二十章 newlib 与 printf 三连坑

## 20.1 栈坑

newlib 全功能 vfprintf 栈 500~1500 字节（浮点翻倍）。多任务并发 printf：**每个可能 printf 的任务**的栈都要装下它。诊断：16 章高水位普查调用 printf 的任务。解：换极简格式化（自研 main.c 的 itm_printf + 128B 静态缓冲即此思路）；或 printf 收敛到单一日志任务。

## 20.2 重入坑（malloc 撕裂）

printf 内部 malloc（行缓冲）→ 两任务同时 printf → 同时 malloc → **newlib 堆元数据撕裂** → 随机崩溃，且崩溃点在 malloc 内部（与 15 章域错误的鉴别：域错误崩在链表宏，重入崩在 sbrk/malloc 内部）。

**选项**：
- printf 前后套专用互斥锁（牺牲并发，最简单）；
- heap_3（包装 newlib malloc + `__malloc_lock` 互斥实现）——注意 heap_3 与 heap_4 是**两个独立的堆**：勾 heap_4 只管内核 pvPortMalloc，newlib printf 仍走自己的 sbrk 弱符号堆；
- `configUSE_NEWLIB_REENTRANT=1`（每任务独立 _reent，代价：TCB 增大 + 需实现锁）。

## 20.3 半主机坑

Debug 半主机（BKPT 0xAB）在无调试器 attached 时**直接 HardFault**。发布烧录后"一跑 printf 就崩"的经典。解：发布构建 `-specs=nosys.specs` + 不定义 semihosting；_write 重定向到 UART/ITM（本工程 syscalls.c 已做）。指纹：HardFault 是 HardFault exception 而非 UsageFault，栈帧 PC 停在 BKPT 指令处。

# 第二十一章 定时器与回调问题

## 21.1 回调阻塞：全系统定时器集体罢工

机理（9.3）：所有回调串行于守护任务。任一回调 `vTaskDelay(100)`/阻塞 take → **其余全部定时器停摆 100ms**。

**指纹**：多个不相关定时器同时失灵、恢复时间一致（"集体罢工"）；守护任务 CPU 占用异常（vTaskGetRunTimeStatistics）。

**解**：回调只做轻处理——重活经队列转交工作任务（本工程 USB MSC 正是此原则的规模化应用：ISR 投事件、线程干活，rtos_design.md 13.1），或用 `xTimerPendFunctionCall` 直接把函数指针挂到守护任务。守护任务栈按"最深回调之和"配：内核不给 `configTIMER_TASK_STACK_DEPTH` 默认值（必须由用户在 FreeRTOSConfig.h 定义），演示工程里常取 `configMINIMAL_STACK_SIZE * 2` 字——回调里一旦 printf 立刻不够（20 章）。

## 21.2 定时器命令的延迟生效

xTimerStart 走命令队列（9.1）：生效延迟 = 入队 + 守护任务被调度。**指纹**：定时器启动比预期晚 0~1 个守护调度周期（微秒~毫秒级）。对"启动必须精确对齐 tick"的场合：改用 vTaskDelayUntil 在任务里做，或接受并补偿固定延迟。**ISR 里调 xTimerStart（非 FromISR 版）是域错误的变种**（15 章 ④ 步骤检查项）。

## 21.3 回调里删除定时器 / 创建定时器

回调运行于守护任务——回调里 xTimerCreate 再 Start：新定时器命令入队，守护任务**正在回调里**，队列处理要等回调返回——若回调又在等这个新定时器 → **自死锁**。纪律：回调里不动定时器拓扑（创建/删除/改周期转交任务侧）。

# 第二十二章 任务删除与悬空句柄

## 22.1 三种死法

**① 句柄悬空**：vTaskDelete(A) 后 A 的句柄仍被别处 xTaskNotify/xQueueSend 引用 → 操作已 free 的 TCB → 17 章的"IBUSERR@0x0"或随机崩溃。**纪律**：删除权与句柄引用权归同一模块；删除后广播失效（全局置 NULL / 引用计数）；优先用"常驻+通知控制启停"替代删除。

**② 持锁被删**（FreeRTOS 独有）：A 持 mutex 被 vTaskDelete（3.4 ②只摘链**不释放锁**）→ pxMutexHolder 指向已 free 的 TCB → 下一个 take 走 vTaskPriorityInherit 读 holder->uxBasePriority → **野指针解引用**（崩溃或锁永久假死）。**解**：任务退出协议——删除前由它自己 give 全部持有的锁再自删；或外部删除前先发"退出请求"消息、等它清理完自删。**自研内核无此坑**（mutex_release_all 自动移交，rtos_design.md 7.5）——两边文档互证的分歧点。

**③ 临界区内自删**：vTaskDelete(NULL) 在持锁状态 → 同②；且 uxCriticalNesting 非零时任务消失 → **临界区计数永久泄漏** → 此后 portEXIT_CRITICAL 永不开中断 → 全系统假死。自删前归位全部资源与临界区状态。

## 22.2 删除的替代方案（按优先级）

1. 任务常驻 + 任务通知控制启停（零删除、零悬空）；
2. vTaskSuspend（可逆，但持锁挂起 = 13.1.2 查点 2）；
3. 真要删：目标任务的"退出协议"（收退出命令 → 清资源 → 自删）——删除权留在任务自己手里。

# 第二十三章 排障工具箱与配置基线

## 23.1 FreeRTOSConfig.h 排障基线（Debug 构建常开）

```c
#define configASSERT( x )  if( ( x ) == 0 ) vAssertCalled( __FILE__, __LINE__ )
#define configCHECK_FOR_STACK_OVERFLOW     2     /* 16章 */
#define configUSE_MALLOC_FAILED_HOOK       1     /* 分配失败可见化 */
#define configGENERATE_RUN_TIME_STATS      1     /* CPU 统计时基(另配 portCONFIGURE_TIMER_FOR_RUN_TIME_STATS) */
#define configUSE_TRACE_FACILITY           1     /* 任务状态快照所需 */
#define configUSE_STATS_FORMATTING_FUNCTIONS 1   /* vTaskList 文本输出 */
```

vAssertCalled 里打印文件/行/当前任务名（`pcTaskGetName(NULL)`）→ 延时 → 复位。**这是把 15/17/21 章的"随机"变"必现"的总开关**。

## 23.2 三件诊断武器

| 武器 | 用法 | 抓什么 |
|---|---|---|
| uxTaskGetSystemState 快照 | 间隔抓两次对比 | 死锁（13.2 板斧一）、饿死、僵尸 |
| uxTaskGetStackHighWaterMark | 发布前普查 | 栈余量（16.3） |
| vTaskGetRunTimeStatistics | 文本表 | 守护任务冻结（21.1）、CPU 热点 |

## 23.3 排障方法论三条（贴显示器）

1. **崩溃点不是肇事点**——域错误（15）与栈溢出（16）都以远处随机崩溃露头。先做确定性防御（configASSERT + 栈 Hook + ValidateInterruptPriority），把随机变必现，再定位；
2. **诊断靠快照与染料**——双快照对比抓死锁、高水位染料抓栈、CPU 统计抓冻结。不猜，量；
3. **临界区与 ISR 域是一体的安全边界**——BASEPRI 两侧（任务侧临界区纪律、中断侧优先级配置）任一侧失守，另一侧的全部承诺作废。每加一个用内核 API 的中断，跑一遍 15.3 的四步表。

---

## 结语

FreeRTOS 与本工程自研内核在核心机制上的高度收敛——位图调度（CLZ/CTZ 互为镜像）、PendSV 两级现场与 EXC_RETURN 自描述、双链登记与"先到者全摘"、优先级继承三函数、守护任务架构、idle 延迟回收自删任务——说明 Cortex-M 抢占式 RTOS 的解空间在强约束下存在局部最优；两个独立实现互为对方正确性的交叉验证。

分歧点则各擅胜场：FreeRTOS 的**双链对滚**（回绕免疫）与 **ValidateInterruptPriority**（ISR 域断言）值得移植进自研内核；自研的**重算式优先级继承**、**删除持锁自动移交**、**唤醒-获取原子性**（所有权预移交）则是 FreeRTOS 已知债务的正确解。排障二十章的三大重灾区（域错误、栈溢出、删除悬空）中，FreeRTOS 原生防御覆盖前两者，第三者靠工程纪律——而纪律的本质，是把"谁拥有什么、谁何时删除"写进设计而不是留给运气。

---

*分析基准：FreeRTOS Kernel V10.4.6 LTS + ARM_CM4F(GCC)，全部代码走读与官方源码逐段核对 | 对照基准：本工程 rtos_design.md | 寄存器级诊断以 [hardfault_debug.md](hardfault_debug.md) 为准*
