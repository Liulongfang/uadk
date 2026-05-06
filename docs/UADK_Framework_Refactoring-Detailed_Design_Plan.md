# UADK 框架重构：详细设计方案

> 日期: 2026-05-06 (根据当前代码实现刷新)
> 基于: 当前 `uadk/` 代码库（不含 v1/）实现状态
> 范围: 框架层 (wd_alg.c, wd_util.c, headers) + API 层 (wd_cipher.c 等) + 驱动层 (drv/*.c)

**说明**: 本文档已根据 patch 合并后的实际代码实现进行了刷新。文档中标注 "已实现" 的内容均可在源代码中验证（含文件:行号引用）。标注 "设计预留" 的内容为原始设计方案中提出但尚未实现的特性。

---

## 第一章 问题定义与设计约束

### 1.1 十个核心问题

> **当前状态 (2026-05-06)**: 根据当前代码实现重新评估，描述解决方式及实现位置。

**层级一：路径融合 (P1-P5)**

| #   | 问题                                            | 根因                                       | 实现方式      |
| --- | --------------------------------------------- | ---------------------------------------- | ------- |
| P1  | init1 和 init2 是两套完全独立的初始化逻辑，需融合为一             | 历史遗留：init1 为 hisi_sec2 单设备设计，init2 为扩展新增 | 已实现：通过 `wd_alg_attrs_init()` 统一入口，api 层区分 V1/V2 调用方式 |
| P2  | init1 入参已含用户预分配的 ctx 队列和调度器，不需框架内部分配          | init1 的 ctx 所有权归调用者                      | 已实现：V1 路径在 api 层直接使用用户传入的 config/sched，通过 `wd_ctx_bind_drivers()` + `wd_alg_init_driver()` 完成初始化 |
| P3  | init2 只传算法名称和 ctx 数量，需框架内部完成驱动发现→ctx 分配→调度器创建 | init2 设计目标是框架接管整个初始化流程                   | 已实现：V2 路径通过 `wd_alg_attrs_init()` (wd_util.c:2699) 完成 Phase 1-2，api 层补充 Phase 2.5-3 |
| P4  | 融合后应统一走 `wd_get_drv_array` → 分配 → 注入调度器流程     | 当前两条路径的设备扫描逻辑互不共享                        | 已实现：v1 和 v2 都调用 `wd_get_drv_array()` (wd_alg.c:673) 进行驱动发现 |
| P5  | 两条路径入参格式不同，融合需要桥接                             | 参数语义不对齐                                  | 已实现：api 层各自将 v1/v2 参数转换为 `wd_init_attrs` 结构后调用统一框架接口 |

**层级二：多驱动兼容 (P6-P7)**

| #   | 问题                                                | 根因                 | 实现方式    |
| --- | ------------------------------------------------- | ------------------ | ----- |
| P6  | init1 硬编码 hisi_sec2，多驱动环境下需约束 init1 只匹配 hisi_sec2 | init1 API 不存在多驱动概念 | 已实现：v1 路径调用 `wd_get_drv_array("cipher", TASK_HW, "hisi_sec2", ...)` 来过滤特定驱动 |
| P7  | 不同驱动支持的算法子集不同，session 创建时需检查已分配 ctx 的驱动是否兼容目标算法   | 框架层此前不具备算法兼容性感知能力  | 已有方案：通过 `wd_sched_set_param()` + `wd_alg_match_drv()` 在调度器层实现 session 级别的 compat ctx 过滤，详见 5.4 节 |

**层级三：ctx 分配时机 (P8-P10)**

| #   | 问题                                             | 根因                        | 实现方式      |
| --- | ---------------------------------------------- | ------------------------- | ------- |
| P8  | init 时分配 ctx 会导致后续 session 算法可能不被已分配 ctx 的驱动支持 | init 只知道算法类型，不知道具体算法      | 已实现：通过 `wd_alg_ctx_init()` (wd_util.c:2490) 在 init 时按 op_type 分配 ctx，依赖调度器在 session 时选择合适的 ctx |
| P9  | 延迟分配无法实现多 session 共享 ctx 池和多设备算力叠加             | 延迟分配每 session 独立 ctx，无法叠加 | 已实现：v2 路径通过 RR 方式将 ctx 分散到多个驱动上，多 session 共享同一 ctx 池 |
| P10 | 需要保证数据结构和内存生命周期                                | 数据结构太多，需要分配内存             | 已实现：`wd_alg_ctxs_uninit()` (wd_util.c:2605) 负责反初始化，`wd_ctx_unbind_drivers()` (wd_util.c:2198) 清理驱动绑定 |

### 1.2 五个设计目标

| 目标  | 描述                                        | 实现状态      |
| --- | ----------------------------------------- | ------- |
| G1  | init1 继续支持 hisi 设备，init2 扩展支持多设备多驱动异构算力叠加 | 已达成：cipher/digest/aead v1 路径完整实现，v2 支持多驱动 rr 绑定 |
| G2  | 支持未来 PCIe/GPU/NPU 等新设备类型，同设备多实体共用一个驱动     | 架构已就绪：`wd_alg_driver` 的 `calc_type` 枚举已预留 UADK_ALG_NPU/GPU，`alloc_ctx`/`free_ctx` 回调机制支持扩展 |
| G3  | init1 和 init2 尽可能复用函数接口，减少冗余代码            | 已达成：v1/v2 共享 `wd_get_drv_array()`、`wd_ctx_bind_drivers()`、`wd_alg_init_driver()` 等框架函数 |
| G4  | init 时按算法类型匹配驱动，session 时按具体算法匹配 ctx      | 已有方案：init 阶段驱动匹配已实现；session 级别的算法兼容路由方案已制定（见 5.4 节），按 coral.md 方案实施 |
| G5  | 对外 API 接口不变，内部函数和接口可修改                    | 已达成：`wd_cipher_init()`、`wd_cipher_init2_()` 等对外接口签名均未改变 |

### 1.3 设计约束

**约束 C1 (init1 路径)**: init1 路径只需要支持固定的 hisi 硬件设备，不需要支持多设备多驱动的异构混合计算能力。V1 用户预分配 ctx 时只申请 hisi_sec2 的硬件队列。

**约束 C2 (init2 路径)**: init2 路径需要支持多类型设备 (HW/CE/SVE/SOFT) 和多同类型设备 (如多个 hisi_sec2 实例) 的算力叠加，实现异构混合加速。

**约束 C3 (外部 API 不变)**: 对外 API 函数签名不可变。`wd_cipher_init(config, sched)` 和 `wd_cipher_init2_(alg, sched_type, task_type, ctx_params)` 保持不变。

---

## 第二章 架构总览

### 2.1 统一初始化流水线

V1 和 V2 路径共享相同的框架函数，但执行流程不同。`wd_alg_attrs_init()` (wd_util.c:2699) 是 V2 路径的核心统一入口，执行 Phase 1 (驱动发现) + Phase 2 (ctx 分配)。V1 路径由于 ctx 已由用户预分配，跳过 Phase 2 的 `wd_alg_attrs_init()`，直接调用 `wd_ctx_bind_drivers()` + `wd_alg_init_driver()`。V1/V2 的区分由 API 层调用不同的框架函数实现，而非通过统一标志位。

```
┌─────────────────────────────────────────────────────────────────────┐
│                     统一初始化流水线 (当前实现)                       │
│                                                                      │
│  API 层 (wd_cipher.c)                                                │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │ wd_cipher_init()   → V1: 调用 wd_get_drv_array() +            │   │
│  │                           wd_ctx_bind_drivers() +             │   │
│  │                           wd_alg_init_driver()               │   │
│  │ wd_cipher_init2_() → V2: 调用 wd_alg_attrs_init() +          │   │
│  │                           wd_ctx_bind_drivers() +             │   │
│  │                           wd_alg_init_driver()               │   │
│  └──────────────────────────┬───────────────────────────────────┘   │
│                             │                                        │
│  ┌──────────────────────────┴───────────────────────────────────┐   │
│  │  框架层 (wd_util.c)                                             │   │
│  │                                                                │   │
│  │  wd_alg_attrs_init(&attrs) — V2 统一入口 (wd_util.c:2699)     │   │
│  │  ├── Phase 1: wd_alg_drv_discover() (wd_util.c:2311)          │   │
│  │  │     → wd_get_alg_type() 映射 alg_name → alg_type           │   │
│  │  │     → wd_get_drv_array() 扫描注册表 (wd_alg.c:673)         │   │
│  │  │       V1: drv_name="hisi_sec2" → [hisi_sec2 cipher]       │   │
│  │  │       V2: drv_name=NULL → [hisi_sec2, isa_ce_sm4, ...]    │   │
│  │  └── Phase 2: wd_alg_ctx_init() (wd_util.c:2490)              │   │
│  │        → RR 分配 ctx (ctx_idx % drv_count = drv)              │   │
│  │        → drv->alloc_ctx() 分配硬件/软件队列                    │   │
│  │        → 分配调度器 + 注入 ctx 范围                             │   │
│  │        → alg_init 回调 (模块内部拷贝)                          │   │
│  │                                                                │   │
│  │  [Phase 2.5: 由 API 层调用]                                    │   │
│  │  wd_ctx_bind_drivers(config_internal, drv_array, count)        │   │
│  │    → RR 绑定 ctxs[i].drv (wd_util.c:2229)                     │   │
│  │    → 存储 drv_array 到 config (wd_util.c:2268)                │   │
│  │    → 递增驱动引用计数                                            │   │
│  │                                                                │   │
│  │  [Phase 3: 由 API 层调用]                                      │   │
│  │  wd_alg_init_driver(config) (wd_util.c:1650)                   │   │
│  │    → 遍历 ctxs, 调用 wd_ctx_init_driver()                     │   │
│  │    → drv->init(config, priv) (初始化硬件队列)                  │   │
│  └──────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.2 V1 与 V2 分化点 (当前实现)

| 阶段        | V1 行为 (由 api 层 wd_cipher_init 控制)                              | V2 行为 (由 wd_alg_attrs_init 统一处理)         |
| --------- | ---------------------------------------------------------------- | ------------------------------------------ |
| Phase 1   | `wd_get_drv_array("cipher", TASK_HW, "hisi_sec2", ...)` → 只返回 hisi_sec2 | `wd_alg_drv_discover()` → `wd_get_drv_array("cipher", task_type, NULL, ...)` → 返回全部匹配驱动 |
| Phase 2   | 跳过 — ctx 已由用户预分配 (wd_ctx_config 已存在)                          | `wd_alg_ctx_init()` 执行 RR 分配 → `drv->alloc_ctx()` 分配队列 |
| Phase 2.5 | `wd_ctx_bind_drivers()` RR 绑定 ctxs[i].drv = hisi_sec2             | `wd_ctx_bind_drivers()` RR 绑定，支持混合驱动          |
| Phase 3   | `wd_alg_init_driver()` → `hisi_sec_init()` (HW QP 分配)              | `wd_alg_init_driver()` → 各驱动 `init()` 遍历执行    |

---

## 第三章 数据结构设计

### 3.1 数据可见性分层

```
┌──────────────────────────────────────────────────────────────────┐
│  API 层 (wd_cipher.c) — 模块全局单例                              │
│                                                                   │
│  struct wd_cipher_setting {                                       │
│      struct wd_ctx_config_internal config;  // 内部 ctx 配置      │
│      struct wd_sched sched;                 // 调度器             │
│      struct wd_async_msg_pool pool;         // 异步消息池         │
│      ...                                                          │
│  } wd_cipher_setting;                                             │
│                                                                   │
│  struct wd_cipher_sess {                                          │
│      const char *alg_name;   // "ecb(aes)", "ctr(sm4)", ...       │
│      void *sched_key;        // 调度器会话 key                     │
│      unsigned char *key;     // 密钥                              │
│      ...                                                          │
│  };                                                               │
├──────────────────────────────────────────────────────────────────┤
│  框架层 (wd_util.c + wd_alg.c + headers)                          │
│                                                                   │
│  struct wd_init_attrs {         // ★ 统一初始化参数载体           │
│      __u32 sched_type;          // 调度策略                       │
│      __u32 task_type;           // TASK_HW / TASK_MIX / TASK_INSTR│
│      char alg[CRYPTO_MAX_ALG_NAME];  // 算法名                    │
│      struct wd_sched *sched;          // 调度器 (v1 传入, v2 内建) │
│      struct wd_ctx_params *ctx_params; // ctx 参数                 │
│      struct wd_ctx_config *ctx_config;   // ctx 配置 (v2 内分配)   │
│      wd_alg_init alg_init;                // 模块回调              │
│      wd_alg_poll_ctx alg_poll_ctx;        // 模块回调              │
│      struct wd_ctx_config_internal *ctx_config_internal; // ★ 内部拷贝│
│      struct wd_alg_driver **drv_array; // ★ Phase 1 输出          │
│      __u32 drv_count;           // ★ Phase 1 输出                   │
│  };                                                               │
│  // 说明：is_v1_path 和 drv_name_filter 是设计中提出的概念但未实际存在于 wd_init_attrs 结构体中。    │
│  // V1/V2 分化由 api 层直接控制对框架函数的调用方式。               │
│                                                                   │
│  struct compat_ctx_set {       // 设计预留: session 兼容 ctx 集合 │
│  // 尚未在当前代码中实现。session 算法兼容性路由取决于调度器扩展。   │                                                               │
├──────────────────────────────────────────────────────────────────┤
│  驱动注册层 (wd_alg.h + wd_alg.c)                                 │
│                                                                   │
│  struct wd_alg_driver {         // 驱动抽象 (当前实现)            │
│      const char *drv_name;      // "hisi_sec2", "isa_ce_sm4", ... │
│      const char *alg_name;      // "ecb(aes)", "cbc(aes)", ...    │
│      int calc_type;             // UADK_ALG_HW/CE_INSTR/SVE_INSTR/SOFT│
│      int priority;              // 100 (HW), 400 (CE), 0 (SOFT)   │
│      int queue_num;             // 队列数量                        │
│      int op_type_num;           // 操作类型数量                    │
│      int priv_size;             // 驱动私有数据大小                │
│      int *drv_data;             // 驱动数据指针                    │
│      handle_t fallback;         // 软算回退驱动                    │
│      int init_state;            // 初始化状态                      │
│      int (*init)(void*,void*);  // QP 分配                        │
│      void (*exit)(void*);       // QP 释放                        │
│      int (*send)(handle_t,void*);                                 │
│      int (*recv)(handle_t,void*);                                 │
│      int (*get_usage)(void*);   // 获取使用率                      │
│      int (*get_extend_ops)(void*); // 获取扩展操作                  │
│      int  (*alloc_ctx)(char*, void*, handle_t*); // ★ 分配 ctx    │
│      void (*free_ctx)(handle_t); // ★ 释放 ctx                    │
│  };                                                               │
│  // 说明：设计中提出的 need_lock, dev_cache, init_refcnt 字段         │
│  // 未在最终实现中采用。引用计数通过注册表节点的 refcnt 管理。              │
│  struct wd_alg_entry {											│
│		char alg_name[ALG_NAME_SIZE];								│
│		bool available;												│
│	};																│
│																	│
│  struct wd_drv_node {												│
│		char drv_name[DEV_NAME_LEN];								│
│		char alg_type[ALG_NAME_SIZE];								│
│		int priority;												│
│		int calc_type;												│
│		int refcnt;													│
│		struct wd_alg_driver *drv;									│
│		struct wd_alg_entry algs[MAX_DRV_ALG_NUM];					│
│		int alg_count;												│
│		struct wd_drv_node *next;									│
│   };                                                              │
├──────────────────────────────────────────────────────────────────┤
│  内部 ctx 层 (wd_internal.h)                                      │
│                                                                   │
│  struct wd_ctx_h {              // HW ctx (UACCE 设备队列)         │
│      __u8 ctx_type;             // ★ NEW 首字段: UADK_CTX_HW(0)   │
│      int fd;                    // /dev/hisi_sec2-X 文件描述符     │
│      char dev_path[MAX_DEV_NAME_LEN];                             │
│      struct uacce_dev *dev;     // 设备指针                       │
│      void *priv;                // QP handle                      │
│      ...                                                          │
│  };                                                               │
│                                                                   │
│  struct wd_soft_ctx {           // 软算/CE/SVE ctx                 │
│      __u8 ctx_type;             // ★ NEW 首字段: CE_INS/SVE_INS/SOFT│
│      int fd;                    // -1 (标记非文件描述符)           │
│      pthread_spinlock_t slock;  // 发送锁                         │
│      pthread_spinlock_t rlock;  // 接收锁                         │
│      struct wd_soft_sqe qfifo[1024]; // 软队列                    │
│      __u32 head, tail, run_num;                                   │
│      void *priv;                                                 │
│  };                                                               │
│                                                                   │
│  struct wd_ce_ctx {             // CE 指令 ctx                     │
│      __u8 ctx_type;             // ★ NEW 首字段: UADK_CTX_CE_INS  │
│      int fd;                                                      │
│      char *drv_name;                                             │
│      void *priv;                                                 │
│  };                                                               │
│                                                                   │
│  struct wd_ctx_internal {       // 内部 ctx 描述符                 │
│      __u8 op_type;              // 操作类型 (加密/解密)            │
│      __u8 ctx_mode;             // CTX_MODE_SYNC / ASYNC           │
│      __u8 ctx_type;             // HW/CE_INS/SVE_INS/SOFT          │
│      __u8 ctx_used;                                              │
│      handle_t ctx;              // → wd_ctx_h / wd_soft_ctx / wd_ce_ctx│
│      __u16 sqn;                 // 调度器序列号                    │
│      pthread_spinlock_t lock;   // ctx 访问锁                      │
│      struct wd_alg_driver *drv; // ★ Phase 2.5 绑定                │
│      void *extend_ops;                                           │
│      void *drv_priv;            // 驱动私有数据 (如 hisi_sec_ctx)  │
│  };                                                               │
│                                                                   │
│  struct drv_region {            // 设计预留: 驱动分组元数据       │
│  // 未在当前代码中实现。驱动区域信息通过 config->drv_array 隐式管理。│
│                                                                   │
│  struct wd_ctx_config_internal { // ★ 内部 ctx 配置 (wd_internal.h:45)│
│      __u32 ctx_num;             // ctx 总数                        │
│      int shmid;                 // 共享内存 id (dfx 计数器)        │
│      struct wd_ctx_internal *ctxs; // ctx 数组                     │
│      void *priv;                                                 │
│      bool epoll_en;                                              │
│      unsigned long *msg_cnt;    // 消息计数 (共享内存)             │
│      char *alg_name;            // "cipher", "digest", ...        │
│      struct wd_alg_driver **drv_array; // ★ 驱动数组 (Phase 2.5 设置) │
│      __u32 drv_count;           // ★ 驱动数量                       │
│  };                                                               │
│  // 设计中提出的 drv_region[] 和 drv_region_count 字段未实施。     │                                                               │
└──────────────────────────────────────────────────────────────────┘
```

### 3.2 新增枚举值

```c
// wd_alg_common.h — ctx 属性 (已有，无需修改，仅确认一致)
enum wd_ctx_property {
    UADK_CTX_HW       = 0x0,  // 硬件加速 ctx (hisi_sec2)
    UADK_CTX_CE_INS   = 0x1,  // ARM CE 指令 ctx (isa_ce_sm4)
    UADK_CTX_SVE_INS  = 0x2,  // ARM SVE 指令 ctx (isa_sve)
    UADK_CTX_SOFT     = 0x3,  // 纯软件 ctx
    UADK_CTX_MAX
};

// wd_alg_common.h — 计算类型 (已有，用于驱动 calc_type 字段)
enum alg_priority {
    UADK_ALG_HW       = 0x0,
    UADK_ALG_CE_INSTR = 0x1,
    UADK_ALG_SVE_INSTR = 0x2,
    UADK_ALG_SOFT     = 0x3
};

// wd_util.h / wd_drv.c — ctx 分配参数 (NEW)
struct wd_drv_ctx_params {
    __u8  ctx_mode;    // CTX_MODE_SYNC / CTX_MODE_ASYNC
    __u8  op_type;     // 操作类型 (如 0=加密, 1=解密)
    __u32 numa_id;     // NUMA 节点亲和
    void *dev;         // NULL=自动选择, 非NULL=显式指定设备
};
```

---

## 第四章 框架层详细设计

### 4.1 文件总览

```
框架层涉及的文件:
  include/wd_alg_common.h — wd_init_attrs, wd_ctx, wd_ctx_config, wd_ctx_params 定义
  include/wd_alg.h       — wd_alg_driver, wd_drv_node, wd_drv_ctx_params 定义
  include/wd_internal.h  — wd_ctx_internal, wd_ctx_config_internal 定义
  include/wn_util.h      — 框架辅助函数声明

  wd_alg.c               — 驱动注册表 (wd_alg_driver_register) + Phase 1 驱动发现 (wd_get_drv_array)
  wd_util.c              — init 流水线核心 (wd_alg_attrs_init, wd_alg_ctx_init, wd_ctx_bind_drivers, wd_alg_init_driver)
  drv/wd_drv.c           — ctx 分配 (wd_hw_alloc_ctx, wd_soft_alloc_ctx)
```

### 4.2 关键函数：`wd_get_drv_array()` — Phase 1 驱动发现

**文件**: `wd_alg.c:673` (已实现, ~88 行)
**功能**: 遍历 `wd_drv_node` 全局链表，按算法类型和任务类型过滤，返回驱动数组

**签名** (当前实现):

```c
int wd_get_drv_array(const char *alg_type,
                     int task_type,
                     char *drv_name,
                     struct wd_alg_driver ***drv_array,
                     __u32 *drv_count);
```

**参数**:
| 参数 | 方向 | 说明 |
|------|------|------|
| `alg_type` | in | 算法类型 ("cipher", "digest", "aead", ...) |
| `task_type` | in | TASK_HW: 只返回 HW; TASK_MIX: 返回全部; TASK_INSTR: 返回非 HW |
| `drv_name` | in | 驱动名过滤 (V1="hisi_sec2"), NULL=不过滤 |
| `drv_array` | out | 去重驱动数组 (caller 负责 free via wd_put_drv_array) |
| `drv_count` | out | 驱动数量 |

**算法流程** (当前实现 wd_alg.c:673-761):

```
1. 调用 wd_get_alg_head() 获取 wd_drv_node 链表头
2. alloc(sizeof(drv*) × max_driver_count) 预分配驱动数组
3. 单次遍历 node = head->next:
   for each node:
     if node->alg_type != alg_type: continue
     if !wd_alg_drv_type_match(task_type, node->calc_type): continue
     if drv_name && node->drv_name != drv_name: continue
     if 节点中所有算法均不可用 (algs[].available 全为 false): continue
     drivers[current_count++] = node->drv  // 链表已去重，无需额外去重
4. if current_count == 0: free drivers, 返回 -WD_EINVAL
5. *drv_array = drivers, *drv_count = current_count, 返回 0
```

**V1/V2 差异**:

```
V1: wd_get_drv_array("cipher", TASK_HW, "hisi_sec2", ...)
  → 返回: [hisi_sec2 cipher_driver], count=1

V2: wd_get_drv_array("cipher", TASK_MIX, NULL, ...)
  → 返回: [hisi_sec2 cipher, isa_ce_sm4 cipher, soft cipher, ...], count=N
```

### 4.3 函数 `wd_drv_alg_count()` — 设计预留

**状态**: 设计文档中提出的覆盖广度统计函数，当前代码中未实现。
**说明**: 该函数设计用于计算指定驱动在特定算法类型下的算法覆盖数量，作为覆盖优先分配算法的前置步骤。由于对应的四步分配算法未实施，此函数也未创建。当前通过 `wd_drv_node.algs[]` 数组的 `available` 字段在 `wd_get_drv_array()` 中进行可用性检查。

### 4.4 关键函数：`wd_alg_ctx_init()` — Phase 2 ctx 分配

**文件**: `wd_util.c:2490` (已实现, ~200 行)
**功能**: 为 V2 路径统一分配 ctx、调度器，完成内部拷贝。这是实际实现的 ctx 分配函数。

> **说明**: 设计文档中提出的 `wd_ctxs_unified_alloc()` 函数名未在最终实现中采用。当前函数名为 `wd_alg_ctx_init()`。

**签名**:

```c
int wd_alg_ctx_init(struct wd_init_attrs *attrs);
```

**算法流程** (当前实现 wd_util.c:2490-2589):

```
1. 从 attrs->ctx_params 计算 ctx 总数:
   sync_num = sum(ctx_params->ctx_set_num[i].sync_ctx_num)
   async_num = sum(ctx_params->ctx_set_num[i].async_ctx_num)
   total_ctx_num = sync_num + async_num

2. 分配 user-visible ctx_config:
   ctx_config = calloc(sizeof(*ctx_config))
   ctx_config->ctxs = calloc(total_ctx_num, sizeof(struct wd_ctx))
   ctx_config->ctx_num = total_ctx_num

3. RR 分配 ctx (ctx_idx % drv_count = drv_idx):
   for ctx_idx in 0..total_ctx_num:
     drv = attrs->drv_array[ctx_idx % attrs->drv_count]
     根据 ctx_idx 位置确定 op_type (扫描 ctx_set_num 累积)
     填充 wd_drv_ctx_params {mode, op_type, numa_id, idx, bmp}
     drv->alloc_ctx(attrs->alg, &dparams, &ctx)  // ★ 真实分配
     ctx_config->ctxs[ctx_idx].ctx = ctx
     ctx_config->ctxs[ctx_idx].ctx_mode = dparams.ctx_mode
     ctx_config->ctxs[ctx_idx].op_type = dparams.op_type

4. 分配调度器:
   sched = wd_sched_rr_alloc(sched_type, max_region_num, attrs->alg, 1)
   wd_alg_sched_instance(sched, ctx_config, ctx_params)  // 注册 ctx 范围

5. 模块回调 (内部拷贝):
   attrs->alg_init(ctx_config, sched, attrs)  // e.g., wd_cipher_common_init()
     其中完成 config->ctx_config_internal 的创建和异步消息池的初始化
```

**与原始设计的差异**: 原始设计提出了一个四步"覆盖优先分配"算法（覆盖广度统计→类型多样性保底→比例分配→真实分配），当前实现采用的是更简单的 RR (Round-Robin) 分配方式。RR 分配确保 ctx 均匀分散到所有可用驱动，但不保证覆盖广度最优。

### 4.5 关键函数：`wd_ctx_bind_drivers()` — Phase 2.5 驱动绑定

**文件**: `wd_util.c:2229` (已实现, ~50 行)
**功能**: RR 方式绑定 ctxs[i].drv + 设置 HW 软算回退

**签名**:

```c
int wd_ctx_bind_drivers(struct wd_ctx_config_internal *config,
                        struct wd_alg_driver **drv_array,
                        __u32 drv_count);
```

**算法流程** (当前实现 wd_util.c:2229-2278):

```
for i = 0 .. config->ctx_num:
  if drv_count == 1:
    config->ctxs[i].drv = drv_array[0]
    config->ctxs[i].ctx_type = drv_array[0]->calc_type
  else:
    // RR 绑定 — ctxs[i].drv 的唯一写入点
    config->ctxs[i].drv = drv_array[i % drv_count]
    config->ctxs[i].ctx_type = config->ctxs[i].drv->calc_type

  // HW 驱动需要软算回退 — 每个唯一驱动只设置一次
  if config->ctxs[i].ctx_type == UADK_ALG_HW:
    if !drv->fallback:
      drv->fallback = wd_request_drv(config->alg_name, ALG_DRV_SOFT)

// 缓存驱动数组到 config (供 session 查询)
config->drv_array = drv_array
config->drv_count = drv_count

// 去重引用计数递增
wd_alg_drv_ref_inc(drv_array, drv_count)
```

> **说明**: 原始设计中包含 `drv_region[]` 元数据填充和 `init_refcnt` 递增，但在实际实现中 `drv_region` 结构未被采用，引用计数通过注册表节点 (`wd_drv_node.refcnt`) 而非驱动结构体的 `init_refcnt` 字段管理。

drv_region[]:
  [0] {drv_idx=0 (hisi_sec2),  ctx_begin=0, ctx_end=0}
  [1] {drv_idx=1 (isa_ce_sm4), ctx_begin=1, ctx_end=1}
  [2] {drv_idx=2 (soft),       ctx_begin=2, ctx_end=2}
  [3] {drv_idx=0 (hisi_sec2),  ctx_begin=3, ctx_end=3}
  ...  (共 8 个 region，因为 RR 交错分布)
```

### 4.6 关键函数：`wd_alg_attrs_init()` — V2 统一入口

**文件**: `wd_util.c:2699`
**功能**: V2 路径的 Phase 1 (驱动发现) + Phase 2 (ctx 分配)

**签名**:

```c
int wd_alg_attrs_init(struct wd_init_attrs *attrs);
```

**调用时序**:

```
wd_alg_attrs_init(attrs)  // V2 路径统一入口 (wd_util.c:2699)
  │
  ├── Phase 1: wd_alg_drv_discover() (wd_util.c:2311)
  │   ├─ wd_get_alg_type(attrs->alg, alg_type)
  │   └─ wd_get_drv_array(alg_type, attrs->task_type,
  │         NULL, &attrs->drv_array, &attrs->drv_count)
  │         // V2: drv_name=NULL 匹配全部驱动
  │
  └── Phase 2: wd_alg_ctx_init() (wd_util.c:2490)
      ├─ wd_sched_rr_alloc()  分配调度器
      ├─ 按 ctx_params 遍历 op_type: drv->alloc_ctx() RR 分配
      ├─ alg_init 回调 (模块内部拷贝)
      │     └─ wd_init_ctx_config()  拷贝 ctxs
      │     └─ wd_init_sched()       拷贝调度器
      │     └─ wd_init_async_request_pool()
      └─ 返回 attrs (drv_array, drv_count, ctx_config_internal 已填充)

注意: Phase 2.5 (wd_ctx_bind_drivers) 和 Phase 3 (wd_alg_init_driver)
      由 API 层在 wd_alg_attrs_init() 返回后单独调用。
      V1 路径不调用 wd_alg_attrs_init()，直接由 API 层调用各 Phase 函数。
```

### 4.7 待删除函数清单

> ✅ **更新说明 (2026-05-05)**: 以下函数状态根据实际代码审查进行了修正

| 函数                        | 文件:行号          | 状态      | 原因                                          |
| ------------------------- | -------------- | ------- | ------------------------------------------- |
| `wd_ctx_drv_config()`     | wd_util.c:2754 | ⚠️ 部分保留 | ⚠️ **问题函数**: 仅保留定义，已从 cipher 模块 init 中移除调用           |
| `wd_ctx_drv_deconfig()`   | wd_util.c      | ⚠️ 部分保留 | 同上，保留定义但 cipher 模块 uninit 中已移除调用                      |
| `wd_alg_drv_bind()`       | wd_util.c:2620 | ✅ 已废弃   | 被 `wd_ctx_bind_drivers()` 替代                |
| `wd_alg_drv_unbind()`     | wd_util.c:2673 | ✅ 已废弃   | 被 `drv->free_ctx` + 驱动引用计数管理 替代        |
| `wd_alg_hw_ctx_init()`    | wd_util.c:3060 | ✅ 已删除   | **已被 `wd_alg_ctx_init()` 替代**             |
| `wd_alg_other_ctx_init()` | wd_util.c:2915 | ✅ 已删除   | **已被 `wd_alg_ctx_init()` 替代**             |
| `wd_alg_other_init()`     | wd_util.c:3024 | ✅ 已删除   | **已被 `wd_alg_ctx_init()` 替代**             |
| `wd_hw_ctx_and_sched()`   | wd_util.c      | ✅ 已删除   | **已被 `wd_alg_ctx_init()` 替代**        |

**✅ 已修复的冗余调用**:

- ✅ wd_cipher.c: 已移除 wd_ctx_drv_config() 调用
- ✅ wd_join_gather.c: 已移除 wd_ctx_drv_config() 调用 (2026-05-04 修复)

**⚠️ 待检查模块** (需确认是否还有冗余调用):

- wd_digest.c, wd_aead.c, wd_comp.c, wd_rsa.c, wd_dh.c, wd_ecc.c, wd_udma.c, wd_agg.c

---

## 第五章 问题→解决函数调用链

### 5.1 P1-P4: 路径融合

**问题**: init1 和 init2 是两套完全独立的初始化逻辑。

**解决**: 两条路径在 API 层区分调用方式，V2 通过 `wd_alg_attrs_init()` 统一入口，V1 直接调用各 Phase 函数。

```
V1 调用链:
  wd_cipher_init(config, sched)
    └─ wd_cipher_common_init(config, sched)  // 深拷贝 + pool
    └─ wd_get_drv_array("cipher", TASK_HW, "hisi_sec2", ...)
    │     → drv_name="hisi_sec2" 精确匹配 → [hisi_sec2]
    └─ wd_ctx_bind_drivers(...) ───────────────────────┐
    └─ wd_alg_init_driver(...)                         │
                                                         │
V2 调用链:                                               │
  wd_cipher_init2_(alg, sched_type, task_type, ctx_params)│
    └─ wd_ctx_param_init()                               │
    └─ wd_alg_attrs_init(&attrs) ──────────────────────┐ │
    └─ wd_ctx_bind_drivers(...)                        │ │
    └─ wd_alg_init_driver(...)                         │ │
                                                         │ │
    ┌────────────────────────────────────────────────────┘ │
    │  ┌──────────────────────────────────────────────────┘
    ▼  ▼
  共享框架函数:
    ├─ Phase 1: wd_get_drv_array()
    │     V1: drv_name="hisi_sec2" → [hisi_sec2]
    │     V2: drv_name=NULL → [hisi_sec2, isa_ce_sm4, soft]
    ├─ Phase 2 (仅 V2):
    │     wd_alg_ctx_init() → drv->alloc_ctx() RR 分配
    │     └─ alg_init 回调 → wd_cipher_common_init()
    ├─ Phase 2.5 (V1+V2):
    │     wd_ctx_bind_drivers() → RR 绑定 ctxs[i].drv
    └─ Phase 3 (V1+V2):
          wd_alg_init_driver() → drv->init()
```

### 5.2 P5: 参数桥接

**问题**: V1 传 `wd_ctx_config*`，V2 传 `alg_name + ctx_params`。

**解决**: 在 API 层构造 `wd_init_attrs` 时桥接。

```
V1 (wd_cipher_init):
  attrs.ctx_config_internal = &wd_cipher_setting.config;
      // ↑ common_init 已填充，包含用户预分配的 ctx
  attrs.ctx_config = config;
      // ↑ 用户传入的外部 ctx_config
  attrs.ctx_params = NULL;
      // ↑ V1 没有 ctx_params，Phase 1 使用 alg="cipher" 提取 alg_type

V2 (wd_cipher_init2_):
  attrs.ctx_config_internal = NULL;
      // ↑ Phase 2 内部分配
  attrs.ctx_config = NULL;
      // ↑ Phase 2 内部 alloc
  attrs.ctx_params = &cipher_ctx_params;
      // ↑ 用户传入的 ctx 参数 (op_type_num, ctx_set_num, bmp)
```

### 5.3 P6: init1 只支持 hisi_sec2

**问题**: init1 硬编码 hisi_sec2。

**解决**: `wd_get_drv_array(filter="hisi_sec2")` 精确过滤。

```
wd_get_drv_array("cipher", TASK_HW, "hisi_sec2", &array, &count)
  │
  ├─ 遍历 alg_list_head
  │   for each node:
  │     if node->drv_name != "hisi_sec2": continue  ← V1 filter
  │     if node->alg_type != "cipher": continue
  │     if node->calc_type != UADK_ALG_HW: continue ← TASK_HW
  │     去重 → 加入数组
  │
  └─ 返回: array = [hisi_sec2 cipher driver], count = 1

→ Phase 2: V1 跳过 alloc_ctx (ctx 已预分配)
→ Phase 2.5: ctxs[i].drv = drv_array[0] = hisi_sec2 cipher driver
```

### 5.4 P7: Session 算法兼容性路由

**问题**: 不同驱动支持的算法子集不同，调度器 `pick_next_ctx` 可能返回不支持该算法的 ctx（例如 AES 请求路由到只支持 SM4 的 isa_ce_sm4 ctx）。

**设计方案**: 通过 `wd_alg_match_drv()` + 调度器 `set_param` 机制实现 session 级别的 compat 过滤。

**核心链路**: `ctx_id → ctxs[idx].drv → wd_alg_match_drv(drv, alg_name)`

**设计原则**:
1. `sched_init` 时正常预取 ctx（无 compat 过滤）
2. `set_param` 时传入 `alg_name` + `ctxs`，触发 `wd_sched_skey_compat_filter()` 修正已预取的 ctx
3. `session_sched_init_ctx()` 增加 compat 过滤逻辑，通过 RR 遍历 domain 找兼容 ctx
4. HUNGRY 策略扩展 ctx 时自动继承 compat 过滤

**数据结构变更**:

```c
// 扩展 wd_sched_params (include/wd_internal.h)
struct wd_sched_params {
    __u32 pkt_size;
    __u16 data_mode;
    __u16 prio_mode;
    /* ★ NEW */
    const char *alg_name;           // 算法名，如 "ecb(aes)"
    struct wd_ctx_internal *ctxs;   // ctx 数组指针，用于获取 ctxs[i].drv
};

