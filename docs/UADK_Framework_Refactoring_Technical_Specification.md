# UADK 重构框架 — 技术说明书

> **版本:** 2.12
> **日期:** 2026-05-06 (基于当前代码实现，不含 v1/)
> **读者对象:** 了解重构后 UADK 框架的新开发者
> **范围:** 框架层 + API 层 + 驱动层

---

## 1. 概述与架构

### 1.1 UADK 是什么

UADK（User-space Accelerator Development Kit）是一个面向 ARM Linux 的用户态硬件加速框架。它在统一 API 背后抽象了**华为鲲鹏密码学/压缩加速器**和 **ARM ISA 扩展指令集**（CE、SVE）。应用程序调用诸如 `wd_do_cipher_sync()` 的函数时，无需了解任务实际是在硬件引擎、ARM 密码指令还是软件回退上执行的。

### 1.2 三层架构详解

```
+------------------------------------------------------------------+
|                     用户应用程序                                    |
|   wd_cipher_init() / wd_cipher_alloc_sess() / wd_do_cipher_sync() |
+------------------------------------------------------------------+
        |                              |
        |  wd_init_attrs              |  wd_cipher_msg
        v                              v
+------------------------------------------------------------------+
|  API 层（算法接口层）                                               |
|  wd_cipher.c  wd_digest.c  wd_aead.c  wd_comp.c  wd_rsa.c ...    |
|                                                                   |
|  职责：                                                            |
|  - Session 管理（分配/释放/设置密钥）                                |
|  - 同步/异步任务提交与完成轮询                                       |
|  - 构造 wd_init_attrs 并调用统一初始化流水线 (V2路径)                |
|  - V1 路径直接调用框架函数 (wd_get_drv_array + wd_ctx_bind_drivers) |
+------------------------------------------------------------------+
        |                              |
        |  wd_alg_attrs_init()        |  sched->pick_next_ctx()
        |  wd_ctx_bind_drivers()      |  ctx->drv->send()
        |  wd_alg_init_driver()       |
        v                              v
+------------------------------------------------------------------+
|  框架层（核心层）                                                  |
|                                                                   |
|  wd_alg.c    — 驱动注册与发现 (wd_get_drv_array)                   |
|  wd_util.c   — 统一初始化流水线 (Phase 1/2/2.5/3)                  |
|  wd_drv.c    — Ctx 分配与软队列辅助函数                              |
|  wd_sched.c  — 调度器实现 (RR/LOOP/HUNGRY/...)                     |
|  wd.c        — UACCE 封装 (sysfs、ctx open/mmap/ioctl/poll)        |
|  wd_mempool.c — 内存池管理                                         |
|  wd_bmm.c    — 缓冲内存管理 (No-SVA 安全检查)                      |
+------------------------------------------------------------------+
        |                              |
        |  wd_alg_driver_register()   |  drv->send() / drv->recv()
        |  drv->init() / drv->exit()  |  drv->alloc_ctx()
        v                              v
+------------------------------------------------------------------+
|  驱动层                                                            |
|                                                                   |
|  libhisi_sec.so  — 华为安全引擎 (cipher/aead/digest)               |
|  libhisi_hpre.so — 华为 HPRE (RSA/ECC/DH)                        |
|  libhisi_zip.so  — 华为 ZIP (压缩)                                |
|  libhisi_dae.so  — 华为 DAE (数据加速引擎)                         |
|  libhisi_udma.so — 华为 UDMA                                      |
|  libisa_ce.so    — ARM 密码扩展指令集 (SM3/SM4)                   |
|  libisa_sve.so   — ARM SVE 多缓冲哈希 (SM3/MD5)                   |
+------------------------------------------------------------------+
        |
        |  ioctl / mmap on /dev/<uacce_device>
        v
+------------------------------------------------------------------+
|  Linux 内核 (UACCE 子系统)                                         |
|  /sys/class/uacce/<dev>/  +  /dev/<dev>                           |
+------------------------------------------------------------------+
```

| 层级 | 包含库 | 核心职责 |
|------|--------|----------|
| **API 层** | `libwd_crypto`、`libwd_comp`、`libwd_dae`、`libwd_udma` | 提供面向用户的 session 式 API，管理密钥/上下文，分发同步/异步请求 |
| **框架层** | `libwd` | 设备发现、ctx 分配、驱动绑定、调度策略、内存管理、初始化流水线编排 |
| **驱动层** | `libhisi_sec.so` 等 | 硬件/指令集的具体实现，通过 `wd_alg_driver_register()` 向框架层注册回调 |
| **内核层** | UACCE 驱动子系统 | 提供 `/dev/uacce-X` 字符设备和 `/sys/class/uacce/` sysfs 属性 |

### 1.3 核心设计原则

1. **统一初始化流水线** — V2 路径收敛到 `wd_alg_attrs_init()`（wd_util.c:2699），封装 Phase 1 (驱动发现) + Phase 2 (ctx 分配)。V1 路径直接调用各个框架函数（`wd_get_drv_array()` + `wd_ctx_bind_drivers()` + `wd_alg_init_driver()`）。

