好的，请继续帮我翻译这个新的 Claude Code 制定的计划：

## 计划：消除 Init2 路径中的冗余驱动绑定

### 背景

UADK 为所有算法库（cipher、comp、digest、aead、rsa、dh、ecc、agg、udma、join_gather）提供了两条初始化路径：

- **传统路径**（`wd_<alg>_init`）：用户提供预先构建的 `wd_ctx_config` + `wd_sched`。`wd_ctx_drv_config` 是第一个也是唯一的驱动绑定点 —— 正确。
- **Init2 路径**（`wd_<alg>_init2_`）：框架通过 `wd_alg_attrs_init` 自动发现设备并创建 ctx。该函数在 ctx 创建期间已经知道正确的驱动类型（为每个 ctx 设置 `ctx_type`），但**不**绑定驱动。然后单独调用 `wd_ctx_drv_config`，重新搜索全局 `alg_list_head` 来重新推导已经隐含的信息。这导致：
  - **冗余**：针对相同的信息，全局驱动列表被搜索了两次。
  - **引用计数泄漏**：在 `TASK_MIX`/`TASK_INSTR` 中，`wd_alg_attrs_init` 为了检查 `calc_type` 会诊断性地调用 `wd_request_drv(alg, ALG_DRV_SOFT)`，增加引用计数。然后 `wd_ctx_drv_config` 通过 `wd_alg_drv_bind` 再次调用它，再次增加引用计数。诊断调用的引用计数从未被释放。
  - **脆弱性**：如果在两次调用之间全局驱动列表发生变化，ctx 可能绑定到与预期不同的驱动。

### 解决方案：预绑定驱动表

**核心思想**：`wd_alg_attrs_init` 在 ctx 创建后立即绑定驱动，将结果存储到 `wd_init_attrs` 的新增 `drv_bindings` 数组中。`wd_ctx_drv_config` 接受这个可选的预绑定表并使用它，而不是重新搜索。传统路径传递 `NULL` —— 行为不变。

### 需要修改的文件

| 文件 | 修改内容 | 大约行数 |
|------|----------|----------|
| `include/wd_util.h` | 向 `wd_init_attrs` 添加 `drv_bindings` 字段；更新 `wd_ctx_drv_config` 声明 | +2，~1 修改 |
| `wd_util.c` | 修复 2 处引用计数泄漏；在 `wd_alg_attrs_init` 中添加绑定循环；更新 `wd_ctx_drv_config`；更新 `wd_alg_attrs_uninit` | ~70 修改/新增 |
| `wd_cipher.c` | 传递 `drv_bindings`（init2 L517）；传递 `NULL`（传统 L418） | 2 处修改 |
| `wd_comp.c` | 传递 `drv_bindings`（init2 L311）；传递 `NULL`（传统 L212） | 2 处修改 |
| `wd_digest.c` | 传递 `drv_bindings`（init2 L448）；传递 `NULL`（传统 L343） | 2 处修改 |
| `wd_aead.c` | 传递 `drv_bindings`（init2 L754）；传递 `NULL`（传统 L648） | 2 处修改 |
| `wd_rsa.c` | 传递 `drv_bindings`（init2 L319）；传递 `NULL`（传统 L224） | 2 处修改 |
| `wd_dh.c` | 传递 `drv_bindings`（init2 L279）；传递 `NULL`（传统 L184） | 2 处修改 |
| `wd_ecc.c` | 传递 `drv_bindings`（init2 L346）；传递 `NULL`（传统 L249） | 2 处修改 |
| `wd_agg.c` | 传递 `drv_bindings`（L725） | 1 处修改 |
| `wd_udma.c` | 传递 `drv_bindings`（L481） | 1 处修改 |
| `wd_join_gather.c` | 传递 `drv_bindings`（L812） | 1 处修改 |

### 详细修改

#### 1. `include/wd_util.h` — 添加字段并更新声明

- 第 136 行附近（`struct wd_init_attrs` 结尾处）：添加字段
  ```c
  struct wd_alg_driver **drv_bindings;
  ```
- 第 176 行：修改声明
  ```c
  int wd_ctx_drv_config(char *alg_name, struct wd_ctx_config_internal *ctx_config,
                        struct wd_alg_driver **pre_bound);
  ```

#### 2. `wd_util.c` — `wd_alg_attrs_init`（第 3149 行附近）

**2a.** 在局部变量区域（约第 3158 行）添加 `__u32 i, j;`。

**2b.** 修复 `TASK_MIX` 的引用计数泄漏（第 3207-3223 行）：在检查 `drv->calc_type` 之后，在调用 `wd_alg_other_ctx_init` 之前以及在 `else` 分支中，添加 `wd_release_drv(drv); drv = NULL;`。