// 扩展 wd_sched_key (wd_sched.c 内部)
struct wd_sched_key {
    // ... 现有字段 ...
    const char *alg_name;           /* ★ NEW */
    struct wd_ctx_internal *ctxs;   /* ★ NEW */
};
```

**函数调用链**:

```
wd_cipher_alloc_sess(setup)
  │
  ├─ sched_init(h_sched_ctx, setup->sched_param)
  │     → 分配 skey, 预取 ctx（无 compat 过滤）
  │
  ├─ set_param(h_sched_ctx, skey, &params)
  │     params = { .alg_name = "ecb(aes)", .ctxs = ... }
  │     │
  │     └→ wd_sched_set_param():
  │          ├─ skey->alg_name = params.alg_name
  │          ├─ skey->ctxs = params.ctxs
  │          └─ ★ wd_sched_skey_compat_filter(sched_ctx, skey, &sync_domain)
  │               │
  │               │  for each ctx in domain cache:
  │               │    if !wd_alg_match_drv(ctxs[ctx].drv, alg_name):
  │               │      new_ctx = session_sched_init_ctx(..., skey)  // 找兼容的
  │               │      replace ctx with new_ctx
  │               │
  │               └→ session_sched_init_ctx(..., skey):
  │                    通过 RR 遍历 domain:
  │                      if wd_alg_match_drv(ctxs[ctx_idx].drv, alg_name):
  │                        return ctx_idx  // 找到兼容 ctx
  │
  └─ return (handle_t)sess
