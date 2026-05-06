# UADK 框架重构：现状分析与完整设计方案

> **日期**: 2026-05-06 (基于当前代码实现，不含 v1/)
> **基于**: 完整源代码验证 (wd_util.c 2734行, wd_alg.c, wd_cipher.c 等)
> **目的**: 对照设计方案中的10个问题(P1-P10)和5个目标(G1-G5)，分析当前实现状态，给出未完成部分的修复方案

---

## 第一章 现状代码分析

### 1.1 框架层核心函数

#### 1.1.1 `wd_alg_attrs_init()` (wd_util.c:2699-2734)

V2 路径的统一初始化入口，封装 Phase 1 + Phase 2：

```
wd_alg_attrs_init()
  ├── Phase 1: wd_alg_drv_discover() → wd_get_drv_array() [驱动发现]
  └── Phase 2: wd_alg_ctx_init() [ctx 分配 + 调度器创建 + 内部拷贝]
```

- Phase 1 通过 `wd_alg_drv_discover()` (wd_util.c:2311) 将 alg 字符串归一化为 alg_type，调用 `wd_get_drv_array()` 获取驱动列表
- Phase 2 通过 `wd_alg_ctx_init()` (wd_util.c:2490) 按 RR 分配 ctx，创建调度器，调用 `attrs->alg_init` 回调
- Phase 3 (`wd_alg_init_driver()`) 和 Phase 2.5 (`wd_ctx_bind_drivers()`) 由调用者执行，不在此函数内部

**当前实现状态**: 此函数仅被 V2 路径调用。V1 路径（如 wd_cipher.c:401-465）直接调用各框架函数。

#### 1.1.2 `wd_alg_ctx_init()` (wd_util.c:2490-2640)

Phase 2 的 ctx 统一分配函数。实现逻辑：
1. 计算 sync/async ctx 总数 (line 2514-2519)
2. 分配 `wd_ctx_config` 和 `wd_ctx` 数组 (line 2527-2541)
3. RR 循环：`drv = attrs->drv_array[ctx_idx % attrs->drv_count]`，调用 `drv->alloc_ctx()` (line 2547-2590)
4. 创建调度器实例 (line 2597-2620)
5. 调用 `attrs->alg_init` 回调填充内部配置 (line 2621-2624)

**注意**: `ctxs[i].drv` 在此函数中保持为 NULL，由 Phase 2.5 设置。`ctxs[i].ctx` 在此函数中由 `drv->alloc_ctx()` 填充。

#### 1.1.3 `wd_ctx_bind_drivers()` (wd_util.c:2229-2278)

Phase 2.5，ctxs[i].drv 的**唯一写入点**：
- 单驱动：所有 ctx 绑定到 drv_array[0] (line 2243-2245)
- 多驱动：`ctxs[i].drv = drv_array[i % drv_count]` (line 2251)
- HW 驱动自动设置 soft fallback (line 2258-2263)
- 缓存 drv_array 到 config (line 2268-2269)
- 去重引用计数递增 (line 2272)

**当前实现不包含** `drv_region[]` 构建逻辑。`drv_region` 结构体和 `drv_region_count` 字段均未定义在 `wd_ctx_config_internal` (include/wd_internal.h:45-56) 中。

#### 1.1.4 `wd_alg_init_driver()` (wd_util.c:1650-1671)

Phase 3，逐 ctx 遍历调用 `wd_ctx_init_driver()`。通过 `drv->init_state` 标志位保证每个驱动仅初始化一次。失败时回滚已初始化的 ctx。

#### 1.1.5 `wd_get_drv_array()` (wd_alg.c:673-730)

Phase 1 的驱动发现函数。签名：

```c
int wd_get_drv_array(const char *alg_type, int task_type, char *drv_name,
                     struct wd_alg_driver ***drv_array, __u32 *drv_count);
```