2. **驱动注册模式** — 所有驱动通过 `wd_alg_driver_register()` 注册到全局链表。框架在初始化时通过 `wd_get_drv_array()` (wd_alg.c:673) 发现可用驱动。

3. **多驱动轮询绑定** — 多个驱动（HW、CE、SVE、SOFT）可以绑定到同一个 ctx 池。`wd_ctx_bind_drivers()` (wd_util.c:2229) 通过轮询（Round-Robin）方式将驱动分配给上下文。

4. **NUMA 感知** — 调度器域具有 NUMA 感知能力。框架会选择最靠近请求 CPU 节点的加速器。

5. **驱动注册自动发现** — 驱动通过 `__attribute__((constructor))` 自动注册到全局链表，无需手动配置。

### 1.4 执行模式

| 模式 | API | 行为 |
|------|-----|------|
| **同步（Sync）** | `wd_do_cipher_sync()` | 提交任务，自旋等待完成，返回结果 |
| **异步（Async）** | `wd_do_cipher_async()` + `wd_cipher_poll()` | 提交并立即返回；稍后通过轮询获取完成结果 |

### 1.5 重构改动总览

| 维度 | 旧方案 | 新方案 |
|------|--------|--------|
| 初始化入口 | 两条独立代码路径（V1 vs V2） | V2: `wd_alg_attrs_init()` 统一入口；V1: 直接调用框架函数 |
| 驱动绑定 | 逐 ctx 在 `wd_ctx_drv_config()` 中 | 集中化：Phase 2.5 `wd_ctx_bind_drivers()` (wd_util.c:2229) |
| 驱动发现 | 隐式通过 dlopen | 显式：Phase 1 `wd_get_drv_array()` (wd_alg.c:673) |
| 队列辅助函数 | 在 `wd_util.c` / `wd_util.h` 中 | 移到 `wd_drv.c` / `wd_drv.h` |
| Ctx 分配 | 每个驱动硬编码 | 统一：`wd_drv_hw_ctx_alloc()` / `wd_drv_soft_ctx_alloc()` |
| 驱动初始化 | 逐 ctx 遍历 | `wd_alg_init_driver()` 逐 ctx 调用 `wd_ctx_init_driver()` |
| alloc_ctx/free_ctx | 无 | 每个驱动设置统一回调接口 |

### 1.6 实现状态

当前代码实现状态：

| 组件 | 状态 | 说明 |
|------|------|------|
| **框架层核心** | 完成 | Phase 1/2/2.5/3 完整实现于 wd_util.c |
| **驱动层适配** | 完成 | 所有 8 个驱动已设置 alloc_ctx/free_ctx |
| **V2 路径 API 整合** | 完成 | 全部 12 个 API 模块调用 `wd_alg_attrs_init()` |
| **V1 路径 API 整合** | 部分完成 | wd_cipher.c 已整合；digest/aead/comp/rsa/ecc/dh 仍使用旧式 `wd_ctx_drv_config()` |
| **wd_ctx_drv_deconfig 清理** | 部分完成 | wd_cipher.c 已清理；其余 10 个模块有残余调用（空 stub，无功能影响） |
| **Session 兼容性路由** | 已有方案 | 通过 wd_alg_match_drv() + 调度器 set_param/skey，详见 Session 生命周期章节 |

---

## 2. 数据结构参考

### 2.1 `wd_init_attrs` — 统一初始化参数载体

**文件：** `include/wd_alg_common.h` 第 190-203 行

```c
struct wd_init_attrs {
    __u32 sched_type;                     // SCHED_POLICY_RR / NONE / LOOP 等
    __u32 task_type;                      // TASK_HW / TASK_MIX / TASK_INSTR
    char alg[CRYPTO_MAX_ALG_NAME];        // 算法名，例如 "cbc(aes)"

    /* Phase 2 期间填充 */
    struct wd_sched *sched;               // 分配的调度器实例
    struct wd_ctx_config *ctx_config;     // 分配的 ctx 配置（V2 路径）

    /* 用户提供 */
    struct wd_ctx_params *ctx_params;     // V2：每操作类型 ctx 数量 + NUMA 掩码
    wd_alg_init alg_init;                  // 模块回调（如 wd_cipher_common_init）
    wd_alg_poll_ctx alg_poll_ctx;          // 模块轮询回调

    /* V1 预填充 / V2 分配 */
    struct wd_ctx_config_internal *ctx_config_internal;

    /* Phase 1 输出 */
    struct wd_alg_driver **drv_array;     // 发现到的驱动数组
    __u32 drv_count;                      // 发现的驱动数量
};
```

**关键使用模式：**