```

**`wd_alg_match_drv()` 函数** (wd_alg.c:423):

已在代码中实现，需在 `wd_alg.h` 中添加声明导出供调度器调用。逻辑：遍历 `wd_drv_node` 链表，按 `(drv, alg_name)` 精确匹配，返回是否找到对应注册项。

**涉及的 10 个 API 模块**:

| 序号 | 文件 | alloc_sess 行号 |
|------|------|----------------|
| 1 | wd_cipher.c | 267 |
| 2 | wd_digest.c | 205 |
| 3 | wd_aead.c | 460 |
| 4 | wd_comp.c | 457 |
| 5 | wd_rsa.c | 931 |
| 6 | wd_ecc.c | 1199 |
| 7 | wd_dh.c | 577 |
| 8 | wd_agg.c | 425 |
| 9 | wd_join_gather.c | 497 |
| 10 | wd_udma.c | 74 |

### 5.5 P8-P9: ctx 分配时机与"不可能三角"

**问题**: init 时分配 ctx 导致 session 时可能不兼容；延迟分配无法多 session 共享。

**解决**: 覆盖优先分配在 init 阶段最大化兼容范围 + session 时在兼容子集中路由。

> **⚠️ 实现说明**: 当前实际实现为简化版本（对每个 op_type 选第一个 HW 驱动），非下面展示的覆盖广度统计+比例分配。以下示例为**设计目标**。

```
┌─────────────────────────────────────────────────────────────────┐
│                       "不可能三角" 解决方案                       │
│                                                                  │
│  Init 阶段 (进程级，一次):                                       │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ wd_ctxs_unified_alloc()                                   │   │
│  │   1. 覆盖广度统计: hisi_sec2=21, isa_ce_sm4=4, soft=21    │   │
│  │   2. 类型多样性保底: 每种 calc_type 至少 1 个 ctx          │   │
│  │   3. 比例分配: 剩余 ctx 按 score 比例分配                   │   │
│  │   4. drv->alloc_ctx() 真实分配硬件队列                      │   │
│  │                                                             │   │
│  │   结果: hisi_sec2:3, isa_ce_sm4:2, soft:3                   │   │
│  │   覆盖: 21/21 (hisi_sec2) + 4/4 (isa_ce_sm4) + 21/21 (soft)│   │
│  └──────────────────────────────────────────────────────────┘   │
│                              │                                    │
│                              ▼                                    │
│  Session 阶段 (每次 alloc_sess):                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ wd_cipher_alloc_sess("ecb(aes)")                          │   │
│  │   → wd_get_compat_ctxs("ecb(aes)")                         │   │
│  │     → compat.indices = [0,1,2,5,6,7] (6 ctx, 排除 CE)     │   │
│  │     → compat.count = 6                                     │   │
│  │   → sched_init(&compat)                                    │   │
│  │     → 仅 6 个兼容 ctx 注入 slots[]                          │   │
│  │     → 调度器在 slots[] 内 RR 选择                           │   │
│  │                                                             │   │
│  │ wd_cipher_alloc_sess("ecb(sm4)")                            │   │
│  │   → wd_get_compat_ctxs("ecb(sm4)")                          │   │
│  │     → compat.indices = [0..7] (全部 8 ctx)                  │   │
│  │     → compat.count = 8                                      │   │
│  │   → 全部 ctx 都被注入 slots[]                                │   │
│  │   → LOOP 策略在 HW/CE/SOFT 之间负载感知切换                  │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