- 遍历 `wd_drv_node` 全局链表
- 按 `alg_type` 匹配（通过 `wd_get_alg_type()`）
- 按 `task_type` 过滤（`TASK_HW` → 仅 `UADK_ALG_HW`；`TASK_INSTR` → 排除 HW；`TASK_MIX` → 全部）
- 按 `drv_name` 过滤（或 NULL 表示全部）
- 通过 `wd_alg_drv_type_match()` 进行类型匹配
- 返回去重后的驱动数组（调用者通过 `free()` 释放）

#### 1.1.6 `wd_alg_ctxs_uninit()` (wd_util.c:2656-2684)

ctx 清理函数：
1. 调用 `wd_alg_drv_ref_dec()` 递减驱动引用计数
2. 遍历所有 ctx，调用 `drv->free_ctx()` 释放
3. 释放 ctxs 数组和 ctx_config

**注意**: `wd_alg_driver` 中不存在 `init_refcnt` 字段。引用计数通过 `wd_alg_drv_ref_inc/dec()` 独立管理。

### 1.2 调度器层分析

**当前状态**: 调度器 (wd_sched.c) 当前不包含 compat 过滤逻辑。P7 方案 (coral.md) 已制定：通过 `wd_alg_match_drv()` + 扩展 `wd_sched_params`/`wd_sched_key` + `wd_sched_skey_compat_filter()` + 修改 `session_sched_init_ctx()` 实现 compat ctx 路由。方案细节见 5.3 节。

### 1.3 API 层模块分析

#### 1.3.1 wd_cipher.c — V1 和 V2 均已整合

| 函数 | 行号 | 框架函数调用 |
|------|------|-------------|
| `wd_cipher_init()` (V1) | 401-465 | `wd_get_drv_array("cipher", TASK_HW, "hisi_sec2", ...)` → `wd_ctx_bind_drivers()` → `wd_alg_init_driver()` |
| `wd_cipher_init2_()` (V2) | 485-582 | `wd_alg_attrs_init()` → `wd_ctx_bind_drivers()` → `wd_alg_init_driver()` |

V1 路径不使用 `wd_alg_attrs_init()`，直接调用各框架函数。V2 路径使用统一入口。

#### 1.3.2 wd_digest.c — V2 已整合，V1 使用旧式

| 函数 | 行号 | 框架函数调用 |
|------|------|-------------|
| `wd_digest_init()` (V1) | 321-363 | `wd_ctx_drv_config("sm3", ...)` + `wd_alg_init_driver()` |
| `wd_digest_init2_()` (V2) | 389-440+ | `wd_alg_attrs_init()` + `wd_ctx_bind_drivers()` + `wd_alg_init_driver()` |

V1 路径使用旧式 `wd_ctx_drv_config()`（空 stub，始终返回 0），未通过 `wd_get_drv_array()` 进行驱动发现。

#### 1.3.3 wd_aead.c — V2 已整合，V1 使用旧式

V1 路径 (`wd_aead_init()`, line 626) 使用 `wd_ctx_drv_config()`。V2 路径 (`wd_aead_init2_()`) 使用 `wd_alg_attrs_init()` (line 744)。

#### 1.3.4 wd_comp.c — V2 已整合，V1 使用旧式

V1 路径 (`wd_comp_init()`, line 190-232) 使用 `wd_ctx_drv_config("zlib", ...)` + `wd_alg_init_driver()`。V2 路径 (`wd_comp_init2_()`, line 250+) 使用 `wd_alg_attrs_init()` (line 300)。

#### 1.3.5 wd_rsa.c, wd_ecc.c, wd_dh.c — V2 已整合，V1 使用旧式

这三个模块均遵循相同模式：
- V1: `wd_ctx_drv_config()` + `wd_alg_init_driver()`
- V2: `wd_alg_attrs_init()` (rsa:308, ecc:335, dh:268)

### 1.4 驱动层分析

所有 8 个驱动已正确设置 alloc_ctx/free_ctx 回调：