| 字段 | V1 值 (wd_cipher.c) | V2 值 (wd_cipher.c) |
|------|---------------------|---------------------|
| `ctx_config_internal` | 不通过 attrs（直接操作 setting.config） | `NULL`（Phase 2 内部分配，由 alg_init 回调填充） |
| `ctx_params` | 不通过 attrs（用户提供 config/sched） | 指向用户的 `wd_ctx_params` |
| `task_type` | `TASK_HW`（固定，传参给 wd_get_drv_array） | 用户指定（HW/MIX/INSTR） |
| `wd_get_drv_array()` 的 `drv_name` 参数 | "hisi_sec2"（精确匹配） | NULL（匹配全部驱动） |

**注意：** V1 路径 (wd_cipher.c:401) 当前**不**使用 `wd_alg_attrs_init()`，而是直接调用 `wd_get_drv_array()` + `wd_ctx_bind_drivers()` + `wd_alg_init_driver()`。只有 V2 路径使用 `wd_alg_attrs_init()`。

### 2.2 `wd_alg_driver` — 驱动抽象

**文件：** `include/wd_alg.h` 第 138-159 行

```c
struct wd_alg_driver {
    /* 标识 */
    const char *drv_name;                 // "hisi_sec2", "isa_ce_sm4"
    const char *alg_name;                 // "ecb(aes)", "cbc(sm4)", ...
    int priority;                         // HW=100, CE=400, SOFT=0
    int calc_type;                        // UADK_ALG_HW / CE_INSTR / SVE_INSTR / SOFT

    /* 资源需求 */
    int queue_num;                        // 每个 ctx 需要的 HW 队列对数
    int op_type_num;                      // 操作子类型数量
    int priv_size;                        // sizeof(驱动的私有上下文结构体)
    int *drv_data;                        // 指向驱动私有数据的不透明指针

    /* 回退链 */
    handle_t fallback;                    // 指向软件回退驱动的句柄（或 0）

    /* 生命周期状态 */
    int init_state;                       // 0=未初始化，1=已初始化

    /* 必需回调 */
    int  (*init)(void *conf, void *priv);
    void (*exit)(void *priv);
    int  (*send)(handle_t ctx, void *drv_msg);
    int  (*recv)(handle_t ctx, void *drv_msg);
    int  (*get_usage)(void *param);
    int  (*get_extend_ops)(void *ops);

    /* Ctx 生命周期回调 */
    int  (*alloc_ctx)(char *alg_name, void *params, handle_t *ctx);
    void (*free_ctx)(handle_t ctx);
};
```

| 值 | 枚举 | 示例驱动 | alloc_ctx |
|----|------|----------|-----------|
| 0 | `UADK_ALG_HW` | hisi_sec, hisi_hpre, hisi_comp | `wd_drv_hw_ctx_alloc` |
| 1 | `UADK_ALG_CE_INSTR` | isa_ce_sm3, isa_ce_sm4 | `wd_drv_soft_ctx_alloc` |
| 2 | `UADK_ALG_SVE_INSTR` | hash_mb | `wd_drv_soft_ctx_alloc` |
| 3 | `UADK_ALG_SOFT` | soft cipher/digest | `wd_drv_soft_ctx_alloc` |

### 2.3 `wd_ctx_config_internal` — 内部上下文配置

**文件：** `include/wd_internal.h` 第 45-56 行

```c
struct wd_ctx_config_internal {
    __u32 ctx_num;
    int shmid;
    struct wd_ctx_internal *ctxs;
    void *priv;
    bool epoll_en;
    unsigned long *msg_cnt;
    char *alg_name;

    /* 多驱动元数据 */
    struct wd_alg_driver **drv_array;     // Phase 1 发现的驱动
    __u32 drv_count;                      // 驱动数量
};
```

### 2.4 `wd_ctx_internal` — 内部上下文描述符

**文件：** `include/wd_internal.h` 第 32-43 行

```c
struct wd_ctx_internal {
    __u8 op_type;
    __u8 ctx_mode;                    // CTX_MODE_SYNC 或 CTX_MODE_ASYNC
    __u8 ctx_type;                    // UADK_CTX_HW / CE_INS / SVE_INS / SOFT
    __u8 ctx_used;
    handle_t ctx;                     // → wd_ctx_h* 或 wd_soft_ctx*
    __u16 sqn;                        // 调度器队列号
    pthread_spinlock_t lock;
    struct wd_alg_driver *drv;        // 拥有此 ctx 的驱动 (Phase 2.5 设置)
    void *drv_priv;
    void *extend_ops;
};
```

**关键不变量：** `ctxs[i].drv` 在 Phase 2.5 期间由 `wd_ctx_bind_drivers()` (wd_util.c:2229) **一次性**设置，之后在整个驱动生命周期中**只读**。它是从 ctx 到驱动 send/recv 分发的链接：`ctxs[idx].drv->send(ctxs[idx].ctx, msg)`。

### 2.5 `wd_ctx_h` — HW 上下文

**文件：** `include/wd_internal.h` 第 21-30 行

```c
struct wd_ctx_h {
    int fd;                             // 首位字段
    char dev_path[MAX_DEV_NAME_LEN];
    char *dev_name;
    char *drv_name;
    unsigned long qfrs_offs[UACCE_QFRT_MAX];
    void *qfrs_base[UACCE_QFRT_MAX];
    struct uacce_dev *dev;
    void *priv;
};
```