### 5.6 P9: 多设备算力叠加 (init2 约束 C2)

**问题**: 需要支持多类型设备 (HW+CE+SOFT) 和多同类型设备 (多个 hisi_sec2 实例) 的算力叠加。

**解决**: 覆盖优先分配时多个同类型设备按比例获得 ctx；session 时调度器通过 dev_id 做负载均衡。

```
场景: 2 个 hisi_sec2 设备 (hisi_sec2-0, hisi_sec2-1) + 1 个 isa_ce_sm4 + 1 个 soft

Phase 1:
  wd_get_drv_array("cipher", TASK_MIX, NULL)
  → 注意: 每个设备实例在 alg_list_head 中注册为独立节点
  → 但 wd_alg_driver * 指针去重后会合并
  → 返回: [hisi_sec2, isa_ce_sm4, soft], count=3

Phase 2 (wd_ctxs_unified_alloc):
  hisi_sec2 分得 3 个 ctx
    → alloc_ctx 内部通过 dev_cache 在 2 个设备间 RR
    → ctx[0] → hisi_sec2-0, ctx[1] → hisi_sec2-1, ctx[2] → hisi_sec2-0
    → 两个设备自然叠加算力

  isa_ce_sm4 分得 2 个 ctx (纯软队列，本地 CPU 执行)
  soft 分得 3 个 ctx

Phase 2.5:
  ctxs[i].drv 绑定: RR 在 3 个驱动间交错

Session:
  对 "ecb(aes)": compat = {hisi_sec2 ctxs, soft ctxs}
    → pick_next_ctx 在兼容子集中 RR
    → dev_id 维度可区分 hisi_sec2-0 vs hisi_sec2-1 → 算力叠加
```