| 驱动文件 | alloc_ctx | free_ctx | 类型 |
|---------|-----------|----------|------|
| hisi_sec.c | `wd_drv_hw_ctx_alloc` | `wd_drv_hw_ctx_free` | HW |
| hisi_hpre.c | `wd_drv_hw_ctx_alloc` | `wd_drv_hw_ctx_free` | HW |
| hisi_comp.c | `wd_drv_hw_ctx_alloc` | `wd_drv_hw_ctx_free` | HW |
| hisi_dae.c | `wd_drv_hw_ctx_alloc` | `wd_drv_hw_ctx_free` | HW |
| hisi_udma.c | `wd_drv_hw_ctx_alloc` | `wd_drv_hw_ctx_free` | HW |
| isa_ce_sm3.c | `wd_drv_soft_ctx_alloc` | `wd_drv_soft_ctx_free` | CE |
| isa_ce_sm4.c | `wd_drv_soft_ctx_alloc` | `wd_drv_soft_ctx_free` | CE |
| hash_mb.c | `wd_drv_soft_ctx_alloc` | `wd_drv_soft_ctx_free` | SVE |

### 1.5 内存管理分析

已实现的内存管理机制：
- `wd_alg_ctx_init()` 分配 ctx_config、ctxs 数组、调度器
- `wd_ctx_bind_drivers()` 调用 `wd_alg_drv_ref_inc()` 递增驱动引用计数
- `wd_ctx_unbind_drivers()` 调用 `wd_alg_drv_ref_dec()` 递减引用计数
- `wd_alg_ctxs_uninit()` 调用 `drv->free_ctx()` 释放每个 ctx
- `wd_alg_drv_undiscover()` 释放 drv_array

---

## 第二章 问题状态重新评估

### 2.1 十个核心问题状态（基于实际代码）

| # | 问题 | 状态 | 实际依据 |
|---|------|------|---------|
| P1 | init1和init2融合为一 | 部分达成 | V2 全部统一到 `wd_alg_attrs_init()`；V1 仅 wd_cipher.c 使用统一框架函数 |
| P2 | init1 ctx 预分配 | 已解决 | V1 路径中用户提供 ctx，框架通过 `wd_alg_init_driver()` 初始化 |
| P3 | init2 框架内部分配 | 已解决 | `wd_alg_ctx_init()` (wd_util.c:2490) 调用 `drv->alloc_ctx()` 分配 |
| P4 | 统一驱动发现流程 | 已解决 | `wd_get_drv_array()` (wd_alg.c:673) 统一发现 |
| P5 | 参数桥接 | 已解决 | `wd_init_attrs` 统一参数载体 |
| P6 | init1 只匹配 hisi_sec2 | 已解决 | V1 通过 `drv_name` 参数（"hisi_sec2"）限制驱动范围 |
| P7 | session 兼容性检查 | 已有方案 | 通过 wd_alg_match_drv() + 调度器 set_param/skey 实现 session compat 路由 |
| P8 | init 时分配 ctx | 已解决 | `wd_alg_ctx_init()` 在初始化时通过 `drv->alloc_ctx()` 分配 |
| P9 | 多设备算力叠加 | 已解决 | RR 绑定实现多驱动 ctx 分配 (wd_util.c:2229) |
| P10 | 内存生命周期 | 已解决 | 分配→使用→释放链路清晰，引用计数管理完善 |

### 2.2 五个设计目标状态（基于实际代码）

| 目标 | 状态 | 实际依据 |
|------|------|---------|
| G1: init1支持hisi设备，init2支持多设备 | 已达成 | V1: wd_cipher.c:401；V2: wd_alg_attrs_init() + RR 绑定 |
| G2: 支持新设备类型 (PCIe/GPU/NPU) | 已达成 | `calc_type` 枚举 + `alloc_ctx` 回调可扩展 |
| G3: init1/init2 复用函数接口 | 基本达成 | V2 全部复用；V1 仅 wd_cipher.c 复用，其余6个模块使用旧式路径 |
| G4: session 按算法匹配 ctx | 已有方案 | 方案已制定 (coral.md)，待 10 个 API 模块集成 set_param |
| G5: 对外 API 不变 | 已达成 | `wd_<alg>_init(config, sched)` 和 `wd_<alg>_init2_(alg, ...)` 签名不变 |

---

## 第三章 问题汇总