**2c.** 修复 `TASK_INSTR` 的引用计数泄漏（第 3235-3251 行）：同样的模式。

**2d.** 在 `switch` 块结束之后（第 3256 行附近），在 `ctx_config->cap = attrs->ctx_params->cap;`（第 3258 行）之前，添加驱动绑定循环：

```c
attrs->drv_bindings = calloc(ctx_config->ctx_num,
                             sizeof(struct wd_alg_driver *));
if (!attrs->drv_bindings) {
    ret = -WD_ENOMEM;
    goto out_ctx_init;
}
for (i = 0; i < ctx_config->ctx_num; i++) {
    attrs->drv_bindings[i] = wd_alg_drv_bind(
        ctx_config->ctxs[i].ctx_type, alg_name);
    if (WD_IS_ERR(attrs->drv_bindings[i])) {
        attrs->drv_bindings[i] = NULL;
        continue;
    }
    if (!attrs->drv_bindings[i]) {
        ret = -WD_EINVAL;
        goto out_bind_err;
    }
}
```

**2e.** 在 `out_ctx_init`（第 3267 行之前）之前添加 `out_bind_err` 标签：

```c
out_bind_err:
    for (j = 0; j < i; j++) {
        if (attrs->drv_bindings[j]) {
            wd_alg_drv_unbind(attrs->drv_bindings[j]);
            attrs->drv_bindings[j] = NULL;
        }
    }
    free(attrs->drv_bindings);
    attrs->drv_bindings = NULL;
```

#### 3. `wd_util.c` — `wd_ctx_drv_config`（第 2754 行附近）

替换函数为新签名和实现，检查 `pre_bound`：

- 如果 `pre_bound` 且 `pre_bound[i]` 非空：直接使用 `pre_bound[i]`，设置 `pre_bound[i] = NULL`（所有权转移）
- 如果 `pre_bound` 但 `pre_bound[i] == NULL`：跳过（在 `attrs_init` 期间未找到驱动）
- 如果 `!pre_bound`：传统行为，调用 `wd_alg_drv_bind`

#### 4. `wd_util.c` — `wd_alg_attrs_uninit`（第 3277 行附近）

在 `wd_alg_ctxs_uninit(ctx_config)` 之前，添加一个循环释放 `drv_bindings` 中剩余的驱动（仅在错误路径中非空；在成功路径中所有条目已被 `wd_ctx_drv_config` 设置为 `NULL`），然后释放 `attrs->drv_bindings`。

#### 5. 所有算法文件 — 更新调用点

- **Init2 调用者（10 个文件）**：将 `wd_ctx_drv_config(alg, &setting.config)` 改为 `wd_ctx_drv_config(alg, &setting.config, <alg>_init_attrs.drv_bindings)`
- **传统调用者（7 个文件）**：将 `wd_ctx_drv_config("hardcoded", &setting.config)` 改为 `wd_ctx_drv_config("hardcoded", &setting.config, NULL)`

### 生命周期验证

**成功路径：**

1. `wd_alg_attrs_init` 分配 `drv_bindings`，绑定所有驱动（引用计数增加）
2. `wd_ctx_drv_config(pre_bound=drv_bindings)` 将每个驱动复制到 `internal.ctxs[i].drv`，设置 `pre_bound[i] = NULL`
3. `wd_alg_attrs_uninit`：清理循环为空操作（全为 `NULL`），释放数组
4. 反初始化：`wd_ctx_drv_deconfig` → 在每个 ctx 的驱动上调用 `wd_alg_drv_unbind`（引用计数减少）

**`wd_alg_attrs_init` 绑定循环中出错：**

- `out_bind_err` 解除绑定 `ctxs[0..i-1]`，释放数组，设置为 `NULL`
- 无泄漏

**`wd_ctx_drv_config` 中出错：**

- `bind_err` 解除绑定内部 `ctxs[0..i-1].drv`
- `pre_bound[k..n-1]` 仍然持有有效驱动
- 调用者的错误路径调用 `wd_alg_attrs_uninit` → 释放剩余的 `pre_bound[k..n-1]`
- 无双重释放，无泄漏

### 验证

- **构建**：`make -j$(nproc)` — 必须使用 `-Wall -Werror` 干净编译
- **传统路径**：验证所有 7 个传统初始化函数仍然工作（第三个参数为 `NULL` 意味着行为不变）
- **Init2 路径**：验证所有 10 个 init2 函数正确传递 `drv_bindings`
- **引用计数**：代码审查确认每个 `wd_request_drv` 都有匹配的 `wd_release_drv` / `wd_alg_drv_unbind`
- **TASK_HW / TASK_MIX / TASK_INSTR**：所有三种模式在 `attrs_init` 中正确绑定驱动