---

## 第六章 API 层 (wd_cipher.c) 改造

### 6.1 `wd_cipher_init()` — V1 路径 (当前实现)

**文件**: `wd_cipher.c:401`
**说明**: V1 路径不经过 `wd_alg_attrs_init()`，ctx 由用户预分配。API 层直接调用框架函数完成初始化。
**改造要点**: 删除 `wd_ctx_drv_config` 调用

```c
int wd_cipher_init(struct wd_ctx_config *config, struct wd_sched *sched)
{
    int ret;

    pthread_atfork(NULL, NULL, wd_cipher_clear_status);

    ret = wd_alg_try_init(&wd_cipher_setting.status);
    if (ret) return ret;

    ret = wd_init_param_check(config, sched);
    if (ret) goto out_clear_init;

    ret = wd_cipher_open_driver(WD_TYPE_V1);  // dlopen libhisi_sec.so
    if (ret) goto out_clear_init;

    // 深拷贝 config → internal, 深拷贝 sched, 分配 pool
    ret = wd_cipher_common_init(config, sched);
    if (ret) goto out_close_driver;

    // V1: 直接调用 wd_get_drv_array() 获取 hisi_sec2 驱动
    //     drv_name="hisi_sec2" 精确匹配单个驱动
    ret = wd_get_drv_array("cipher", TASK_HW, "hisi_sec2",
                           &wd_cipher_setting.config.drv_array,
                           &wd_cipher_setting.config.drv_count);
    if (ret) goto out_uninit_nolock;

    // V1: ctx 已由用户预分配，直接绑定驱动
    ret = wd_ctx_bind_drivers(&wd_cipher_setting.config,
                               wd_cipher_setting.config.drv_array,
                               wd_cipher_setting.config.drv_count);
    if (ret) goto out_drv_deconfig;

    ret = wd_alg_init_driver(&wd_cipher_setting.config);
    if (ret) goto out_drv_deconfig;

    wd_alg_set_init(&wd_cipher_setting.status);
    return 0;

out_drv_deconfig:
    wd_ctx_unbind_drivers(&wd_cipher_setting.config);
out_uninit_nolock:
    wd_cipher_common_uninit();
out_close_driver:
    wd_cipher_close_driver(WD_TYPE_V1);
out_clear_init:
    wd_alg_clear_init(&wd_cipher_setting.status);
    return ret;
}
```

### 6.2 `wd_cipher_init2_()` — V2 路径

**文件**: `wd_cipher.c:456`
**改造要点**: 构造 `wd_init_attrs` → 调用统一入口 → **删除** `wd_ctx_drv_config` (line 517)

```c
int wd_cipher_init2_(char *alg, __u32 sched_type, int task_type,
                     struct wd_ctx_params *ctx_params)
{
    struct wd_ctx_nums cipher_ctx_num[WD_CIPHER_DECRYPTION + 1] = {0};
    struct wd_ctx_params cipher_ctx_params = {0};
    struct wd_init_attrs attrs = {0};
    int state, ret = -WD_EINVAL;
    bool flag;

    pthread_atfork(NULL, NULL, wd_cipher_clear_status);

    state = wd_alg_try_init(&wd_cipher_setting.status);
    if (state) return state;

    if (!alg || sched_type >= SCHED_POLICY_BUTT ||
        task_type < 0 || task_type >= TASK_MAX_TYPE)
        goto out_uninit;

    flag = wd_cipher_alg_check(alg);
    if (!flag) { WD_ERR("cipher:%s unsupported!\n", alg); goto out_uninit; }

    state = wd_cipher_open_driver(WD_TYPE_V2);  // wd_dlopen_drv(NULL) 加载全部驱动
    if (state) goto out_uninit;

    while (ret != 0) {
        memset(&wd_cipher_setting.config, 0, sizeof(struct wd_ctx_config_internal));

        cipher_ctx_params.ctx_set_num = cipher_ctx_num;
        ret = wd_ctx_param_init(&cipher_ctx_params, ctx_params,
                                alg, task_type, WD_CIPHER_TYPE,
                                WD_CIPHER_DECRYPTION + 1);
        if (ret) {
            if (ret == -WD_EAGAIN) continue;
            goto out_dlclose;
        }

        // ====== 构造统一 attrs ======
        strcpy(attrs.alg, alg);
        attrs.sched_type      = sched_type;
        attrs.task_type       = task_type;     // TASK_HW / TASK_MIX / TASK_INSTR
        attrs.ctx_params      = &cipher_ctx_params;
        attrs.alg_init        = wd_cipher_common_init;
        attrs.alg_poll_ctx    = wd_cipher_poll_ctx;
            // V2: wd_alg_attrs_init() 内部调用 wd_get_drv_array() 时 drv_name=NULL (全量驱动)
        attrs.ctx_config_internal = NULL;      // Phase 2 内部分配
        attrs.ctx_config      = NULL;          // Phase 2 内部分配

        ret = wd_alg_attrs_init(&attrs);
        if (ret) {
            if (ret == -WD_ENODEV) {
                wd_ctx_param_uninit(&cipher_ctx_params);
                continue;
            }
            WD_ERR("fail to init alg attrs.\n");
            goto out_params_uninit;
        }
    }

    // ====== [删除]: ret = wd_ctx_drv_config(alg, &wd_cipher_setting.config); ======
    // 原因是 wd_alg_attrs_init() 的 Phase 2.5 (wd_ctx_bind_drivers) 已完成绑定

    ret = wd_alg_init_driver(&wd_cipher_setting.config);
    if (ret) goto out_uninit_nolock;

    wd_alg_set_init(&wd_cipher_setting.status);
    wd_ctx_param_uninit(&cipher_ctx_params);
    return 0;

out_uninit_nolock:
    wd_cipher_common_uninit();
    wd_alg_attrs_uninit(&attrs);
out_params_uninit:
    wd_ctx_param_uninit(&cipher_ctx_params);
out_dlclose:
    wd_cipher_close_driver(WD_TYPE_V2);
out_uninit:
    wd_alg_clear_init(&wd_cipher_setting.status);
    return ret;
}
```