### 2.6 No-SVA 安全检查

`wd_ctx_is_hw()` 函数检查 `wd_ctx_internal.ctx_type` 字段（非 `wd_ctx_h` 结构体）。类型字段位于 `wd_ctx_internal` 中，在 `ctxs[i].ctx` 所指向内存中通过直接转换访问。

---

## 3. 初始化流水线

### 3.1 流水线总览

统一初始化流水线分为 4 个阶段：

- **Phase 1 — 驱动发现：** 扫描全局驱动注册链表，按算法类型和任务类型过滤，返回去重后的驱动数组。
- **Phase 2 — Ctx 分配：** 计算 ctx 总数，RR 方式调用 `drv->alloc_ctx()` 分配硬件队列或软队列，创建调度器。
- **Phase 2.5 — 驱动绑定：** RR 方式将每个 ctx 绑定到驱动，设置 HW fallback，缓存驱动数组，递增引用计数。
- **Phase 3 — 驱动初始化：** 遍历所有 ctx，调用每个驱动的 `init()` 回调。

V2 路径中，Phase 1 和 Phase 2 封装在 `wd_alg_attrs_init()` (wd_util.c:2699) 中。Phase 2.5 和 Phase 3 由调用者执行。V1 路径直接调用各个框架函数。

### 3.2 Phase 1：驱动发现

**函数：** `wd_alg_drv_discover()` (wd_util.c:2311) → `wd_get_drv_array()` (wd_alg.c:673)

```c
int wd_get_drv_array(const char *alg_type,      // "cipher", "digest", ...
                     int task_type,              // TASK_HW / TASK_MIX / TASK_INSTR
                     char *drv_name,             // V1="hisi_sec2", V2=NULL
                     struct wd_alg_driver ***drv_array_out,
                     __u32 *drv_count_out);
```

**算法流程：**
1. 分配初始容量（基于 `alg_registry.drv_type_num`）的动态数组
2. 遍历全局 `wd_drv_node` 链表：
   - 按 `alg_type` 匹配
   - 按 `task_type` 过滤（`TASK_HW` → 仅 `UADK_ALG_HW`；`TASK_INSTR` → 排除 `UADK_ALG_HW`；`TASK_MIX` → 全部）
   - 按 `drv_name` 过滤（或 NULL 表示全部）
3. 通过 `wd_alg_drv_type_match()` 进行类型匹配
4. 在 `*drv_array_out` 和 `*drv_count_out` 中返回去重后的驱动数组及数量

**V1 结果：** `[hisi_sec2 cipher_driver]`，count=1
**V2 结果：** `[hisi_sec2 cipher, isa_ce_sm4 cipher, ...]`，count=1+

### 3.3 Phase 2：上下文分配

**函数：** `wd_alg_ctx_init()` (wd_util.c:2490)

**工作流程：**

1. 计算 sync/async ctx 总数
2. 分配 `wd_ctx_config` 和 `wd_ctx` 数组
3. **RR 分配循环**：对每个 ctx_idx，选择 `drv_array[ctx_idx % drv_count]`，调用 `drv->alloc_ctx()`
4. 创建调度器实例（`wd_sched_rr_alloc`）
5. 调用 `attrs->alg_init` 回调（如 `wd_cipher_common_init`），将配置深拷贝到模块内部

**注意：** `ctxs[i].drv` 在此阶段保持为 NULL，由 Phase 2.5 设置。`ctxs[i].ctx` 在此阶段由 `drv->alloc_ctx()` 填充。

### 3.4 Phase 2.5：驱动绑定

**函数：** `wd_ctx_bind_drivers()` (wd_util.c:2229)

```c
int wd_ctx_bind_drivers(struct wd_ctx_config_internal *config,
                        struct wd_alg_driver **drv_array, __u32 drv_count)
{
    for (i = 0; i < config->ctx_num; i++) {
        if (drv_count == 1) {
            config->ctxs[i].drv = drv_array[0];
        } else {
            config->ctxs[i].drv = drv_array[i % drv_count];
        }
        config->ctxs[i].ctx_type = config->ctxs[i].drv->calc_type;

        /* HW 驱动设置软回退 */
        if (config->ctxs[i].ctx_type == UADK_ALG_HW) {
            if (!drv->fallback)
                drv->fallback = wd_request_drv(config->alg_name, ALG_DRV_SOFT);
        }
    }

    /* 缓存 + 去重引用计数递增 */
    config->drv_array = drv_array;
    config->drv_count = drv_count;
    wd_alg_drv_ref_inc(drv_array, drv_count);
}
```

**这是 `ctxs[i].drv` 在整个生命周期中的唯一写入点。**

### 3.5 Phase 3：驱动初始化

**函数：** `wd_alg_init_driver()` (wd_util.c:1650)

