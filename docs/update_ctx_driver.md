# UADK 框架层与用户态驱动层优化

## Context

当前 `wd_alg_init_driver` 框架层按 ctx 遍历调用 `wd_ctx_init_driver`，但同一个驱动的后续 ctx 只检查 `init_state` 后立即返回（空操作）。驱动 init 内部再次遍历所有 ctxs 分配 QP。两层遍历 O(2N)，效率低下。

**目标**: 框架层只遍历 unique driver，驱动层只存储属于自己的 ctx 镜像，消除双重遍历，降低层间耦合。

---

## 1. 框架层变更 (`wd_util.c`)

### 1.1 `wd_alg_init_driver` — 按 driver 迭代

```c
// 原来: 遍历所有 ctxs
for (i = 0; i < config->ctx_num; i++) {
    ret = wd_ctx_init_driver(config, &config->ctxs[i]);
}

// 改为: 遍历 drv_array (unique drivers)
for (i = 0; i < config->drv_count; i++) {
    ret = wd_ctx_init_driver(config, config->drv_array[i]);
}
```

### 1.2 `wd_ctx_init_driver` — 参数从 ctx 改为 driver

```c
// 原来: static int wd_ctx_init_driver(struct wd_ctx_config_internal *config,
//                                      struct wd_ctx_internal *ctx_config)

// 改为:
static int wd_ctx_init_driver(struct wd_ctx_config_internal *config,
                               struct wd_alg_driver *driver)
{
    void *priv;
    int ret;

    if (!driver || !driver->priv_size)
        return -WD_EINVAL;

    priv = calloc(1, driver->priv_size);
    if (!priv)
        return -WD_ENOMEM;

    ret = driver->init(config, priv);
    if (ret < 0) {
        free(priv);
        return ret;
    }
    driver->drv_data = priv;

    if (driver->fallback) {
        ret = wd_alg_init_fallback((struct wd_alg_driver *)driver->fallback);
        if (ret)
            driver->fallback = 0;
    }
    return 0;
}
```

消除 `init_state` 守卫、`drv_priv` 赋值——因为每个 driver 只被调用一次。

### 1.3 `wd_alg_uninit_driver` — 按 driver 迭代 + `wd_clear_ctx_config` 外提

```c
void wd_alg_uninit_driver(struct wd_ctx_config_internal *config)
{
    __u32 i;

    for (i = 0; i < config->drv_count; i++)
        wd_ctx_uninit_driver(config, config->drv_array[i]);

    wd_clear_ctx_config(config);  // 所有 driver exit 后统一清理一次
}
```

### 1.4 `wd_ctx_uninit_driver` — 参数从 ctx 改为 driver

```c
static void wd_ctx_uninit_driver(struct wd_ctx_config_internal *config,
                                  struct wd_alg_driver *driver)
{
    void *priv = driver->drv_data;

    if (!driver || !priv)
        return;

    driver->exit(priv);
    driver->drv_data = NULL;
    free(priv);

    if (driver->fallback)
        wd_alg_uninit_fallback((struct wd_alg_driver *)driver->fallback);
}
```

### 1.5 `drv_priv` 字段处理

`wd_ctx_internal.drv_priv` 当前从未被框架设置为有效值。`wd_ecc.c` 中三处 `ctx->drv_priv` 引用改为通过 `ctx->drv->drv_data` 获取：

```c
// wd_ecc.c:1623, 2310:
msg.priv = ctx->drv->drv_data;  // 替代 ctx->drv_priv
```

`wd_ctx_internal.drv_priv` 字段可标记为 deprecated，后续清理。

---

## 2. 驱动层变更 (以 `hisi_sec.c` 为例，其他 HW 驱动同理)

### 2.1 `struct hisi_sec_ctx` — 存储本驱动的 ctx 镜像