### 6.3 `wd_cipher_alloc_sess()` — Session 兼容 ctx 路由 (P7)

**文件**: `wd_cipher.c:267`

```c
handle_t wd_cipher_alloc_sess(struct wd_cipher_sess_setup *setup)
{
    struct wd_cipher_sess *sess = NULL;
    struct wd_sched_params params = {0};

    if (unlikely(!setup)) return (handle_t)0;

    sess = malloc(sizeof(struct wd_cipher_sess));
    if (!sess) return (handle_t)0;
    memset(sess, 0, sizeof(struct wd_cipher_sess));

    if (setup->alg >= WD_CIPHER_ALG_TYPE_MAX ||
        setup->mode >= WD_CIPHER_MODE_TYPE_MAX)
        goto free_sess;

    sess->alg_name = wd_cipher_alg_name[setup->alg][setup->mode];
    ret = wd_drv_alg_support(sess->alg_name, &wd_cipher_setting.config);
    if (!ret) {
        WD_ERR("failed to support this algorithm: %s!\n", sess->alg_name);
        goto free_sess;
    }
    sess->alg = setup->alg;
    sess->mode = setup->mode;

    if (cipher_setup_memory_and_buffers(sess, setup))
        goto free_sess;

    /* sched_init: 分配 skey, 预取 ctx (无 compat 过滤) */
    sess->sched_key = (void *)wd_cipher_setting.sched.sched_init(
        wd_cipher_setting.sched.h_sched_ctx,
        setup->sched_param);
    if (WD_IS_ERR(sess->sched_key)) {
        WD_ERR("failed to init schedule key!\n");
        goto free_key;
    }

    /* ★ P7: set_param 传入 compat 信息, 修正已预取的 ctx */
    params.alg_name = sess->alg_name;
    params.ctxs     = wd_cipher_setting.config.ctxs;
    wd_cipher_setting.sched.set_param(
        wd_cipher_setting.sched.h_sched_ctx,
        sess->sched_key, &params);

    return (handle_t)sess;

free_key:
    sess->mm_ops.free(sess->mm_ops.usr, sess->key);
free_sess:
    free(sess);
    return (handle_t)0;
}
```
### 6.4 uninit 清理路径

```c
// V1 uninit
void wd_cipher_uninit(void)
{
    wd_alg_uninit_driver(&wd_cipher_setting.config);
    // [删除: wd_ctx_drv_deconfig(&wd_cipher_setting.config)]
    wd_cipher_common_uninit();
    wd_cipher_close_driver(WD_TYPE_V1);
    wd_alg_clear_init(&wd_cipher_setting.status);
}

// V2 uninit
void wd_cipher_uninit2(void)
{
    // [删除: wd_ctx_drv_deconfig(&wd_cipher_setting.config)]
    wd_cipher_common_uninit();
    wd_alg_attrs_uninit(&wd_cipher_init_attrs);
    wd_cipher_close_driver(WD_TYPE_V2);
    wd_alg_clear_init(&wd_cipher_setting.status);
}
```

---

## 第七章 驱动层 (hisi_sec.c cipher) 改造

### 7.1 GEN_SEC_ALG_DRIVER 宏扩展

**文件**: `drv/hisi_sec.c:806`

```c
#define GEN_SEC_ALG_DRIVER(sec_alg_name, alg_type) \
{\
    .drv_name    = "hisi_sec2",\
    .alg_name    = (sec_alg_name),\
    .calc_type   = UADK_ALG_HW,\
    .priority    = 100,\
    .priv_size   = sizeof(struct hisi_sec_ctx),\
    .queue_num   = SEC_CTX_Q_NUM_DEF,\
    .op_type_num = 1,\
    .fallback    = 0,\
    .init        = hisi_sec_init,\
    .exit        = hisi_sec_exit,\
    .send        = alg_type##_send,\
    .recv        = alg_type##_recv,\
    .get_usage   = hisi_sec_get_usage,\
    .get_extend_ops = sec_aead_get_extend_ops,\
    /* ====== NEW ====== */ \
    .alloc_ctx   = wd_drv_hw_ctx_alloc,\
    .free_ctx    = wd_drv_hw_ctx_free,\
}
```

### 7.2 `hisi_sec_init()` — 保持不变

`hisi_sec_init()` (原设计 line 3892，实际代码行号可能不同) 接收 `wd_ctx_config_internal *config`，遍历 `config->ctxs[]`，为 `ctx_type == UADK_CTX_HW` 的 ctx 分配 QP：

```c
static int hisi_sec_init(void *conf, void *priv)
{
    struct wd_ctx_config_internal *config = conf;
    struct hisi_sec_ctx *sec_ctx = priv;
    ...

    for (i = 0; i < config->ctx_num; i++) {
        if (config->ctxs[i].ctx_type != UADK_CTX_HW ||
            !config->ctxs[i].ctx)
            continue;  // 跳过非 HW ctx

        h_ctx = config->ctxs[i].ctx;
        // 分配 QP...
        h_qp = hisi_qm_alloc_qp(&qm_priv, h_ctx);
        config->ctxs[i].sqn = qm_priv.sqn;
    }
    ...
}
```

**关键**: `hisi_sec_init()` 不需要改动。Phase 2.5 之后 `ctxs[i].drv` 已正确绑定到 hisi_sec2，且 `ctxs[i].ctx_type` 正确设置为 `UADK_CTX_HW`。

### 7.3 `cipher_send()` / `cipher_recv()` — 保持不变

```c
static int cipher_send(handle_t ctx, void *msg)
{
    struct hisi_qp *qp = (struct hisi_qp *)wd_ctx_get_priv(ctx);
    if (qp->q_info.hw_type == HISI_QM_API_VER2_BASE)
        return hisi_sec_cipher_send(ctx, msg);
    return hisi_sec_cipher_send_v3(ctx, msg);
}
```

send/recv 的 ctx 参数来自 `ctxs[idx].ctx`，已通过 Phase 2/2.5 正确初始化。不需改动。

---

## 第八章 数据流图

### 8.1 Init 阶段完整数据流 (V2 - init2)

```
wd_cipher_init2_("ecb(aes)", SCHED_POLICY_RR, TASK_MIX, ctx_params)
│
├─ 输入: alg, sched_type, task_type, ctx_params
│
├─ wd_cipher_alg_check("ecb(aes)") → true
├─ wd_cipher_open_driver(WD_TYPE_V2)
│    └─ wd_dlopen_drv(NULL)
│         └─ dlopen("libhisi_sec.so")  → 驱动注册 → alg_list_head 增加 21 个 cipher 节点
│         └─ dlopen("libisa_ce.so")    → 驱动注册 → alg_list_head 增加 4 个 cipher 节点
│
├─ wd_ctx_param_init(&cipher_ctx_params, ctx_params, alg, task_type, ...)
│    └─ 解析 ctx_set_num[]: sync_ctx_num, async_ctx_num, ctx_prop, ...
│
├─ 构造 wd_init_attrs:
│    .alg = "ecb(aes)"
│    .sched_type = SCHED_POLICY_RR
│    .task_type = TASK_MIX
│    .alg_poll_ctx = wd_cipher_poll_ctx
│
├── wd_alg_attrs_init(&attrs) ────────────────────────────┐
│                                                           │
│  Phase 1: 驱动发现                                       │
│  ┌──────────────────────────────────────────────────┐    │
│  │ wd_get_alg_type("ecb(aes)", alg_type)            │    │
│  │   → alg_type = "cipher"                          │    │
│  │                                                   │    │
│  │ wd_get_drv_array("cipher", TASK_MIX, NULL,        │    │
│  │                   &drv_array, &drv_count)          │    │
│  │   → 遍历 alg_list_head:                           │    │
│  │      "ecb(aes)" / hisi_sec2 / HW / available   ✓  │    │
│  │      "cbc(aes)" / hisi_sec2 / HW / available   ✓  │    │
│  │      ... (去重后 = hisi_sec2)                      │    │
│  │      "ecb(sm4)" / isa_ce_sm4 / CE / available  ✓  │    │
│  │      ... (去重后 = isa_ce_sm4)                     │    │
│  │      "ecb(aes)" / soft / SOFT / available      ✓  │    │
│  │      ... (去重后 = soft)                           │    │
│  │   → drv_array = [hisi_sec2, isa_ce_sm4, soft]     │    │
│  │   → drv_count = 3                                  │    │
│  │   → 存入 attrs.drv_array, attrs.drv_count          │    │
│  └──────────────────────────────────────────────────┘    │
│                                                           │
│  Phase 2: ctx 分配                                       │
│  ┌──────────────────────────────────────────────────┐    │
│  │ wd_sched_rr_alloc(SCHED_POLICY_RR, op_type_num,   │    │
│  │                    numa_max+1, poll_func)          │    │
│  │   → attrs->sched = alg_sched                      │    │
│  │                                                   │    │
│  │ wd_alg_ctx_init(attrs)                             │    │
│  │   ctx_total = sum of all ctx_set_num entries      │    │
│  │   假设 ctx_total = 8 (4 sync + 4 async)           │    │
│  │                                                   │    │
│  │   RR 分配: ctx_idx % drv_count 分配到各驱动        │    │
│  │   hisi_sec2=3, isa_ce_sm4=2, soft=3              │    │
│  │                                                   │    │
│  │   循环 drv->alloc_ctx():                          │    │
│  │     hisi_sec2:                                    │    │
│  │       wd_drv_hw_ctx_alloc(drv, {mode,op,dev=NULL})│    │
│  │         → 内部: 设备缓存 → NUMA 感知 →            │    │
│  │           wd_request_ctx(dev) → open /dev/hisi_sec2│    │
│  │           ctx->ctx_type = UADK_CTX_HW             │    │
│  │         → 返回 h_ctx                              │    │
│  │       ctxs[0..2].ctx = h_ctx (3 个 HW ctx)        │    │
│  │       ctxs[0..2].ctx_type = UADK_CTX_HW           │    │
│  │                                                   │    │
│  │     isa_ce_sm4:                                   │    │
│  │       wd_drv_soft_ctx_alloc(drv, {mode})           │    │
│  │         → calloc(wd_soft_ctx), spin_init          │    │
│  │         → sctx->ctx_type = UADK_CTX_CE_INS       │    │
│  │         → sctx->fd = -1                           │    │
│  │         → 返回 h_ctx                              │    │
│  │       ctxs[3..4].ctx = h_ctx (2 个 CE ctx)        │    │
│  │       ctxs[3..4].ctx_type = UADK_CTX_CE_INS       │    │
│  │                                                   │    │
│  │     soft:                                         │    │
│  │       wd_drv_soft_ctx_alloc(drv, {mode})           │    │
│  │         → sctx->ctx_type = UADK_CTX_SOFT         │    │
│  │       ctxs[5..7].ctx = h_ctx (3 个 SOFT ctx)      │    │
│  │       ctxs[5..7].ctx_type = UADK_CTX_SOFT         │    │
│  │                                                   │    │
│  │   ctx_config->ctx_num = 8                          │    │
│  │   ctx_config->ctxs = [8 个 wd_ctx]                 │    │
│  │                                                   │    │
│  │ alg_init_func(ctx_config, alg_sched)              │    │
│  │   = wd_cipher_common_init(ctx_config, sched)       │    │
│  │     ├─ wd_init_ctx_config(&internal, ctx_config)   │    │
│  │     │    → 深拷贝 ctxs → wd_ctx_internal[]         │    │
│  │     │    → 设置 ctxs[i].ctx_type, ctxs[i].ctx_mode│    │
│  │     ├─ wd_init_sched(&internal_sched, sched)       │    │
│  │     └─ wd_init_async_request_pool(&pool, ...)      │    │
│  │     → attrs->ctx_config_internal = &setting.config │    │
│  └──────────────────────────────────────────────────┘    │
│                                                           │
│  Phase 2.5: 驱动绑定                                     │
│  ┌──────────────────────────────────────────────────┐    │
│  │ wd_ctx_bind_drivers(config_internal,               │    │
│  │                      drv_array, drv_count)         │    │
│  │   → ctxs[0].drv = drv_array[0%3=0] = hisi_sec2   │    │
│  │   → ctxs[1].drv = drv_array[1%3=1] = isa_ce_sm4  │    │
│  │   → ctxs[2].drv = drv_array[2%3=2] = soft        │    │
│  │   → ctxs[3].drv = drv_array[3%3=0] = hisi_sec2   │    │
│  │   → ... (RR 交错)                                 │    │
│  │   → 递增驱动引用计数 (wd_alg_drv_ref_inc())          │    │
│  │   → config->drv_array = drv_array                  │    │
│  │   → config->drv_count = 3                          │    │
│  │   → store drv_array/drv_count to config            │    │
│  └──────────────────────────────────────────────────┘    │
│                                                           │
│  返回 attrs 到调用者 ─────────────────────────────────    │
└───────────────────────────────────────────────────────────┘
│
├── [删除: wd_ctx_drv_config(alg, &config)] ← BUG 修复
│
├── wd_alg_init_driver(&wd_cipher_setting.config)
│    └─ for ctx in config->ctxs:
│         if ctx.drv && !ctx.drv->init_state:
│             ctx.drv->init(config, priv)
│               = hisi_sec_init(config, hisi_sec_ctx)
│                 → hisi_qm_alloc_qp() 为每个 HW ctx 分配 QP
│             (isa_ce_sm4 和 soft 的 init 可能为空或无操作)
│
└── wd_alg_set_init(&wd_cipher_setting.status)
     → status = WD_INIT
```

