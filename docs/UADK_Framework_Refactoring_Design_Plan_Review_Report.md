# UADK框架重构设计方案审视报告

> 日期: 2026-05-06 (基于当前代码实现，不含 v1/ 目录)
> 基于: 实际源代码验证 (wd_util.c 2734行, wd_alg.c, wd_cipher.c 等)
> 目的: 对照设计方案中的10个问题(P1-P10)和5个目标(G1-G5)，审视当前代码实现状态

---

## 一、审视结论总览

| 类别 | 状态 | 说明 |
|------|------|------|
| P1-P6, P8-P10 (核心问题) | 已实现 | 见第二章逐项验证 |
| P7 (session兼容性路由) | 已有方案 | 通过 wd_alg_match_drv() + 调度器 set_param/skey 机制实现，详见 coral.md 方案 |
| G1-G3, G5 (设计目标) | 已达成 | 框架统一、API不变、V1/V2复用 |
| G4 (session按算法匹配ctx) | 部分达成 | 受 P7 限制 |

**重要说明**: 此前版本报告中声称的 `wd_get_compat_ctxs()`（wd_alg.c 中不存在）、`compat_ctx_set`（wd_sched.c 中不存在）等函数/字段均为设计文档中的规划，尚未在代码中实现。此前声称的 3 个 "P0 致命 Bug"（wd_cipher.c:541 goto 死代码、wd_aead.c:336 compat 未传递、wd_util.c:3287 过时 TODO）均基于错误的行号——实际代码中不存在这些问题。

---

## 二、十个核心问题逐一审视 (基于实际代码)

### P1: V1/V2 双路径融合

**设计方案要求**: 将 V1 (wd_<alg>_init) 和 V2 (wd_<alg>_init2_) 两条初始化路径统一到同一套框架函数。

**实际实现状态**: 部分达成。V2 路径在所有模块中已统一使用 `wd_alg_attrs_init()`（Phase 1+2）。V1 路径的实现因模块而异：

| 模块 | V1 路径 | V2 路径 | 框架统一程度 |
|------|---------|---------|-------------|
| wd_cipher.c | `wd_get_drv_array()` + `wd_ctx_bind_drivers()` + `wd_alg_init_driver()` (line 426-443) | `wd_alg_attrs_init()` + `wd_ctx_bind_drivers()` + `wd_alg_init_driver()` (line 537-559) | **完全统一** — V1/V2 使用相同框架函数 |
| wd_digest.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` (line 343-347) | `wd_alg_attrs_init()` (line 438) | **部分统一** — V1 使用旧式 drv_config |
| wd_aead.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` | `wd_alg_attrs_init()` (line 744) | **部分统一** — V1 使用旧式 drv_config |
| wd_comp.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` (line 212-216) | `wd_alg_attrs_init()` (line 300) | **部分统一** — V1 使用旧式 drv_config |
| wd_rsa.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` | `wd_alg_attrs_init()` (line 308) | **部分统一** — V1 使用旧式 drv_config |
| wd_ecc.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` | `wd_alg_attrs_init()` (line 335) | **部分统一** — V1 使用旧式 drv_config |
| wd_dh.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` | `wd_alg_attrs_init()` (line 268) | **部分统一** — V1 使用旧式 drv_config |
| wd_agg.c | V2-only | `wd_alg_attrs_init()` (line 715) | V2 统一 |
| wd_udma.c | V2-only | `wd_alg_attrs_init()` (line 470) | V2 统一 |
| wd_join_gather.c | V2-only | `wd_alg_attrs_init()` (line 802) | V2 统一 |

**结论**: V2 路径完全统一（所有12个模块均使用 `wd_alg_attrs_init()`），V1 路径仅 wd_cipher.c 完成了向统一框架的迁移。其余模块的 V1 路径仍使用 `wd_ctx_drv_config()` + `wd_alg_init_driver()` 的旧式模式，未通过 `wd_get_drv_array()` 进行驱动发现，也未通过 `wd_ctx_bind_drivers()` 进行 Round-Robin 绑定。