### 3.1 已实现的功能

| 功能 | 位置 | 说明 |
|------|------|------|
| Phase 1 驱动发现 | wd_util.c:2311 + wd_alg.c:673 | 按 alg_type + task_type 匹配，去重 |
| Phase 2 ctx 分配 | wd_util.c:2490 | RR 分配，调用 `drv->alloc_ctx()` |
| Phase 2.5 驱动绑定 | wd_util.c:2229 | RR 绑定 + HW fallback |
| Phase 3 驱动初始化 | wd_util.c:1650 | 逐 ctx 调用 `wd_ctx_init_driver()` |
| ctx 清理 | wd_util.c:2656 | `drv->free_ctx()` + refcnt 递减 |
| V2 路径 API 整合 | 12个模块 | 全部调用 `wd_alg_attrs_init()` |
| 驱动 alloc_ctx/free_ctx | 8个驱动 | 全部设置统一回调 |

### 3.2 尚未实施的设计项

| 编号 | 设计项 | 设计文档中的位置 | 实际状态 |
|------|--------|-----------------|---------|
| U2 | `wd_get_compat_ctxs()` | 设计文档 | 已替换 — P7 方案改用 wd_alg_match_drv() + skey 过滤 |
| U3 | `compat_ctx_set` 结构体 | 设计文档 | 已替换 — P7 方案不再需要独立 compat_ctx_set 结构 |
| U4 | `drv_region` 结构体 | include/wd_internal.h | 不存在。`wd_ctx_config_internal` 中无此字段 |
| U5 | `init_refcnt` 字段 | wd_alg_driver | 不存在。引用计数通过独立函数 `wd_alg_drv_ref_inc/dec()` 管理 |
| U6 | `need_lock` 字段 | wd_alg_driver | 不存在 |
| U7 | `dev_cache` 字段 | wd_alg_driver | 不存在 |

### 3.3 待改进的实际问题

| 编号 | 问题 | 涉及模块 | 说明 |
|------|------|---------|------|
| I1 | V1 路径未统一 | digest, aead, comp, rsa, ecc, dh | V1 init 使用 `wd_ctx_drv_config()` (空 stub) 而非 `wd_get_drv_array()` + `wd_ctx_bind_drivers()` |
| I2 | wd_ctx_drv_config 空 stub | 8个模块 | `wd_ctx_drv_config()` (wd_util.c:2183-2185) 始终返回 0，仍在 V1 路径中被调用 |
| I3 | wd_ctx_drv_deconfig 空 stub | 10个模块 | 函数体为空 (wd_util.c:2187-2189)，共 30 处残余调用 |
| I4 | Session 兼容性路由待实施 | 全部模块 | 方案已制定，需在 10 个 API 模块 alloc_sess 中集成 set_param compat 过滤 |

---

## 第四章 关键函数参考（当前实际代码）

| 函数 | 文件 | 行号 | 说明 |
|------|------|------|------|
| `wd_alg_attrs_init()` | wd_util.c | 2699 | V2 统一初始化：Phase 1 + Phase 2 |
| `wd_alg_drv_discover()` | wd_util.c | 2311 | Phase 1：驱动发现 |
| `wd_alg_ctx_init()` | wd_util.c | 2490 | Phase 2：ctx 分配 + 调度器 |
| `wd_ctx_bind_drivers()` | wd_util.c | 2229 | Phase 2.5：RR 绑定 |
| `wd_alg_init_driver()` | wd_util.c | 1650 | Phase 3：驱动初始化 |
| `wd_alg_ctxs_uninit()` | wd_util.c | 2656 | ctx 清理 |
| `wd_alg_attrs_uninit()` | wd_util.c | 2680 | V2 清理：释放 ctx + 调度器 |
| `wd_get_drv_array()` | wd_alg.c | 673 | 驱动发现：遍历注册表 |
| `wd_ctx_drv_config()` | wd_util.c | 2183 | **空 stub**：始终返回 0 |
| `wd_ctx_drv_deconfig()` | wd_util.c | 2187 | **空 stub**：空函数体 |
| `wd_drv_hw_ctx_alloc()` | wd_drv.c | 19 | HW ctx 分配 |
| `wd_drv_soft_ctx_alloc()` | wd_drv.c | 66 | Soft ctx 分配 |