```c
int wd_alg_init_driver(struct wd_ctx_config_internal *config)
{
    for (i = 0; i < config->ctx_num; i++) {
        if (!config->ctxs[i].ctx)
            continue;
        ret = wd_ctx_init_driver(config, &config->ctxs[i]);
        if (ret)
            goto init_err;
    }
    return 0;

init_err:
    for (j = 0; j < i; j++)
        wd_ctx_uninit_driver(config, &config->ctxs[j]);
    return ret;
}
```

逐 ctx 遍历，通过 `wd_ctx_init_driver()` 对每个 ctx 调用 `drv->init()`。`drv->init_state` 标志位保证每个驱动仅初始化一次。

### 3.6 反初始化流程

**函数：** `wd_alg_ctxs_uninit()` (wd_util.c:2656)，`wd_alg_attrs_uninit()` (wd_util.c:2680)

反初始化流程：
1. `wd_alg_uninit_driver()` — 调用 `drv->exit()` 释放 HW 资源
2. `wd_ctx_unbind_drivers()` — 递减驱动引用计数，清空 `ctxs[i].drv`
3. `wd_alg_ctxs_uninit()` — 调用 `drv->free_ctx()` 释放 ctx，释放 ctxs 数组
4. 释放调度器
5. `wd_alg_drv_undiscover()` — 释放 `drv_array`

---

## 4. V1 vs V2 详细对比

### 4.1 V1 路径：`wd_cipher_init()`

**文件：** `wd_cipher.c` 第 401-465 行

```c
int wd_cipher_init(struct wd_ctx_config *config, struct wd_sched *sched);
```

**逐步流程：**

| 步骤 | 动作 | 代码位置 |
|------|------|---------|
| 1 | 注册 `atfork` 处理器 | wd_cipher.c:406 |
| 2 | 原子 CAS init 状态检查 | wd_cipher.c:408 |
| 3 | 验证 config 和 sched 非空 | wd_cipher.c:412 |
| 4 | `dlopen("libhisi_sec.so")` → 驱动构造函数注册 | wd_cipher.c:416 |
| 5 | `wd_cipher_common_init()` → 深拷贝 config，初始化 sched | wd_cipher.c:421 |
| 6 | `wd_get_drv_array("cipher", TASK_HW, "hisi_sec2", ...)` → Phase 1 | wd_cipher.c:426 |
| 7 | `wd_ctx_bind_drivers()` → Phase 2.5 | wd_cipher.c:435 |
| 8 | `wd_alg_init_driver()` → Phase 3 | wd_cipher.c:443 |
| 9 | `wd_alg_set_init()` → status = `WD_INIT` | wd_cipher.c:449 |

**注意：** V1 路径**不使用** `wd_alg_attrs_init()`。它直接调用各框架函数。

**V1 反初始化 (wd_cipher.c:467-483)：**
```c
wd_alg_uninit_driver(&wd_cipher_setting.config);  // drv->exit()
wd_ctx_unbind_drivers(&wd_cipher_setting.config);  // refcnt 递减
wd_cipher_common_uninit();                          // pool + sched 清理
wd_put_drv_array(...);                              // 释放 drv_array
wd_cipher_close_driver(WD_TYPE_V1);                 // dlclose(libhisi_sec.so)
wd_alg_clear_init(&wd_cipher_setting.status);       // 重置状态
```

### 4.2 V2 路径：`wd_cipher_init2_()`

**文件：** `wd_cipher.c` 第 485-582 行

```c
int wd_cipher_init2_(char *alg, __u32 sched_type, int task_type,
                     struct wd_ctx_params *ctx_params);
```

**逐步流程：**

| 步骤 | 动作 | 代码位置 |
|------|------|---------|
| 1 | 注册 `atfork` 处理器 | wd_cipher.c:492 |
| 2 | 原子 CAS init 状态检查 | wd_cipher.c:494 |
| 3 | 验证 alg、sched_type、task_type 参数 | wd_cipher.c:498-502 |
| 4 | 算法名有效性检查 | wd_cipher.c:504 |
| 5 | `wd_cipher_open_driver(WD_TYPE_V2)` → 驱动加载 | wd_cipher.c:510 |
| 6 | **重试循环** — `while (ret != 0)` | wd_cipher.c:514-546 |
| 6a | `wd_ctx_param_init()` → 合并默认值 | wd_cipher.c:520 |
| 6b | 填充 `wd_cipher_init_attrs` | wd_cipher.c:529-535 |
| 6c | `wd_alg_attrs_init()` → Phase 1 + Phase 2 | wd_cipher.c:537 |
| 6d | 遇到 `-WD_ENODEV`：减少 ctx 数量重试 | wd_cipher.c:539-541 |
| 7 | `wd_ctx_bind_drivers()` → Phase 2.5 | wd_cipher.c:550 |
| 8 | `wd_alg_init_driver()` → Phase 3 | wd_cipher.c:559 |
| 9 | `wd_alg_set_init()` → status = `WD_INIT` | wd_cipher.c:565 |