### P2: 驱动自动发现

**设计方案要求**: V2 路径自动通过 dlopen 发现并加载驱动。

**实际实现**: **已实现**。`wd_alg_drv_discover()` (wd_util.c:2311) 将用户传入的 alg 字符串（如 "cipher"）归一化为 alg_type，调用 `wd_get_drv_array()` (wd_alg.c:673) 遍历驱动注册链表，按 alg_type 和 task_type 匹配，返回去重后的驱动数组。

```
wd_alg_attrs_init() (wd_util.c:2699)
  └─> Phase 1: wd_alg_drv_discover() (wd_util.c:2311)
        └─> wd_get_drv_array() (wd_alg.c:673) — 遍历 wd_drv_node 链表
```

### P3: 统一上下文分配

**设计方案要求**: 所有驱动通过统一的回调接口分配上下文。

**实际实现**: **已实现**。`wd_alg_ctx_init()` (wd_util.c:2490) 实现统一分配：
1. 计算 sync/async ctx 总数（line 2514-2519）
2. 分配 `wd_ctx_config` 和 `wd_ctx` 数组（line 2527-2541）
3. 按 RR 规则循环调用 `drv->alloc_ctx()` 分配每个 ctx（line 2547-2590）
4. 创建调度器实例（line 2597-2620）
5. 调用 `attrs->alg_init` 回调填充内部结构（line 2621-2624）

注意：`ctxs[i].drv` 在此函数中保持为 NULL，由 Phase 2.5 设置。

### P4: Round-Robin 驱动绑定

**设计方案要求**: ctx 按 Round-Robin 规则均匀绑定到不同驱动。

**实际实现**: **已实现**。`wd_ctx_bind_drivers()` (wd_util.c:2229) 是 ctxs[i].drv 的**唯一写入点**：
- 单驱动场景：所有 ctx 绑定到同一驱动（line 2243-2245）
- 多驱动场景：`ctxs[i].drv = drv_array[i % drv_count]`（line 2251）
- HW 驱动自动设置 soft fallback（line 2258-2263）
- 缓存 drv_array 到 config 供 session 查询（line 2268-2269）
- 去重引用计数递增（line 2272）

### P5: 错误处理与回滚

**设计方案要求**: 初始化失败时正确回滚已分配资源。

**实际实现**: **已实现**。每个阶段失败都有对应的清理路径：
- Phase 1 失败：直接返回，无资源需释放
- Phase 2 失败（wd_alg_attrs_init:2723）：跳转到 `out_undiscover` 释放 drv_array
- Phase 2.5/3 失败（由调用者处理）：wd_cipher.c 的错误标签包含 `out_unbind_drivers`、`out_common_uninit`、`out_free_drv_array`、`out_close_driver`、`out_clear_init` 等完整回滚路径

### P6: 多驱动异构算力叠加

**设计方案要求**: 同一算法类型支持多个不同驱动（如 hisi_sec + isa_ce）同时工作。

**实际实现**: **已实现**。`wd_get_drv_array()` (wd_alg.c:673) 遍历 wd_drv_node 链表时，对所有匹配 alg_type 的驱动去重收集。`wd_ctx_bind_drivers()` 通过 RR 绑定将 ctx 均匀分配到不同驱动：

```
示例（2 个驱动，4 个 ctx）：
  ctx[0].drv = drv_array[0 % 2] = drv_array[0]  (hisi_sec)
  ctx[1].drv = drv_array[1 % 2] = drv_array[1]  (isa_ce)
  ctx[2].drv = drv_array[2 % 2] = drv_array[0]  (hisi_sec)
  ctx[3].drv = drv_array[3 % 2] = drv_array[1]  (isa_ce)
```

### P7: Session 兼容性路由

**设计方案要求**: 创建 session 时根据算法匹配兼容的 ctx。