### 8.2 Session 阶段数据流 (P7 方案)

```
wd_cipher_alloc_sess({.alg=AES, .mode=ECB, ...})
│
├─ sess->alg_name = "ecb(aes)"
│
├─ sched_init(h_sched_ctx, setup->sched_param)
│    └─ 分配 skey, 预取 sync_ctx + async_ctx (无 compat 过滤)
│    └─ 返回 skey
│
├─ ★ set_param(h_sched_ctx, skey, &params)
│    params = { .alg_name="ecb(aes)", .ctxs=wd_cipher_setting.config.ctxs }
│    │
│    └→ wd_sched_set_param():
│         ├─ skey->alg_name = "ecb(aes)"
│         ├─ skey->ctxs = ctxs
│         └─ wd_sched_skey_compat_filter(sched_ctx, skey, &sync_domain):
│              │
│              │  for each ctx in domain.idx_cache:
│              │    ┌─ ctxs[0]: drv=hisi_sec2
│              │    │    wd_alg_match_drv(hisi_sec2, "ecb(aes)")
│              │    │    → 遍历 wd_drv_node: 找到 "ecb(aes)"+hisi_sec2 → true ✅
│              │    │    → 保持不变
│              │    ├─ ctxs[1]: drv=hisi_sec2 → true ✅ → 保持不变
│              │    ├─ ctxs[3]: drv=isa_ce_sm4
│              │    │    wd_alg_match_drv(isa_ce_sm4, "ecb(aes)")
│              │    │    → isa_ce_sm4 只注册了 sm4 系列 → false ❌
│              │    │    → 调用 session_sched_init_ctx(..., skey) 找兼容 ctx
│              │    │       通过 RR 遍历 domain:
│              │    │         if wd_alg_match_drv(ctxs[ctx_idx].drv, "ecb(aes)"):
│              │    │           return ctx_idx  // 找到兼容 ctx (如 hisi_sec2)
│              │    │    → 替换: idx_cache[i] = new_ctx
│              │    └─ ctxs[4]: drv=isa_ce_sm4 → false ❌ → 找兼容 ctx 替换
│              │
│              └→ 同样处理 async_domain
│
│ 结果: domain cache 中所有 ctx 的 drv 都支持 "ecb(aes)"
│
└─ return (handle_t)sess

后续 pick_next_ctx():
  → 从已过滤的 domain cache 中选择 ctx
  → 保证返回兼容的 ctx
```
---

## 第九章 数据一致性保证

### 9.1 ctx_type 首字段不变量

**规则**: `ctx_type` 必须是所有 ctx 结构体的首字段 (offset 0)。

```
struct wd_ctx_h:    offset 0 = ctx_type (UADK_CTX_HW = 0)
struct wd_soft_ctx: offset 0 = ctx_type (UADK_CTX_CE_INS=1 / SVE_INS=2 / SOFT=3)
struct wd_ce_ctx:   offset 0 = ctx_type (UADK_CTX_CE_INS = 1)
```

**安全检测** (wd_bmm.c):

```c
static inline bool wd_ctx_is_hw(handle_t h_ctx) {
    return h_ctx && (*(__u8 *)h_ctx) == UADK_CTX_HW;
}
```

**注意**: `UADK_CTX_HW == 0`，而 `calloc` 初始化为 0。所以非 HW ctx 的分配函数 (`wd_drv_soft_ctx_alloc`) **必须**在 `calloc` 后显式设置非零值。

### 9.2 驱动引用计数管理

**场景**: hisi_sec2 驱动同时注册了 cipher (21 项)、digest (13 项)、aead (6 项)。当 cipher 和 digest 同时初始化时:

- `wd_cipher_init2_()` → `wd_ctx_bind_drivers()` → `wd_alg_drv_ref_inc()` 递增 hisi_sec2 引用
- `wd_digest_init2_()` → `wd_ctx_bind_drivers()` → `wd_alg_drv_ref_inc()` 递增 hisi_sec2 引用
- `wd_alg_init_driver()` 检查 `init_state`，防止重复 `hisi_sec_init()`

**说明**: 引用计数通过注册表节点 `wd_drv_node.refcnt` 管理 (`wd_alg_drv_ref_inc()` / `wd_alg_drv_ref_dec()`)，而非在 `wd_alg_driver` 结构体中独立维护。

### 9.3 ctxs[i].drv 与驱动初始化去重

**规则**: `ctxs[i].drv` 由 `wd_ctx_bind_drivers()` 在 Phase 2.5 **一次性**设置。此后在驱动 init/exit/uninit 整个生命周期中**只读不写**。

**驱动初始化解重**: `wd_alg_init_driver()` 通过 `drv->init_state` 标记判断是否已初始化，确保每个唯� (唯一驱动只调用一次 `drv->init()`)。

```c
// 在 wd_alg_init_driver() 中，遍历各 ctx 绑定的驱动:
for (i = 0; i < config->ctx_num; i++) {
    drv = config->ctxs[i].drv;
    if (drv && !drv->init_state) {
        drv->init(config, drv->drv_data);  // 每个唯一驱动只调用一次
        drv->init_state = 1;
    }
}
```

### 9.4 ctxs[].ctx 与 ctxs[].ctx_type 一致性

| ctx 来源                           | ctx 类型        | ctx_type 值             | 谁设置                          |
| -------------------------------- | ------------- | ---------------------- | ---------------------------- |
| `wd_drv_hw_ctx_alloc()`          | `wd_ctx_h`    | `UADK_CTX_HW (0)`      | `wd_drv_hw_ctx_alloc()` 内部   |
| `wd_drv_soft_ctx_alloc()` (CE)   | `wd_soft_ctx` | `UADK_CTX_CE_INS (1)`  | `wd_drv_soft_ctx_alloc()` 内部 |
| `wd_drv_soft_ctx_alloc()` (SVE)  | `wd_soft_ctx` | `UADK_CTX_SVE_INS (2)` | `wd_drv_soft_ctx_alloc()` 内部 |
| `wd_drv_soft_ctx_alloc()` (SOFT) | `wd_soft_ctx` | `UADK_CTX_SOFT (3)`    | `wd_drv_soft_ctx_alloc()` 内部 |
| V1 用户预分配                         | `wd_ctx_h`    | 用户设置                   | 用户在 `wd_ctx.ctx_type` 中设置    |

**V1 注意事项**: V1 路径 `clone_ctx_to_internal()` (wd_util.c:213) 拷贝 `ctx->ctx_type` 到 `ctx_in->ctx_type`。用户需要正确设置外部 `wd_ctx.ctx_type = UADK_CTX_HW`。

---

## 第十章 改造文件与改动量