**V2 反初始化 (wd_cipher.c:584-599)：**
```c
wd_alg_uninit_driver(&wd_cipher_setting.config);   // drv->exit()
wd_ctx_unbind_drivers(&wd_cipher_setting.config);   // refcnt 递减
wd_cipher_common_uninit();                           // pool + sched 清理
wd_alg_attrs_uninit(&wd_cipher_init_attrs);          // 释放 ctx_config, sched
wd_cipher_close_driver(WD_TYPE_V2);                  // dlclose 所有 .so
wd_alg_clear_init(&wd_cipher_setting.status);        // 重置状态
```

**关键区别：** V2 调用 `wd_alg_attrs_uninit()` 释放框架层在 Phase 2 中分配的 ctx_config 和调度器。V1 不调用（ctx 是用户分配和管理的）。

### 4.3 V1 vs V2 全面对比

| 维度 | V1（`wd_cipher_init`） | V2（`wd_cipher_init2_`） |
|------|-----------------------|---------------------------|
| **API 签名** | `(config, sched)` | `(alg, sched_type, task_type, ctx_params)` |
| **驱动发现** | Phase 1 查 "hisi_sec2" 1 个驱动 | Phase 1 查所有 cipher 驱动 |
| **Ctx 分配** | 用户通过 `wd_request_ctx()` 预分配 | Phase 2 期间框架自动分配 |
| **调度器** | 用户提供 `struct wd_sched*` | 框架通过 `wd_sched_rr_alloc()` 创建 |
| **多驱动** | 否 — 仅单个 hisi_sec2 | 是 — HW + CE + SVE + SOFT |
| **失败重试** | 否 — 立即失败 | 是 — `while(ret != 0)` 循环带重试 |
| **驱动加载** | `dlopen("libhisi_sec.so")` | `wd_dlopen_drv(NULL)` — 扫描所有 .so |
| **配置内存** | 用户管理 | 框架管理（在 uninit 中释放） |
| **使用 wd_alg_attrs_init** | 否 | 是 (line 537) |

---

## 5. Session 生命周期

Session 是 UADK 中用户与算法交互的核心对象。每个 session 代表一个算法上下文（如 "使用 AES-ECB 模式加密"），包含密钥和调度器键。

**当前方案 (coral.md)：** Session 兼容性路由通过以下链路实现：`ctx_id → ctxs[idx].drv → wd_alg_match_drv(drv, alg_name)`。

**数据流**: sched_init 预取 ctx（无过滤） → set_param 传入 `alg_name` + `ctxs` → `wd_sched_skey_compat_filter()` 遍历 domain cache 替换不兼容 ctx → pick_next_ctx 返回兼容 ctx。

**核心函数**: `wd_alg_match_drv()` (wd_alg.c:423) 已实现；`wd_sched_set_param()` 存储 compat 信息并触发过滤；`session_sched_init_ctx()` 扩展 skey 参数支持 RR compat 查找。

**涉及的 10 个 API 模块**: wd_cipher.c:267, wd_digest.c:205, wd_aead.c:460, wd_comp.c:457, wd_rsa.c:931, wd_ecc.c:1199, wd_dh.c:577, wd_agg.c:425, wd_join_gather.c:497, wd_udma.c:74。

---

## 6. 驱动集成指南

### 6.1 驱动注册检查清单

要添加新驱动，实现以下步骤：

1. **定义驱动结构体**：
   ```c
   static struct wd_alg_driver my_driver = {
       .drv_name   = "my_driver",
       .alg_name   = "ecb(aes)",
       .calc_type  = UADK_ALG_HW,
       .priority   = 100,
       .priv_size  = sizeof(struct my_drv_ctx),
       .queue_num  = 1,
       .op_type_num = 1,
       .fallback   = 0,
       .init       = my_drv_init,
       .exit       = my_drv_exit,
       .send       = my_drv_send,
       .recv       = my_drv_recv,
       .get_usage  = my_drv_get_usage,
       .alloc_ctx  = wd_drv_hw_ctx_alloc,    // 或 wd_drv_soft_ctx_alloc
       .free_ctx   = wd_drv_hw_ctx_free,     // 或 wd_drv_soft_ctx_free
   };
   ```

2. **在构造函数中注册**：
   ```c
   wd_alg_driver_register(&my_driver);
   ```

3. **在析构函数中注销**：
   ```c
   wd_alg_driver_unregister(&my_driver);
   ```

### 6.2 驱动层实现状态

所有驱动已正确实现 alloc_ctx/free_ctx 回调：