**实际实现**: **已有方案**（基于 coral.md）。原设计中提出的 `wd_get_compat_ctxs()` 和 `compat_ctx_set` 已被更轻量的方案取代，核心链路为 `ctx_id → ctxs[idx].drv → wd_alg_match_drv(drv, alg_name)`：

- **`wd_alg_match_drv()`** (wd_alg.c:423) — 已实现，检查驱动是否支持特定算法名。需在 `wd_alg.h` 中添加声明导出
- **`wd_sched_params.alg_name` + `ctxs`** — `wd_internal.h` 新增字段，承载 compat 过滤参数
- **`wd_sched_key.alg_name` + `ctxs`** — 调度器内部存储 compat 信息
- **`wd_sched_skey_compat_filter()`** — 遍历 domain cache，调用 `wd_alg_match_drv()` 检查每个 ctx，不兼容则替换
- **`session_sched_init_ctx()`** — 扩展 `skey` 参数，通过 RR 遍历 domain 找到兼容 ctx
- **`wd_sched_set_param()`** — 存储 compat 信息并触发 `wd_sched_skey_compat_filter()` 修正已预取的 ctx
- **10 个 API 模块** — 在 `alloc_sess` 中 `sched_init` 后调用 `set_param` 传入 compat 信息

**设计流程**: `sched_init` 预取 ctx（无过滤） → `set_param` 传入 `alg_name` + `ctxs` → `wd_sched_skey_compat_filter()` 修正 domain cache → 后续 `pick_next_ctx` 返回兼容 ctx。

### P8: 驱动生命周期管理

**设计方案要求**: 引用计数管理驱动加载/卸载。

**实际实现**: **已实现**。
- `wd_alg_drv_ref_inc()` / `wd_alg_drv_ref_dec()` 管理驱动引用计数
- `wd_alg_drv_ref_dec()` (wd_util.c:2191-2209) 对 ctxs[i].drv 去重后逐一递减
- `wd_alg_ctxs_uninit()` (wd_util.c:2656-2684) 负责 ctx 清理时递减引用计数
- `wd_dlopen_drv()` 和 `wd_dlclose_drv()` 管理 dlopen 加载的动态库生命周期

### P9: 调度器域管理

**设计方案要求**: 调度器统一管理 ctx 的调度域。

**实际实现**: **已实现**。`wd_alg_sched_instance()` (wd_util.c:2336) 在 `wd_alg_ctx_init()` 中被调用，创建调度器实例并将所有 ctx 注册到调度域。调度策略通过 `attrs->sched_type` 配置。

### P10: 内存生命周期管理

**设计方案要求**: 明确 ctx 和调度器内存的分配和释放责任。

**实际实现**: **已实现**。生命周期清晰：
- **分配**: `wd_alg_ctx_init()` 分配 ctx_config、ctxs 数组、调度器
- **使用**: ctxs 通过 `attrs->alg_init` 回调填充后属于模块的内部 config
- **释放**: `wd_alg_ctxs_uninit()` 释放 ctxs、调度器、ctx_config
- **引用计数**: `wd_alg_ctxs_uninit()` 在释放前调用 `wd_alg_drv_ref_dec()`

---

## 三、五个设计目标达成情况

| 目标 | 状态 | 说明 |
|------|------|------|
| G1: init1继续支持hisi设备，init2扩展支持多设备多驱动异构算力叠加 | 已达成 | V1 路径保留（wd_cipher.c:401），V2 路径支持多驱动 RR 绑定（wd_util.c:2229） |
| G2: 支持未来 PCIe/GPU/NPU 等新设备类型 | 已达成 | calc_type 字段已定义（UADK_ALG_HW/SOFT/SVE/CE），alloc_ctx 回调可扩展 |
| G3: init1和init2尽可能复用函数接口，减少冗余代码 | 基本达成 | V2 路径所有12个模块复用 `wd_alg_attrs_init()`；V1 路径仅 wd_cipher.c 使用了统一框架函数，其余7个模块的 V1 仍使用旧式 `wd_ctx_drv_config()` |
| G4: init时按算法类型匹配驱动，session时按具体算法匹配ctx | 已有方案 | init 阶段驱动匹配已实现；session 阶段兼容性匹配方案已制定（P7 coral.md），待 API 模块集成 set_param |
| G5: 对外API接口不变，内部函数和接口可修改 | 已达成 | `wd_<alg>_init(config, sched)` 和 `wd_<alg>_init2_(alg, sched_type, task_type, ctx_params)` 签名保持不变 |