```c
// 原来:
struct hisi_sec_ctx {
    struct wd_ctx_config_internal config;  // 拷贝整个框架结构
};

// 改为:
struct hisi_sec_ctx {
    struct wd_ctx_internal **ctxs;  // 只存属于本驱动的 ctx 指针
    __u32 ctx_num;                   // 本驱动的 ctx 数量
};
```

### 2.2 `hisi_sec_init` — 只存储属于自己的 ctx

```c
static int hisi_sec_init(void *conf, void *priv)
{
    struct wd_ctx_config_internal *config = conf;
    struct hisi_sec_ctx *sec_ctx = priv;
    struct hisi_qm_priv qm_priv;
    handle_t h_qp;
    __u32 i, j, count;

    // 第一遍: 计数属于本驱动的 ctx
    count = 0;
    for (i = 0; i < config->ctx_num; i++) {
        if (config->ctxs[i].ctx_type == UADK_ALG_HW && config->ctxs[i].ctx)
            count++;
    }

    sec_ctx->ctxs = calloc(count, sizeof(struct wd_ctx_internal *));
    if (!sec_ctx->ctxs)
        return -WD_ENOMEM;
    sec_ctx->ctx_num = count;

    // 第二遍: 分配 QP + 存储 ctx 镜像
    qm_priv.sqe_size = sizeof(struct hisi_sec_sqe);
    count = 0;
    for (i = 0; i < config->ctx_num; i++) {
        if (config->ctxs[i].ctx_type != UADK_ALG_HW ||
            !config->ctxs[i].ctx)
            continue;

        qm_priv.op_type = 0;
        qm_priv.qp_mode = config->ctxs[i].ctx_mode;
        qm_priv.epoll_en = (qm_priv.qp_mode == CTX_MODE_SYNC) ?
                   config->epoll_en : 0;
        qm_priv.idx = i;
        h_qp = hisi_qm_alloc_qp(&qm_priv, config->ctxs[i].ctx);
        if (!h_qp)
            goto out;
        config->ctxs[i].sqn = qm_priv.sqn;

        sec_ctx->ctxs[count++] = &config->ctxs[i];
    }

    return 0;

out:
    for (j = 0; j < count; j++) {
        h_qp = (handle_t)wd_ctx_get_priv(sec_ctx->ctxs[j]->ctx);
        hisi_qm_free_qp(h_qp);
    }
    free(sec_ctx->ctxs);
    return -WD_EINVAL;
}
```

### 2.3 `hisi_sec_exit` — 从自己的镜像释放

```c
static void hisi_sec_exit(void *priv)
{
    struct hisi_sec_ctx *sec_ctx = priv;
    handle_t h_qp;
    __u32 i;

    for (i = 0; i < sec_ctx->ctx_num; i++) {
        h_qp = (handle_t)wd_ctx_get_priv(sec_ctx->ctxs[i]->ctx);
        hisi_qm_free_qp(h_qp);
    }
    free(sec_ctx->ctxs);
}
```

### 2.4 `hisi_sec_get_usage` — 从自己的镜像查询

```c
static int hisi_sec_get_usage(void *param)
{
    struct hisi_dev_usage *sec_usage = param;
    struct wd_alg_driver *drv = sec_usage->drv;
    struct hisi_sec_ctx *priv;
    char *ctx_dev_name;
    handle_t qp;
    __u32 i;

    priv = (struct hisi_sec_ctx *)drv->drv_data;
    if (!priv)
        return -WD_EACCES;

    for (i = 0; i < priv->ctx_num; i++) {
        ctx_dev_name = wd_ctx_get_dev_name(priv->ctxs[i]->ctx);
        if (!strcmp(sec_usage->dev_name, ctx_dev_name)) {
            qp = (handle_t)wd_ctx_get_priv(priv->ctxs[i]->ctx);
            if (qp)
                return hisi_qm_get_usage(qp, 0);
        }
    }

    return -WD_EACCES;
}
```

### 2.5 `get_extend_ops` — `ctxs[0]` 改为 `sec_ctx->ctxs[0]`