| 驱动 | 文件 | alloc_ctx | free_ctx | 类型 |
|------|------|-----------|----------|------|
| hisi_sec | `drv/hisi_sec.c` | `wd_drv_hw_ctx_alloc` | `wd_drv_hw_ctx_free` | HW |
| hisi_hpre | `drv/hisi_hpre.c` | `wd_drv_hw_ctx_alloc` | `wd_drv_hw_ctx_free` | HW |
| hisi_comp | `drv/hisi_comp.c` | `wd_drv_hw_ctx_alloc` | `wd_drv_hw_ctx_free` | HW |
| hisi_dae | `drv/hisi_dae.c` | `wd_drv_hw_ctx_alloc` | `wd_drv_hw_ctx_free` | HW |
| hisi_udma | `drv/hisi_udma.c` | `wd_drv_hw_ctx_alloc` | `wd_drv_hw_ctx_free` | HW |
| isa_ce_sm3 | `drv/isa_ce_sm3.c` | `wd_drv_soft_ctx_alloc` | `wd_drv_soft_ctx_free` | CE |
| isa_ce_sm4 | `drv/isa_ce_sm4.c` | `wd_drv_soft_ctx_alloc` | `wd_drv_soft_ctx_free` | CE |
| hash_mb | `drv/hash_mb/hash_mb.c` | `wd_drv_soft_ctx_alloc` | `wd_drv_soft_ctx_free` | SVE |

### 6.3 辅助函数

**文件：** `wd_drv.c`

| 函数 | 作用 |
|------|------|
| `wd_drv_hw_ctx_alloc()` | HW ctx 分配（UACCE 设备打开） |
| `wd_drv_hw_ctx_free()` | HW ctx 释放 |
| `wd_drv_soft_ctx_alloc()` | Soft/CE/SVE ctx 分配（环形缓冲区） |
| `wd_drv_soft_ctx_free()` | Soft ctx 释放 |

---

## 7. API 层模块整合状态

### 7.1 V2 路径整合状态

所有 API 模块的 V2 路径已完整调用 `wd_alg_attrs_init()`：

| 模块 | `wd_alg_attrs_init` 调用行号 |
|------|---------------------------|
| wd_cipher.c | line 537 |
| wd_digest.c | line 438 |
| wd_aead.c | line 744 |
| wd_comp.c | line 300 |
| wd_rsa.c | line 308 |
| wd_ecc.c | line 335 |
| wd_dh.c | line 268 |
| wd_agg.c | line 715 |
| wd_udma.c | line 470 |
| wd_join_gather.c | line 802 |

### 7.2 V1 路径整合状态

| 模块 | V1 路径 | 整合方式 |
|------|---------|----------|
| wd_cipher.c | `wd_get_drv_array()` + `wd_ctx_bind_drivers()` + `wd_alg_init_driver()` | 完全整合 |
| wd_digest.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` | 旧式（wd_ctx_drv_config 是空 stub） |
| wd_aead.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` | 旧式 |
| wd_comp.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` | 旧式 |
| wd_rsa.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` | 旧式 |
| wd_ecc.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` | 旧式 |
| wd_dh.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` | 旧式 |

### 7.3 wd_ctx_drv_deconfig 残留

`wd_ctx_drv_deconfig()` (wd_util.c:2187-2189) 函数体为空（no-op stub）。以下模块仍有残余调用（无功能影响）：

| 文件 | 调用次数 |
|------|---------|
| wd_aead.c | 4 |
| wd_digest.c | 4 |
| wd_comp.c | 4 |
| wd_rsa.c | 4 |
| wd_ecc.c | 4 |
| wd_dh.c | 4 |
| wd_udma.c | 2 |
| wd_agg.c | 2 |
| wd_join_gather.c | 2 |

wd_cipher.c 已完全移除这些调用。

---

## 8. 文件地图与引用链

### 8.1 完整文件清单

| 文件 | 层级 | 角色 |
|------|------|------|
| `include/wd.h` | 基础 | 错误码、内存操作、UACCE 类型、`handle_t` |
| `include/wd_alg.h` | 框架 | `wd_alg_driver`、驱动注册 API、`wd_get_drv_array` |
| `include/wd_alg_common.h` | 框架 | `wd_init_attrs`、`wd_ctx`、`wd_ctx_config`、`wd_sched`、所有枚举 |
| `include/wd_internal.h` | 框架 | `wd_ctx_internal`、`wd_ctx_config_internal`、`wd_ctx_h` |
| `include/wd_util.h` | 框架 | 初始化/反初始化函数声明、环境配置 |
| `include/wd_drv.h` | 框架 | `wd_drv_hw/soft_ctx_alloc/free` |
| `include/wd_sched.h` | 框架 | 调度器虚函数表、`wd_sched_params` |
| `wd_alg.c` | 框架实现 | 驱动注册链表、`wd_get_drv_array` (line 673) |
| `wd_util.c` | 框架实现 | `wd_alg_attrs_init` (line 2699)、`wd_ctx_bind_drivers` (line 2229)、`wd_alg_init_driver` (line 1650) |
| `wd_drv.c` | 框架实现 | ctx 分配 + 软队列辅助函数 |
| `wd_sched.c` | 框架实现 | RR/LOOP/HUNGRY/DEV 调度器实现 |
| `wd.c` | 框架实现 | UACCE 封装 |
| `wd_cipher.c` | API 实现 | V1/V2 初始化、session 分配/释放、同步/异步分发 |