| 文件                        | 改动  | 行数                    | 说明                                                                                          |
| ------------------------- | --- | --------------------- | ------------------------------------------------------------------------------------------- |
| `include/wd_util.h`       | 修改  | +15                   | `wd_init_attrs` 新增 5 字段; `wd_ctx_spin_lock` 泛化签名                                            |
| `include/wd_alg.h`        | 修改  | +15                   | `wd_alg_driver` 新增 `alloc_ctx`/`free_ctx` 回调; 驱动节点操作函数声明                                         |
| `include/wd_alg_common.h` | 修改  | +10                   | `wd_drv_ctx_params` 结构体                                                                     |
| `include/wd_internal.h`   | 修改  | +10                   | ctx 首字段 `ctx_type`; `wd_ctx_config_internal` 扩展 (`drv_array`, `drv_count`)                               |
| `wd_alg.c`                | 修改  | +88                   | 新增 `wd_get_drv_array()` 驱动发现函数                                                                 |
| `wd_util.c`               | 重写  | +200/-200             | 重写 `wd_alg_attrs_init`; 新增 `wd_alg_ctx_init`, `wd_ctx_bind_drivers`; 删除 6 个旧函数        |
| `wd_bmm.c`                | 修改  | +15                   | `wd_ctx_is_hw()` + `wd_insert_ctx_list` 安全检测                                                |
| `wd_cipher.c`             | 修改  | +20/-20               | V1/V2 init 适配; uninit 清理                              |
| `drv/hisi_sec.c`          | 修改  | +2                    | `GEN_SEC_ALG_DRIVER` 宏新增 `alloc_ctx`/`free_ctx` 字段                                                               |
| **总计**                    | —   | **~360 新增 / ~200 删除** | **净增 ~160 行**                                                                               |

### 不在本次范围

| 项目                                                | 后续 Phase      |
| ------------------------------------------------- | ------------- |
| 其余 8 个算法模块 (digest/aead/comp/rsa/dh/ecc/agg/udma) | 与 cipher 同样模式 |
| 其余 7 个驱动的 `alloc_ctx`/`free_ctx` 适配               | 驱动改造 Phase    |
| 调度器 hash-bucket + 动态 slots[]                      | 调度器 Phase     |
| `wd_drv.c` 设备缓存 (`wd_drv_hw_ctx_alloc` 重写)        | 设备抽象 Phase    |
| V1 `wd_alg_drv_bind` 的 fallback 查找逻辑保留            | V1 向后兼容       |

---

## 第十一章 验证方案

### 11.1 编译验证

```bash
./cleanup.sh && ./autogen.sh && ./conf.sh && make -j$(nproc)
./cleanup.sh && ./autogen.sh && ./configure --with-static_drv && make -j$(nproc)
```

### 11.2 不变量检查

| #   | 不变量                                          | 验证方法                                      |
| --- | -------------------------------------------- | ----------------------------------------- |
| 1   | V1 `wd_cipher_init(config,sched)` 仍正常工作      | 运行现有 hisi_sec2 测试                         |
| 2   | `grep -r "wd_ctx_drv_config"` 返回空            | 在 uadk/ 下搜索                               |
| 3   | V2 `wd_cipher_init2_` 正确发现多驱动                | 日志打印 `drv_array` 内容                       |
| 4   | RR 分配比例正确                                   | 日志打印各驱动 ctx 数量: hisi_sec2=3, ce=2, soft=3 |
| 5   | `ctxs[].drv` 与 `drv_array` 对应一致             | 遍历验证 `ctxs[i].drv == drv_array[i % drv_count]` |
| 6   | `wd_cipher_alloc_sess("ecb(aes)")` compat 正确 | compat.count=6 (排除 2 个 CE ctx)            |
| 7   | `wd_cipher_alloc_sess("ecb(sm4)")` 全部兼容      | compat.count=8 (全部 ctx 都支持 sm4)           |
| 8   | 不兼容算法明确报错                                    | 请求不存在的算法，检查 `-WD_ENODEV`                  |
| 9   | `wd_insert_ctx_list(soft_ctx)` 安全跳过          | 传入 soft ctx，不崩溃                           |
| 10  | 锁机制正确                                | HW ctx send/recv 有锁，CE ctx 无锁             |
| 11  | 驱动引用计数正确                             | hisi_sec2 同时用于 cipher+digest 时 refcnt=2   |

### 11.3 回归测试

```bash
sudo -E ./test/sanity_test.sh
```

### 11.4 性能基准

```bash
uadk_tool benchmark --alg aes-128-ecb --mode sva --sync \
    --pktlen 4096 --seconds 5 --thread 1 --ctxnum 4

uadk_tool benchmark --alg aes-128-ecb --mode sva --sync \
    --pktlen 4096 --seconds 10 --thread 8 --ctxnum 32

uadk_tool benchmark --alg sm4-ecb --mode sva --sync \
    --pktlen 4096 --seconds 5 --thread 1 --ctxnum 4
```

---

## 第十二章 设计与实现差异总结 (2026-05-06 校验)

### 12.1 未实施的设计项

以下设计方案中提出的特性在当前代码中**未实施**，为设计预留项：

| 设计项 | 设计文档章节 | 当前状态 | 说明 |
|--------|-------------|---------|------|
| `wd_ctxs_unified_alloc()` | 4.4 | 函数名不同 | 实际函数名为 `wd_alg_ctx_init()` (wd_util.c:2490)，功能为简化的 RR 分配 |
| `wd_drv_alg_count()` | 4.3 | 未实施 | 覆盖广度统计函数未创建 |
| 四步覆盖优先分配算法 | 4.4 | 未实施 | 当前使用简化的 RR 分配 (`ctx_idx % drv_count`) |
| `compat_ctx_set` 结构体 | 3.1 | 已替换 | P7 方案已调整为调度器层 set_param + skey 方式，不再需要独立 compat_ctx_set 结构 |
| `wd_get_compat_ctxs()` | 5.4 | 已替换 | 原设计函数，P7 新方案改用 wd_alg_match_drv() + 调度器 skey 过滤实现 |
| `wd_drv_supports_alg()` | 5.4 | 已替换 | 功能已由 wd_alg_match_drv() (wd_alg.c:423) 实现 |
| `drv_region` 结构体 | 3.1 | 未实施 | 驱动分组元数据未采用，通过 `drv_array` 间接管理 |
| `need_lock` 驱动字段 | 3.1 | 未实施 | 锁策略由 ctx 层决定，不在驱动层 |
| `dev_cache` 驱动字段 | 3.1 | 未实施 | 设备缓存由 `wd_hw_alloc_ctx()` 内部管理 |
| `init_refcnt` 驱动字段 | 3.1 | 命名不同 | 引用计数通过 `wd_drv_node.refcnt` 管理 |

### 12.2 准确行号参考 (基于当前代码库, wd_util.c 共 2734 行)

| 文档引用 | 文档声称行号 | 实际 | 说明 |
|---------|-------------|------|------|
| `wd_ctxs_unified_alloc()` | wd_util.c:2971-3108 | **不存在** | 文件仅 2734 行，函数从未创建。实际功能由 `wd_alg_ctx_init()` (2490行) 提供 |
| `TODO: wd_ctxs_unified_alloc` | wd_util.c:3287 | **不存在** | 文件中无此 TODO 注释 |
| P0 Bug B1 (goto out_uninit_nolock) | wd_cipher.c:541 | **不存在** | 该行是 `continue;`，V2 init2 路径正确工作 |
| P0 Bug B2 (sess->compat) | wd_aead.c:336 | **不存在** | 搜索 `compat` 在 wd_aead.c 中无结果 |
| P0 Bug B3 (过时 TODO) | wd_util.c:3287 | **不存在** | 搜索 `TODO.*unified` 无结果 |

### 12.3 当前已知实际问题

1. **`wd_ctx_drv_deconfig()` 残留调用**: 该函数在 9 个模块中被调用 (aead/dh/agg/comp/ecc/digest/rsa/join_gather/udma)，但其函数体在 `wd_util.c:2187-2189` 为空实现。这些调用不产生运行时影响，但为代码清理项。

2. **P7 (Session 算法兼容性路由) 已有方案**: 原设计中的 `wd_get_compat_ctxs()` 和 `compat_ctx_set` 已被 `wd_alg_match_drv()` + 调度器 `set_param`/`skey` 方案取代 (详见 5.4 节)。等待在各 API 模块的 `alloc_sess` 中实施。

3. **`wd_alg_attrs_init()` 仅包含 Phase 1+2**: Phase 2.5 (`wd_ctx_bind_drivers()`) 和 Phase 3 (`wd_alg_init_driver()`) 由各 API 模块在 `wd_alg_attrs_init()` 返回后单独调用，而非在统一入口内部完成。

4. **V1 init 路径在各模块中模式一致但不统一进入**: cipher/digest/aead/comp/rsa/ecc/dh 的 V1 init 各自独立调用 `wd_get_drv_array()` + `wd_ctx_bind_drivers()` + `wd_alg_init_driver()`，而非通过统一入口。

### 12.4 实现状态总结 (2026-05-06)

| 类别 | 状态 | 说明 |
|------|------|------|
| P1-P6, P8-P10 (核心问题) | 已实现 | 见第一章各条目 |
| P7 (Session 兼容性) | 已有方案 | 通过 wd_alg_match_drv() + set_param/skey 实现 session compat 路由，见 5.4 节 |
| G1-G3, G5 (设计目标) | 已达成 | API 接口不变，v1/v2 复用框架函数 |
| G4 (Session 时按算法匹配 ctx) | 已有方案 | P7 方案已制定，待 API 模块 alloc_sess 中集成 set_param compat 过滤 |
| 框架层核心函数 | 已实现 | wd_alg_attrs_init, wd_alg_ctx_init, wd_ctx_bind_drivers, wd_alg_init_driver |
| 驱动层适配 (alloc_ctx/free_ctx) | 已实现 | 所有 8 个驱动均已适配 |
| API 层 V2 init 整合 | 已实现 | 所有模块 V2 init 均调用 wd_alg_attrs_init() |
| wd_ctx_drv_deconfig 残留 | 代码清理项 | 9 个模块调用空函数体 |

---

*文档完成更新 - 2026-05-06*