---

## 第五章 修复方案

### 5.1 V1 路径统一 (问题 I1)

**目标**: 将 digest/aead/comp/rsa/ecc/dh 的 V1 init 迁移到统一框架函数。

**参照实现**: wd_cipher.c:401-465

**修改模式** (以 wd_digest.c 为例):

```c
int wd_digest_init(struct wd_ctx_config *config, struct wd_sched *sched)
{
    __u32 drv_count = 0;
    int ret;

    // ... 前置检查 (atfork, try_init, param_check, open_driver) ...

    /* 内部配置拷贝 */
    ret = wd_digest_init_nolock(config, sched);
    if (ret)
        goto out_close_driver;

    /* Phase 1: 驱动发现 */
    ret = wd_get_drv_array("digest", TASK_HW, "hisi_sec2",
            &wd_digest_setting.config.drv_array, &drv_count);
    if (ret) {
        WD_ERR("driver discovery failed!\n");
        goto out_uninit_nolock;
    }

    /* Phase 2.5: RR 绑定 */
    ret = wd_ctx_bind_drivers(&wd_digest_setting.config,
            wd_digest_setting.config.drv_array, drv_count);
    if (ret) {
        WD_ERR("driver binding failed!\n");
        goto out_free_drv_array;
    }

    /* Phase 3: 驱动初始化 */
    ret = wd_alg_init_driver(&wd_digest_setting.config);
    if (ret) {
        WD_ERR("driver init failed!\n");
        goto out_unbind_drivers;
    }

    wd_alg_set_init(&wd_digest_setting.status);
    return 0;

out_unbind_drivers:
    wd_ctx_unbind_drivers(&wd_digest_setting.config);
out_free_drv_array:
    wd_put_drv_array(wd_digest_setting.config.drv_array, drv_count);
    wd_digest_setting.config.drv_array = NULL;
out_uninit_nolock:
    wd_digest_uninit_nolock();
out_close_driver:
    wd_digest_close_driver(WD_TYPE_V1);
out_clear_init:
    wd_alg_clear_init(&wd_digest_setting.status);
    return ret;
}
```

**核心变更**:
1. 替换 `wd_ctx_drv_config("sm3", ...)` 为 `wd_get_drv_array("digest", TASK_HW, "hisi_sec2", ...)`
2. 替换后的 `wd_alg_init_driver()` 调用不变
3. 添加 `wd_ctx_bind_drivers()` 调用
4. 完善错误标签（out_unbind_drivers, out_free_drv_array）

### 5.2 移除 wd_ctx_drv_config/deconfig 调用 (问题 I2, I3)

`wd_ctx_drv_config()` 和 `wd_ctx_drv_deconfig()` 均为空 stub，可以安全移除。

**V1 路径修复**: 完成 5.1 节的迁移后，`wd_ctx_drv_config()` 的调用将自然消失。

**wd_ctx_drv_deconfig 清理**: 在各模块的 uninit 和错误路径中移除 `wd_ctx_drv_deconfig()` 调用。这些调用当前无任何功能影响（空函数体），属于代码清洁工作。

**涉及模块**: wd_aead.c (4处), wd_digest.c (4处), wd_comp.c (4处), wd_rsa.c (4处), wd_ecc.c (4处), wd_dh.c (4处), wd_udma.c (2处), wd_agg.c (2处), wd_join_gather.c (2处)

### 5.3 Session 兼容性路由 (问题 I4)

此功能的实现需要（基于 coral.md 方案）：

**调度器层改动 (wd_sched.c)**:
1. 扩展 `wd_sched_key` 新增 `alg_name`、`ctxs` 字段
2. 新增 `wd_sched_skey_compat_filter()` 函数 — 遍历 domain cache，通过 `wd_alg_match_drv()` 检查并替换不兼容的 ctx
3. 修改 `session_sched_init_ctx()` — 增加 `skey` 参数，支持通过 RR 遍历 domain 找兼容 ctx
4. 修改 `wd_sched_set_param()` — 存储 compat 信息并触发 `wd_sched_skey_compat_filter()` 修正已预取的 ctx