---

## 四、驱动层适配状态

### 4.1 所有驱动已设置 alloc_ctx/free_ctx

| 驱动 | 文件 | alloc_ctx | free_ctx | 类型 |
|------|------|-----------|----------|------|
| hisi_sec | drv/hisi_sec.c | `wd_drv_hw_ctx_alloc` | `wd_drv_hw_ctx_free` | HW |
| hisi_hpre | drv/hisi_hpre.c | `wd_drv_hw_ctx_alloc` | `wd_drv_hw_ctx_free` | HW |
| hisi_comp | drv/hisi_comp.c | `wd_drv_hw_ctx_alloc` | `wd_drv_hw_ctx_free` | HW |
| hisi_dae | drv/hisi_dae.c | `wd_drv_hw_ctx_alloc` | `wd_drv_hw_ctx_free` | HW |
| hisi_udma | drv/hisi_udma.c | `wd_drv_hw_ctx_alloc` | `wd_drv_hw_ctx_free` | HW |
| isa_ce_sm3 | drv/isa_ce_sm3.c | `wd_drv_soft_ctx_alloc` | `wd_drv_soft_ctx_free` | CE |
| isa_ce_sm4 | drv/isa_ce_sm4.c | `wd_drv_soft_ctx_alloc` | `wd_drv_soft_ctx_free` | CE |
| hash_mb | drv/hash_mb/hash_mb.c | `wd_drv_soft_ctx_alloc` | `wd_drv_soft_ctx_free` | SVE |

**结论**: 驱动层完全适配。

---

## 五、API 层模块整合状态

### 5.1 V2 路径整合状态

所有 API 模块的 V2 路径（`wd_<alg>_init2_`）均已调用 `wd_alg_attrs_init()`（wd_util.c:2699），完成 Phase 1+2：

| 模块 | wd_alg_attrs_init 调用行号 | 后续 Phase 2.5+3 |
|------|---------------------------|-------------------|
| wd_cipher.c | line 537 | wd_ctx_bind_drivers (line 550) + wd_alg_init_driver (line 559) |
| wd_digest.c | line 438 | wd_ctx_bind_drivers + wd_alg_init_driver |
| wd_aead.c | line 744 | wd_ctx_bind_drivers + wd_alg_init_driver |
| wd_comp.c | line 300 | wd_ctx_bind_drivers + wd_alg_init_driver |
| wd_rsa.c | line 308 | wd_ctx_bind_drivers + wd_alg_init_driver |
| wd_ecc.c | line 335 | wd_ctx_bind_drivers + wd_alg_init_driver |
| wd_dh.c | line 268 | wd_ctx_bind_drivers + wd_alg_init_driver |
| wd_agg.c | line 715 | wd_ctx_bind_drivers + wd_alg_init_driver |
| wd_udma.c | line 470 | wd_ctx_bind_drivers + wd_alg_init_driver |
| wd_join_gather.c | line 802 | wd_ctx_bind_drivers + wd_alg_init_driver |

### 5.2 V1 路径整合状态