```c
// 原来: sec_ctx->config.ctxs[0].ctx
// 改为: sec_ctx->ctxs[0]->ctx
```

---

## 3. 对其他 HW 驱动的修改模式

| 驱动          | ctx 结构        | init            | exit            | get_usage                                   |
| ------------- | --------------- | --------------- | --------------- | ------------------------------------------- |
| `hisi_sec.c`  | `hisi_sec_ctx`  | `hisi_sec_init` | `hisi_sec_exit` | `hisi_sec_get_usage`                        |
| `hisi_hpre.c` | `hisi_hpre_ctx` | `hpre_*_init`   | `hpre_*_exit`   | `hpre_rsa_get_usage` / `hpre_ecc_get_usage` |
| `hisi_comp.c` | `hisi_zip_ctx`  | `hisi_zip_init` | `hisi_zip_exit` | `hisi_zip_get_usage`                        |
| `hisi_dae.h`  | `hisi_dae_ctx`  | `dae_init`      | `dae_exit`      | `dae_get_usage`                             |
| `hisi_udma.c` | `hisi_udma_ctx` | `udma_init`     | `udma_exit`     | `udma_get_usage`                            |

每个驱动改动模式相同：

1. `struct hisi_*_ctx`: `wd_ctx_config_internal config` → `wd_ctx_internal **ctxs + __u32 ctx_num`
2. init: 过滤 `ctx_type == UADK_ALG_HW` 的 ctx 存入镜像
3. exit: 遍历自有镜像释放
4. get_usage: 遍历自有镜像查找 dev_name

ISA 驱动 (`isa_ce_sm3.c`, `isa_ce_sm4.c`, `hash_mb.c`) 的 get_usage 返回 0（空实现），改动仅需适配框架层签名变化。

---

## 4. 优化效果对比

| 维度                 | 优化前                              | 优化后                                |
| -------------------- | ----------------------------------- | ------------------------------------- |
| 框架层 init 遍历次数 | ctx_num (如 16)                     | drv_count (如 2~3)                    |
| 驱动层 init 遍历次数 | ctx_num (全部，含其他驱动)          | ctx_num (仅本驱动)                    |
| 驱动存储内容         | 完整 `wd_ctx_config_internal`       | 仅本驱动的 ctx 指针数组               |
| 层间耦合             | 驱动依赖框架 config 结构定义        | 驱动只保存 `wd_ctx_internal *` 指针   |
| get_usage 查询路径   | `drv_data → config → 遍历所有 ctxs` | `drv_data → ctxs[] → 遍历本驱动 ctxs` |

---

## 5. 受影响的文件汇总

```
include/wd_internal.h      — wd_ctx_internal.drv_priv 标记 deprecated
wd_util.c                   — wd_alg_init_driver, wd_alg_uninit_driver,
                              wd_ctx_init_driver, wd_ctx_uninit_driver
wd_ecc.c                    — ctx->drv_priv → ctx->drv->drv_data
drv/hisi_sec.c              — hisi_sec_ctx, init, exit, get_usage
drv/hisi_hpre.c             — hisi_hpre_ctx, init, exit, get_usage×2
drv/hisi_comp.c             — hisi_zip_ctx, init, exit, get_usage
drv/hisi_dae_common.c       — hisi_dae_ctx, init, exit, get_usage
drv/hisi_udma.c             — hisi_udma_ctx, init, exit, get_usage
drv/isa_ce_sm3.c            — get_usage (签名适配)
drv/isa_ce_sm4.c            — get_usage (签名适配)
drv/hash_mb/hash_mb.c       — get_usage (签名适配)
```

## 6. 验证

1. `make clean && make` — 编译通过
2. `uadk_tool/test/` — sec/comp 功能测试
3. `uadk_tool/benchmark/` — 性能对比
4. `wd_get_dev_usage` — 多设备场景 get_usage 正确
5. valgrind — 无内存泄漏