### 8.2 关键函数参考（基于当前代码）

| 函数 | 文件 | 行号 | 角色 |
|------|------|------|------|
| `wd_alg_attrs_init()` | wd_util.c | 2699 | V2 统一初始化入口：Phase 1 + Phase 2 |
| `wd_alg_drv_discover()` | wd_util.c | 2311 | Phase 1：驱动发现 |
| `wd_alg_ctx_init()` | wd_util.c | 2490 | Phase 2：ctx 分配 + 调度器创建 |
| `wd_ctx_bind_drivers()` | wd_util.c | 2229 | Phase 2.5：RR 绑定 + HW fallback |
| `wd_alg_init_driver()` | wd_util.c | 1650 | Phase 3：驱动初始化 |
| `wd_alg_ctxs_uninit()` | wd_util.c | 2656 | ctx 清理 + refcnt 递减 |
| `wd_alg_attrs_uninit()` | wd_util.c | 2680 | V2 清理：释放 ctx + 调度器 |
| `wd_get_drv_array()` | wd_alg.c | 673 | 遍历驱动注册表，按类型匹配 |
| `wd_drv_hw_ctx_alloc()` | wd_drv.c | 19 | HW ctx 分配 |
| `wd_drv_soft_ctx_alloc()` | wd_drv.c | 66 | ISA/软 ctx 分配 |
| `wd_cipher_init()` | wd_cipher.c | 401 | V1 初始化入口 |
| `wd_cipher_init2_()` | wd_cipher.c | 485 | V2 初始化入口 |

### 8.3 设计预留（代码中不存在的函数）

| 函数名 | 设计文档中的用途 |
|--------|-----------------|
| `wd_get_compat_ctxs()` | 已替换 — P7 方案使用 wd_alg_match_drv() + skey 过滤（见 Session 生命周期） |
| `wd_drv_alg_count()` | 统计每个驱动每种类型的算法数 |
| `wd_drv_supports_alg()` | 检查驱动是否支持特定算法名 |

---

## 9. 当前已知问题

### 9.1 V1 路径未统一

wd_digest.c、wd_aead.c、wd_comp.c、wd_rsa.c、wd_ecc.c、wd_dh.c 的 V1 init 路径仍使用旧式 `wd_ctx_drv_config()` + `wd_alg_init_driver()` 模式，未通过 `wd_get_drv_array()` 进行驱动发现，也未通过 `wd_ctx_bind_drivers()` 进行 Round-Robin 绑定。

### 9.2 wd_ctx_drv_config/deconfig 空 stub 调用

`wd_ctx_drv_config()` (wd_util.c:2183-2185) 和 `wd_ctx_drv_deconfig()` (wd_util.c:2187-2189) 函数体分别为 `return 0` 和空，但在 10 个模块中仍有 30 处残余调用。

### 9.3 Session 兼容性路由

P7 方案已通过 coral.md 制定：使用 `wd_alg_match_drv()` + `sched_set_param`/`skey` 机制替代原 `compat_ctx_set`/`wd_get_compat_ctxs()` 方案。

---

## 10. 附录

### 10.1 错误码

**文件：** `include/wd.h`

| 代码 | 值 | 含义 |
|------|----|------|
| `WD_SUCCESS` | 0 | 操作成功 |
| `WD_EAGAIN` | 11 | 请重试 |
| `WD_ENOMEM` | 12 | 内存不足 |
| `WD_EBUSY` | 16 | 设备或资源忙 |
| `WD_ENODEV` | 19 | 没有这样的设备 |
| `WD_EINVAL` | 22 | 无效参数 |
| `WD_ETIMEDOUT` | 110 | 操作超时 |

### 10.2 上下文属性码

| 枚举 | 值 | 上下文类型 |
|------|----|-----------|
| `UADK_CTX_HW` | 0x0 | 硬件加速器（UACCE） |
| `UADK_CTX_CE_INS` | 0x1 | ARM 密码扩展指令 |
| `UADK_CTX_SVE_INS` | 0x2 | ARM SVE 指令 |
| `UADK_CTX_SOFT` | 0x3 | 纯软件 |

### 10.3 调度器策略

| 枚举 | 值 | 描述 |
|------|----|------|
| `SCHED_POLICY_RR` | 0 | 在区域内的所有 ctx 之间轮询 |
| `SCHED_POLICY_NONE` | 1 | 单 ctx，无调度 |
| `SCHED_POLICY_SINGLE` | 2 | 固定 ctx |
| `SCHED_POLICY_DEV` | 3 | 基于设备 ID 的调度 |
| `SCHED_POLICY_LOOP` | 4 | 在 HW 和 ISA ctx 之间交替 |
| `SCHED_POLICY_HUNGRY` | 5 | 每 session 键平衡 |
| `SCHED_POLICY_INSTR` | 6 | 仅指令加速 |

---

> **本文档版本：** 2.12
> **更新日期：** 2026-05-06
> **审查状态：** 基于实际代码验证更新