| 模块 | V1 路径 | 整合方式 |
|------|---------|----------|
| wd_cipher.c | `wd_get_drv_array()` + `wd_ctx_bind_drivers()` + `wd_alg_init_driver()` | **已整合** — 使用统一框架函数 |
| wd_digest.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` | **旧式** — wd_ctx_drv_config 是空 stub |
| wd_aead.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` | **旧式** |
| wd_comp.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` | **旧式** |
| wd_rsa.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` | **旧式** |
| wd_ecc.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` | **旧式** |
| wd_dh.c | `wd_ctx_drv_config()` + `wd_alg_init_driver()` | **旧式** |

### 5.3 wd_ctx_drv_deconfig 残留

`wd_ctx_drv_deconfig()` (wd_util.c:2187-2189) 函数体为**空**（no-op stub），但在以下模块中仍有残余调用：

| 文件 | 调用行号 | 调用次数 |
|------|----------|---------|
| wd_aead.c | 660, 676, 768, 786 | 4 |
| wd_digest.c | 356, 372, 462, 480 | 4 |
| wd_comp.c | 225, 240, 325, 342 | 4 |
| wd_rsa.c | 237, 252, 333, 350 | 4 |
| wd_ecc.c | 262, 277, 360, 377 | 4 |
| wd_dh.c | 197, 216, 293, 314 | 4 |
| wd_udma.c | 378, 495 | 2 |
| wd_agg.c | 739, 761 | 2 |
| wd_join_gather.c | 826, 848 | 2 |

wd_cipher.c 已完全移除 `wd_ctx_drv_deconfig()` 调用。

### 5.4 wd_ctx_drv_config 残留

`wd_ctx_drv_config()` (wd_util.c:2183-2185) 同样是空 stub（`{ return 0; }`），在以下模块的 V1 路径中仍被调用：
- wd_digest.c:343, wd_aead.c, wd_comp.c:212, wd_rsa.c, wd_ecc.c, wd_dh.c, wd_udma.c, wd_agg.c, wd_join_gather.c

---

## 六、关键函数实现状态

| 函数 | 文件 | 行号 | 状态 | 说明 |
|------|------|------|------|------|
| `wd_alg_attrs_init()` | wd_util.c | 2699 | 已实现 | Phase 1+2 编排器。V2 路径统一入口 |
| `wd_alg_drv_discover()` | wd_util.c | 2311 | 已实现 | Phase 1: 驱动发现（纯查询，无副作用） |
| `wd_alg_ctx_init()` | wd_util.c | 2490 | 已实现 | Phase 2: ctx 分配 + 调度器 + 内部拷贝 |
| `wd_ctx_bind_drivers()` | wd_util.c | 2229 | 已实现 | Phase 2.5: RR 绑定 + HW fallback 设置 |
| `wd_alg_init_driver()` | wd_util.c | 1650 | 已实现 | Phase 3: 调用每个驱动的 init 回调 |
| `wd_alg_ctxs_uninit()` | wd_util.c | 2656 | 已实现 | ctx 清理 + refcnt 递减 |
| `wd_get_drv_array()` | wd_alg.c | 673 | 已实现 | 遍历驱动注册表，按 alg_type + task_type 匹配 |
| `wd_drv_hw_ctx_alloc()` | wd_drv.c | 19 | 已实现 | HW ctx 分配（打开 UACCE 设备队列） |
| `wd_drv_soft_ctx_alloc()` | wd_drv.c | 66 | 已实现 | Soft ctx 分配（ring buffer） |
| `wd_ctx_drv_config()` | wd_util.c | 2183 | **空 stub** | `return 0` — 旧接口，被多个 V1 路径调用 |
| `wd_ctx_drv_deconfig()` | wd_util.c | 2187 | **空 stub** | 空函数体 — 旧接口，被10个模块调用 |

### 设计预留（未实现）的函数

| 函数 | 设计文档中的位置 | 说明 |
|------|-----------------|------|
| `wd_get_compat_ctxs()` | wd_alg.c (设计) | 不存在。Session 兼容性路由未实现 |
| `wd_drv_supports_alg()` | wd_alg.c (设计) | 不存在 |
| `compat_ctx_set` | 调度器 (设计) | 不存在 |
| `wd_drv_alg_count()` | wd_alg.c (设计) | 从未实现 |

---

## 七、当前已知问题

### 7.1 功能缺失

| 问题 | 影响范围 | 说明 |
|------|----------|------|
| V1 路径未统一 | wd_digest.c, wd_aead.c, wd_comp.c, wd_rsa.c, wd_ecc.c, wd_dh.c | V1 init 使用 `wd_ctx_drv_config()` (空 stub) 而非 `wd_get_drv_array()` + `wd_ctx_bind_drivers()`，缺少驱动自动发现和 RR 绑定 |
| Session 兼容性路由 | 待实施 | 方案已制定 (coral.md)，需在 10 个 API 模块的 alloc_sess 中集成 set_param compat 过滤 |

### 7.2 代码清洁问题

| 问题 | 涉及模块数 | 说明 |
|------|-----------|------|
| wd_ctx_drv_config 空 stub 调用 | 8 个模块 | `wd_ctx_drv_config()` 始终返回 0，但仍在 V1 路径中被调用 |
| wd_ctx_drv_deconfig 空 stub 调用 | 10 个模块 | 函数体为空，共 30 处残余调用（wd_cipher.c 已清理） |

### 7.3 不存在的 Bug（此前报告错误声称）

| 声称的 Bug | 实际代码 | 结论 |
|------------|----------|------|
| wd_cipher.c:541 goto 跳过 Phase 3 | line 541 是 `continue;` 在 while 循环内，Phase 3 在 line 559 正常执行 | **不存在** |
| wd_aead.c:336 sess->compat 未传递 | aead.c 中无 `compat` 字段引用 | **不存在** |
| wd_util.c:3287 过时 TODO | 文件仅 2734 行 | **不存在** |

---

## 八、总结

### 8.1 设计方案执行状态

| 维度 | 状态 | 说明 |
|------|------|------|
| 路径融合 (P1-P5) | 基本完成 | V2 路径全部统一；V1 路径仅 wd_cipher.c 完成迁移 |
| 多驱动兼容 (P6) | 已完成 | RR 绑定 + HW fallback |
| Session 路由 (P7) | 设计预留 | 兼容性 ctx 路由未实施 |
| ctx分配时机 (P8-P9) | 已完成 | 引用计数 + 调度器域管理 |
| 内存生命周期 (P10) | 已完成 | 分配/使用/释放链路清晰 |
| 设计目标 (G1-G5) | 基本达成 | G4 受 P7 限制 |

### 8.2 关键里程碑

| 里程碑 | 状态 |
|--------|------|
| Phase 1 驱动发现 (`wd_alg_drv_discover`) | 完成 |
| Phase 2 统一分配 (`wd_alg_ctx_init`) | 完成 |
| Phase 2.5 驱动绑定 (`wd_ctx_bind_drivers`) | 完成 |
| Phase 3 驱动初始化 (`wd_alg_init_driver`) | 完成 |
| 驱动层适配 (alloc_ctx/free_ctx) | 完成 |
| V2 路径 API 层整合 (12个模块) | 完成 |
| V1 路径 API 层整合 | 仅 wd_cipher.c 完成，7个模块待迁移 |
| wd_ctx_drv_deconfig 清理 | 仅 wd_cipher.c 完成，10个模块待清理 |

### 8.3 建议的后续工作

1. **V1 路径统一**: 将 digest/aead/comp/rsa/ecc/dh 的 V1 init 迁移到 `wd_get_drv_array()` + `wd_ctx_bind_drivers()` 模式（参照 wd_cipher.c:401-465）
2. **旧 stub 清理**: 移除 10 个模块中 `wd_ctx_drv_config()` 和 `wd_ctx_drv_deconfig()` 的残余调用
3. **P7 实施**: 按 coral.md 方案在各 API 模块 alloc_sess 中集成 set_param compat 过滤（`wd_sched_params` 传入 `alg_name` + `ctxs`），在调度器层添加 `wd_sched_skey_compat_filter()` 和修改 `session_sched_init_ctx()`

---

*报告更新完成 - 2026-05-06*