**头文件改动**:
5. 扩展 `wd_sched_params` (wd_internal.h) 新增 `alg_name`、`ctxs` 字段
6. 在 `wd_alg.h` 中添加 `wd_alg_match_drv()` 声明导出

**API 层改动 (10 个模块)**:
7. 在各模块 `alloc_sess` 中，`sched_init` 后构造 `wd_sched_params` 并调用 `set_param` 传入 `alg_name` + `ctxs`

涉及的 10 个 API 模块: wd_cipher.c:267, wd_digest.c:205, wd_aead.c:460, wd_comp.c:457, wd_rsa.c:931, wd_ecc.c:1199, wd_dh.c:577, wd_agg.c:425, wd_join_gather.c:497, wd_udma.c:74

---

## 第六章 验证方案

### 6.1 编译验证

```bash
./cleanup.sh && ./autogen.sh && ./conf.sh && make -j$(nproc)
```

### 6.2 功能验证

| 测试项 | 验证方法 |
|--------|----------|
| V1 init 正常工作 | 运行现有 hisi_sec2 测试 |
| V2 init 发现多驱动 | 日志输出 drv_array 内容和 drv_count |
| drv->alloc_ctx 被调用 | 添加调试日志（wd_alg_ctx_init 中已有 WD_ERR 日志） |
| ctx 释放正确 | 检查 uninit 后 refcnt 归零 |

### 6.3 回归测试

```bash
sudo -E ./test/sanity_test.sh
```

---

## 第七章 总结

### 7.1 实现状态

| 维度 | 完成情况 |
|------|---------|
| Phase 1 驱动发现 | 完成 (wd_alg.c:673, wd_util.c:2311) |
| Phase 2 ctx 分配 | 完成 (wd_util.c:2490) |
| Phase 2.5 驱动绑定 | 完成 (wd_util.c:2229) |
| Phase 3 驱动初始化 | 完成 (wd_util.c:1650) |
| 驱动层 alloc_ctx/free_ctx | 完成 (8个驱动) |
| V2 路径 API 整合 | 完成 (12个模块) |
| V1 路径 API 整合 | wd_cipher.c 完成，6个模块待迁移 |
| wd_ctx_drv_deconfig 清理 | wd_cipher.c 完成，10个模块待清理 |
| Session 兼容性路由 | 设计预留 |

### 7.2 核心发现

1. **框架层核心已完整实现**: Phase 1/2/2.5/3 四个阶段的函数均已在 wd_util.c 中实现，V2 路径完全统一。

2. **驱动层完全适配**: 所有 8 个驱动已设置 alloc_ctx/free_ctx 回调。

3. **V1 路径存在技术债务**: 7 个模块中仅 wd_cipher.c 完成了 V1 路径向统一框架的迁移。其余 6 个模块仍使用旧式 `wd_ctx_drv_config()`（空 stub）。

4. **Session 兼容性路由已有方案**: 原设计中的 `wd_get_compat_ctxs()`/`compat_ctx_set` 已被 `wd_alg_match_drv()` + 调度器 `set_param`/`skey` 方案取代。详见 coral.md 设计方案。

### 7.3 建议的后续工作

1. **V1 路径统一** (问题 I1): 将 digest/aead/comp/rsa/ecc/dh 的 V1 init 迁移到 `wd_get_drv_array()` + `wd_ctx_bind_drivers()` 模式
2. **清理旧 stub** (问题 I2/I3): 移除 `wd_ctx_drv_config()` 和 `wd_ctx_drv_deconfig()` 的残余调用
3. **P7 实施** (问题 I4): 按 coral.md 方案实施 session 兼容性路由 — 调度器层添加 `wd_sched_skey_compat_filter()`，各 API 模块 alloc_sess 集成 set_param

---

*文档更新完成 - 2026-05-06*
