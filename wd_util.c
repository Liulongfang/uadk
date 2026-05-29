// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2020-2021 Huawei Technologies Co.,Ltd. All rights reserved.
 * Copyright 2020-2021 Linaro ltd.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>
#include <ctype.h>
#include "wd_sched.h"
#include "wd_util.h"
#include "wd_alg.h"
#include "wd_bmm.h"

#define WD_ASYNC_DEF_POLL_NUM		1
#define WD_ASYNC_DEF_QUEUE_DEPTH	1024
#define WD_BALANCE_THRHD		1280
#define WD_RECV_MAX_CNT_SLEEP		60000000
#define WD_RECV_MAX_CNT_NOSLEEP		200000000
#define PRIVILEGE_FLAG			0600
#define MIN(a, b)			((a) > (b) ? (b) : (a))
#define MAX(a, b)			((a) > (b) ? (a) : (b))

#define WD_INIT_SLEEP_UTIME		1000
#define WD_INIT_RETRY_TIMES		10000
#define US2S(us)			((us) >> 20)
#define WD_INIT_RETRY_TIMEOUT		3

#define WD_SOFT_CTX_NUM		2
#define WD_SOFT_SYNC_CTX		0
#define WD_SOFT_ASYNC_CTX		1
#define WD_DRV_MAX_NUM		128

#define WD_DRV_LIB_DIR			"uadk"
#define WD_DRV_CONF_FILE		"uadk.cnf"

#define WD_PATH_DIR_NUM			2

struct msg_pool {
	/* message array allocated dynamically */
	void *msgs;
	int *used;
	__u32 msg_num;
	__u32 msg_size;
	int tail;
};

/* parse wd env begin */

/* define comp's combination of two operation type and two mode here */
static const char *comp_ctx_type[2][2] = {
	{"sync-comp:", "sync-decomp:"},
	{"async-comp:", "async-decomp:"}
};

/* define two ctx mode here for cipher and other alg */
static const char *ctx_type[2][1] = { {"sync:"}, {"async:"} };

static const char *wd_env_name[WD_TYPE_MAX] = {
	"WD_COMP_CTX_NUM",
	"WD_CIPHER_CTX_NUM",
	"WD_DIGEST_CTX_NUM",
	"WD_AEAD_CTX_NUM",
	"WD_RSA_CTX_NUM",
	"WD_DH_CTX_NUM",
	"WD_ECC_CTX_NUM",
	"WD_AGG_CTX_NUM",
	"WD_UDMA_CTX_NUM",
	"WD_JOIN_GATHER_CTX_NUM",
};

struct drv_lib_list {
	void *dlhandle;
	struct drv_lib_list *next;
};

static void *wd_internal_alloc(void *usr, size_t size)
{
	if (size != 0)
		return malloc(size);
	else
		return NULL;
}

static void wd_internal_free(void *usr, void *va)
{
	if (va != NULL)
		free(va);
}

static __u32 wd_mem_bufsize(void *usr)
{
	/* Malloc memory min size is 1 Byte */
	return 1;
}

int wd_mem_ops_init(handle_t h_ctx, struct wd_mm_ops *mm_ops, int mem_type)
{
	int ret;

	ret = wd_is_sva(h_ctx);
	if (ret == UACCE_DEV_SVA || ret == -WD_HW_EACCESS) {
		/*
		 * In software queue scenario, all memory is handled as virtual memory
		 * and processed in the same way as SVA mode
		 */
		mm_ops->sva_mode = true;
	} else if (!ret) {
		mm_ops->sva_mode = false;
	} else {
		WD_ERR("failed to check ctx!\n");
		return ret;
	}

	/*
	 * Under SVA mode, there is no need to consider the memory type;
	 * directly proceed with virtual memory handling
	 */
	if (mm_ops->sva_mode) {
		mm_ops->alloc = (void *)wd_internal_alloc;
		mm_ops->free = (void *)wd_internal_free;
		mm_ops->iova_map = NULL;
		mm_ops->iova_unmap = NULL;
		mm_ops->get_bufsize = (void *)wd_mem_bufsize;
		mm_ops->usr = NULL;
		return 0;
	}

	switch (mem_type) {
	case UADK_MEM_AUTO:
		/*
		 * The memory pool needs to be allocated according to
		 * the block size when it is first executed in the UADK
		 */
		mm_ops->usr = NULL;
		WD_ERR("automatic under No-SVA mode is not supported!\n");
		return -WD_EINVAL;
	case UADK_MEM_USER:
		if (!mm_ops->alloc || !mm_ops->free || !mm_ops->iova_map ||
		    !mm_ops->iova_unmap || !mm_ops->usr) { // The user create a memory pool
			WD_ERR("failed to check memory ops, some ops function is NULL!\n");
			return -WD_EINVAL;
		}
		break;
	case UADK_MEM_PROXY:
		if (!mm_ops->usr) {
			WD_ERR("failed to check memory pool!\n");
			return -WD_EINVAL;
		}
		mm_ops->alloc = (void *)wd_mem_alloc;
		mm_ops->free = (void *)wd_mem_free;
		mm_ops->iova_map = (void *)wd_mem_map;
		mm_ops->iova_unmap = (void *)wd_mem_unmap;
		mm_ops->get_bufsize = (void *)wd_get_bufsize;
		break;
	default:
		WD_ERR("failed to check memory type!\n");
		return -WD_EINVAL;
	}

	return 0;
}

static void clone_ctx_to_internal(struct wd_ctx *ctx,
					  struct wd_ctx_internal *ctx_in)
{
	ctx_in->ctx = ctx->ctx;
	ctx_in->op_type = ctx->op_type;
	ctx_in->ctx_mode = ctx->ctx_mode;
}

static int wd_shm_create(struct wd_ctx_config_internal *in)
{
	int shm_size = sizeof(unsigned long) * WD_CTX_CNT_NUM;
	void *ptr;
	int shmid;

	if (!wd_need_info())
		return 0;

	shmid = shmget(WD_IPC_KEY, shm_size, IPC_CREAT | PRIVILEGE_FLAG);
	if (shmid < 0) {
		WD_ERR("failed to get shared memory id(%d).\n", errno);
		return -WD_EINVAL;
	}

	ptr = shmat(shmid, NULL, 0);
	if (ptr == (void *)-1) {
		WD_ERR("failed to get shared memory addr(%d).\n", errno);
		return -WD_EINVAL;
	}

	memset(ptr, 0, shm_size);

	in->shmid = shmid;
	in->msg_cnt = ptr;

	return 0;
}

static void wd_shm_delete(struct wd_ctx_config_internal *in)
{
	if (!wd_need_info())
		return;

	/* deleted shared memory */
	shmdt(in->msg_cnt);
	shmctl(in->shmid, IPC_RMID, NULL);

	in->shmid = 0;
	in->msg_cnt = NULL;
}

int wd_init_ctx_config(struct wd_ctx_config_internal *in,
		       struct wd_ctx_config *cfg)
{
	struct wd_ctx_internal *ctxs;
	__u32 i, j;
	int ret;

	if (!cfg->ctx_num) {
		WD_ERR("invalid: ctx_num is 0!\n");
		return -WD_EINVAL;
	}

	ret = wd_shm_create(in);
	if (ret)
		return ret;

	ctxs = calloc(1, cfg->ctx_num * sizeof(struct wd_ctx_internal));
	if (!ctxs) {
		WD_ERR("failed to alloc memory for internal ctxs!\n");
		ret = -WD_ENOMEM;
		goto err_shm_del;
	}

	for (i = 0; i < cfg->ctx_num; i++) {
		if (!cfg->ctxs[i].ctx) {
			WD_ERR("invalid: ctx<%u> is NULL!\n", i);
			break;
		}
		clone_ctx_to_internal(cfg->ctxs + i, ctxs + i);
		ret = pthread_spin_init(&ctxs[i].lock, PTHREAD_PROCESS_SHARED);
		if (ret) {
			WD_ERR("failed to init ctxs lock!\n");
			goto err_out;
		}

		ret = wd_insert_ctx_list(cfg->ctxs[i].ctx, in->alg_name);
		if (ret) {
			WD_ERR("failed to add ctx to mem list!\n");
			goto err_out;
		}
	}

	in->ctxs = ctxs;
	in->priv = cfg->priv;
	in->ctx_num = cfg->ctx_num;

	return 0;

err_out:
	for (j = 0; j < i; j++)
		pthread_spin_destroy(&ctxs[j].lock);
	free(ctxs);
err_shm_del:
	wd_shm_delete(in);
	return ret;
}

int wd_init_sched(struct wd_sched *in, struct wd_sched *from)
{
	if (!from->name || !from->sched_init || !from->set_param ||
	    !from->pick_next_ctx || !from->poll_policy) {
		WD_ERR("invalid: member of wd_sched is NULL!\n");
		return -WD_EINVAL;
	}

	in->h_sched_ctx = from->h_sched_ctx;
	in->name = strdup(from->name);
	in->sched_init = from->sched_init;
	in->pick_next_ctx = from->pick_next_ctx;
	in->poll_policy = from->poll_policy;
	in->set_param = from->set_param;

	return 0;
}

void wd_clear_sched(struct wd_sched *in)
{
	char *name = (char *)in->name;

	if (name)
		free(name);
	in->h_sched_ctx = 0;
	in->name = NULL;
	in->sched_init = NULL;
	in->pick_next_ctx = NULL;
	in->poll_policy = NULL;
	in->set_param = NULL;
}

void wd_clear_ctx_config(struct wd_ctx_config_internal *in)
{
	__u32 i;

	for (i = 0; i < in->ctx_num; i++)
		pthread_spin_destroy(&in->ctxs[i].lock);

	in->priv = NULL;
	in->ctx_num = 0;
	if (in->ctxs) {
		free(in->ctxs);
		in->ctxs = NULL;
	}

	wd_remove_ctx_list();
	wd_shm_delete(in);
}

void wd_memset_zero(void *data, __u32 size)
{
	__u32 tmp = size;
	char *s = data;

	if (!s)
		return;

	while (tmp--)
		*s++ = 0;
}

static void get_ctx_msg_num(struct wd_cap_config *cap, __u32 *msg_num)
{
	if (!cap || !cap->ctx_msg_num)
		return;

	if (cap->ctx_msg_num > WD_POOL_MAX_ENTRIES) {
		WD_INFO("ctx_msg_num %u is invalid, use default value: %u!\n",
			cap->ctx_msg_num, *msg_num);
		return;
	}

	*msg_num = cap->ctx_msg_num;
}

static int init_msg_pool(struct msg_pool *pool, __u32 msg_num, __u32 msg_size)
{
	pool->msgs = calloc(1, msg_num * msg_size);
	if (!pool->msgs) {
		WD_ERR("failed to alloc memory for msgs arrary of msg pool!\n");
		return -WD_ENOMEM;
	}

	pool->used = calloc(1, msg_num * sizeof(int));
	if (!pool->used) {
		free(pool->msgs);
		pool->msgs = NULL;
		WD_ERR("failed to alloc memory for used arrary of msg pool!\n");
		return -WD_ENOMEM;
	}

	pool->msg_size = msg_size;
	pool->msg_num = msg_num;
	pool->tail = 0;

	return 0;
}

static void uninit_msg_pool(struct msg_pool *pool)
{
	if (!pool->msg_num)
		return;

	free(pool->msgs);
	free(pool->used);
	pool->msgs = NULL;
	pool->used = NULL;
	memset(pool, 0, sizeof(*pool));
}

int wd_init_async_request_pool(struct wd_async_msg_pool *pool, struct wd_ctx_config *config,
			       __u32 msg_num, __u32 msg_size)
{
	__u32 pool_num = config->ctx_num;
	__u32 i, j;
	int ret;

	pool->pool_num = pool_num;

	pool->pools = calloc(1, pool_num * sizeof(struct msg_pool));
	if (!pool->pools) {
		WD_ERR("failed to alloc memory for async msg pools!\n");
		return -WD_ENOMEM;
	}

	/* If user set valid msg num, use user's. */
	get_ctx_msg_num(config->cap, &msg_num);
	for (i = 0; i < pool_num; i++) {
		if (config->ctxs[i].ctx_mode == CTX_MODE_SYNC)
			continue;

		ret = init_msg_pool(&pool->pools[i], msg_num, msg_size);
		if (ret < 0)
			goto err;
	}

	return 0;
err:
	for (j = 0; j < i; j++)
		uninit_msg_pool(&pool->pools[j]);
	free(pool->pools);
	pool->pools = NULL;
	return ret;
}

void wd_uninit_async_request_pool(struct wd_async_msg_pool *pool)
{
	__u32 i;

	for (i = 0; i < pool->pool_num; i++)
		uninit_msg_pool(&pool->pools[i]);

	free(pool->pools);
	pool->pools = NULL;
	pool->pool_num = 0;
}

void *wd_find_msg_in_pool(struct wd_async_msg_pool *pool,
			  int ctx_idx, __u32 tag)
{
	struct msg_pool *p;
	__u32 msg_num;

	if ((__u32)ctx_idx > pool->pool_num) {
		WD_ERR("invalid: message ctx id index is %d!\n", ctx_idx);
		return NULL;
	}
	p = &pool->pools[ctx_idx];
	msg_num = p->msg_num;

	/* tag value start from 1 */
	if (tag == 0 || tag > msg_num) {
		WD_ERR("invalid: message cache tag is %u!\n", tag);
		return NULL;
	}

	return (void *)((uintptr_t)p->msgs + p->msg_size * (tag - 1));
}

int wd_get_msg_from_pool(struct wd_async_msg_pool *pool,
			 int ctx_idx, void **msg)
{
	struct msg_pool *p = &pool->pools[ctx_idx];
	__u32 msg_num = p->msg_num;
	__u32 msg_size = p->msg_size;
	__u32 cnt = 0;
	__u32 idx = p->tail;

	/* Scheduler set a sync ctx */
	if (!msg_num)
		return -WD_EINVAL;

	while (__atomic_test_and_set(&p->used[idx], __ATOMIC_ACQUIRE)) {
		idx = (idx + 1) % msg_num;
		cnt++;
		if (cnt == msg_num)
			return -WD_EBUSY;
	}

	p->tail = (idx + 1) % msg_num;
	*msg = (void *)((uintptr_t)p->msgs + msg_size * idx);

	return idx + 1;
}

void wd_put_msg_to_pool(struct wd_async_msg_pool *pool, int ctx_idx, __u32 tag)
{
	struct msg_pool *p = &pool->pools[ctx_idx];
	__u32 msg_num = p->msg_num;

	/* tag value start from 1 */
	if (!tag || tag > msg_num) {
		WD_ERR("invalid: message cache idx is %u!\n", tag);
		return;
	}

	__atomic_clear(&p->used[tag - 1], __ATOMIC_RELEASE);
}

int wd_check_src_dst(void *src, __u32 in_bytes, void *dst, __u32 out_bytes)
{
	if ((in_bytes && !src) || (out_bytes && !dst))
		return -WD_EINVAL;

	return 0;
}

int wd_check_datalist(struct wd_datalist *head, __u64 size)
{
	struct wd_datalist *tmp = head;
	__u64 list_size = 0;

	while (tmp) {
		if (tmp->data)
			list_size += tmp->len;

		tmp = tmp->next;
	}

	return list_size >= size ? 0 : -WD_EINVAL;
}

void dump_env_info(struct wd_env_config *config)
{
	struct wd_env_config_per_numa *config_numa;
	struct wd_ctx_range **ctx_table;
	int i, j, k;

	FOREACH_NUMA(i, config, config_numa) {
		if (!config_numa->ctx_table)
			continue;

		ctx_table = config_numa->ctx_table;
		WD_ERR("-> %s: %d: sync num: %u\n", __func__, i,
		       config_numa->sync_ctx_num);
		WD_ERR("-> %s: %d: async num: %u\n", __func__, i,
		       config_numa->async_ctx_num);
		for (j = 0; j < CTX_MODE_MAX; j++)
			for (k = 0; k < config_numa->op_type_num; k++) {
				WD_ERR("-> %d: [%d][%d].begin: %u\n",
				       i, j, k, ctx_table[j][k].begin);
				WD_ERR("-> %d: [%d][%d].end: %u\n",
				       i, j, k, ctx_table[j][k].end);
				WD_ERR("-> %d: [%d][%d].size: %u\n",
				       i, j, k, ctx_table[j][k].size);
			}
	}
}

static void *wd_get_config_numa(struct wd_env_config *config, int node)
{
	struct wd_env_config_per_numa *config_numa;
	int i;

	FOREACH_NUMA(i, config, config_numa)
		if (config_numa->node == node)
			break;

	if (i == config->numa_num) {
		WD_ERR("invalid: missing numa node is %d!\n", node);
		return NULL;
	}

	return config_numa;
}

static void wd_free_numa(struct wd_env_config *config)
{
	struct wd_env_config_per_numa *config_numa;
	int i;

	FOREACH_NUMA(i, config, config_numa)
		free(config_numa->dev);

	free(config->config_per_numa);
	config->config_per_numa = NULL;
	config->numa_num = 0;
}

/**
 * @numa_dev_num: number of devices of the same type (like sec2) on each numa.
 * @numa_num: number of numa node that has this type of device.
 */
static __u16 wd_get_dev_numa(struct uacce_dev_list *head,
			     int *numa_dev_num, __u16 size)
{
	struct uacce_dev_list *list = head;
	__u16 numa_num = 0;

	while (list) {
		if (list->dev->numa_id >= size) {
			WD_ERR("invalid: numa id is %d!\n", list->dev->numa_id);
			return 0;
		}

		if (!numa_dev_num[list->dev->numa_id])
			numa_num++;

		numa_dev_num[list->dev->numa_id]++;
		list = list->next;
	}

	return numa_num;
}

static void wd_set_numa_dev(struct uacce_dev_list *head,
			    struct wd_env_config *config)
{
	struct uacce_dev_list *list = head;
	struct wd_env_config_per_numa *config_numa;
	struct uacce_dev *dev;

	while (list) {
		config_numa = wd_get_config_numa(config, list->dev->numa_id);
		if (!config_numa)
			break;

		dev = config_numa->dev + config_numa->dev_num;
		memcpy(dev, list->dev, sizeof(*list->dev));
		config_numa->dev_num++;
		list = list->next;
	}
}

static int wd_set_config_numa(struct wd_env_config *config,
			      const int *numa_dev_num, int max_node)
{
	struct wd_env_config_per_numa *config_numa;
	int i;

	config->config_per_numa = calloc(config->numa_num, sizeof(*config_numa));
	if (!config->config_per_numa)
		return -WD_ENOMEM;

	config_numa = config->config_per_numa;
	for (i = 0; i < max_node; i++) {
		if (!numa_dev_num[i])
			continue;

		config_numa->node = i;
		config_numa->dev = calloc(numa_dev_num[i],
					  sizeof(struct uacce_dev));
		if (!config_numa->dev) {
			/* free config_per_numa and all uacce dev */
			wd_free_numa(config);
			return -WD_ENOMEM;
		}

		config_numa->dev_num = 0;
		config_numa++;
	}

	return 0;
}

static int wd_alloc_numa(struct wd_env_config *config,
			 const struct wd_alg_ops *ops)
{
	struct uacce_dev_list *head;
	int *numa_dev_num;
	int ret, max_node;

	max_node = numa_max_node() + 1;
	if (max_node <= 0)
		return -WD_EINVAL;

	numa_dev_num = calloc(max_node, sizeof(int));
	if (!numa_dev_num)
		return -WD_ENOMEM;

	/* get uacce_dev */
	head = wd_get_accel_list(ops->alg_name);
	if (!head) {
		WD_ERR("invalid: no device to support %s\n", ops->alg_name);
		ret = -WD_ENODEV;
		goto free_numa_dev_num;
	}

	/* get numa num and device num of each numa from uacce_dev list */
	config->numa_num = wd_get_dev_numa(head, numa_dev_num, max_node);
	if (config->numa_num == 0 || config->numa_num > max_node) {
		WD_ERR("invalid: numa number is %u!\n", config->numa_num);
		ret = -WD_ENODEV;
		goto free_list;
	}

	/* alloc and init config_per_numa and all uacce dev */
	ret = wd_set_config_numa(config, numa_dev_num, max_node);
	if (ret) {
		WD_ERR("failed to set numa config, ret = %d!\n", ret);
		goto free_list;
	}

	/* set device and device num for config numa from uacce_dev list */
	wd_set_numa_dev(head, config);
	wd_free_list_accels(head);
	free(numa_dev_num);

	return 0;

free_list:
	config->numa_num = 0;
	wd_free_list_accels(head);
free_numa_dev_num:
	free(numa_dev_num);
	return ret;
}

static int is_number(const char *str)
{
	size_t i, len;

	if (!str)
		return 0;

	len = strlen(str);
	if (len == 0)
		return 0;

	if (len != 1 && str[0] == '0')
		return 0;

	for (i = 0; i < len; i++)
		if (!(isdigit(str[i])))
			return 0;

	return 1;
}

static int str_to_bool(const char *s, bool *target)
{
	int tmp;

	if (!is_number(s))
		return -WD_EINVAL;

	tmp = strtol(s, NULL, 10);
	if (tmp != 0 && tmp != 1)
		return -WD_EINVAL;

	*target = tmp;

	return 0;
}

static int parse_num_on_numa(const char *s, int *num, int *node)
{
	char *sep, *start, *left;

	if (!strlen(s)) {
		WD_ERR("invalid: input string length is zero!\n");
		return -WD_EINVAL;
	}

	start = strdup(s);
	if (!start)
		return -WD_ENOMEM;

	left = start;
	sep = strsep(&left, "@");
	if (!sep)
		goto out;

	if (is_number(sep) && is_number(left)) {
		*num = strtol(sep, NULL, 10);
		*node = strtol(left, NULL, 10);
		free(start);
		return 0;
	}

out:
	WD_ERR("invalid: input env format is %s!\n", s);
	free(start);
	return -WD_EINVAL;
}

static int wd_alloc_ctx_table_per_numa(struct wd_env_config_per_numa *config)
{
	struct wd_ctx_range **ctx_table;
	int i, j, ret;

	if (config->ctx_table)
		return 0;

	ctx_table = calloc(1, sizeof(struct wd_ctx_range *) * CTX_MODE_MAX);
	if (!ctx_table)
		return -WD_ENOMEM;

	for (i = 0; i < CTX_MODE_MAX; i++) {
		ctx_table[i] = calloc(1,
				sizeof(struct wd_ctx_range) *
				config->op_type_num);
		if (!ctx_table[i]) {
			ret = -WD_ENOMEM;
			goto free_mem;
		}
	}

	config->ctx_table = ctx_table;

	return 0;

free_mem:
	for (j = 0; j < i; j++)
		free(ctx_table[j]);

	free(ctx_table);
	return ret;
}

static void wd_free_ctx_table_per_numa(struct wd_env_config_per_numa *config)
{
	int i;

	if (!config->ctx_table)
		return;

	for (i = 0; i < CTX_MODE_MAX; i++)
		free(config->ctx_table[i]);

	free(config->ctx_table);
	config->ctx_table = NULL;
}

static void wd_free_ctx_table(struct wd_env_config *config)
{
	struct wd_env_config_per_numa *config_numa;
	int i;

	FOREACH_NUMA(i, config, config_numa)
		wd_free_ctx_table_per_numa(config_numa);
}

static int get_and_fill_ctx_num(struct wd_env_config_per_numa *config_numa,
				const char *p, int ctx_num)
{
	struct wd_ctx_range **ctx_table = config_numa->ctx_table;
	const char *type;
	int i, j;

	/**
	 * There're two types of environment variables, mode:num@node and
	 * mode-type:num@node, parse ctx num with comp_ctx_type and ctx_type.
	 */

	for (i = 0; i < CTX_MODE_MAX; i++)
		for (j = 0; j < config_numa->op_type_num; j++) {
			if (config_numa->op_type_num == 1)
				type = ctx_type[i][j];
			else
				type = comp_ctx_type[i][j];

			if (!strncmp(p, type, strlen(type))) {
				ctx_table[i][j].size = ctx_num;
				return 0;
			}
		}

	return -WD_EINVAL;
}

static int wd_parse_section(struct wd_env_config *config, char *section)
{
	struct wd_env_config_per_numa *config_numa;
	int ctx_num, node, ret;
	char *ctx_section;

	ctx_section = index(section, ':');
	if (!ctx_section) {
		WD_ERR("invalid: ctx section got wrong format: %s!\n", section);
		return -WD_EINVAL;
	}

	ctx_section++;

	ret = parse_num_on_numa(ctx_section, &ctx_num, &node);
	if (ret)
		return ret;

	config_numa = wd_get_config_numa(config, node);
	if (!config_numa)
		return -WD_EINVAL;

	config_numa->op_type_num = config->op_type_num;
	ret = wd_alloc_ctx_table_per_numa(config_numa);
	if (ret)
		return ret;

	ret = get_and_fill_ctx_num(config_numa, section, ctx_num);
	if (ret) {
		WD_ERR("invalid: ctx section got wrong ctx type: %s!\n",
		       section);
		wd_free_ctx_table_per_numa(config_numa);
		return ret;
	}

	return 0;
}

static int get_start_ctx_index(struct wd_env_config *config,
			       struct wd_env_config_per_numa *config_numa)
{
	struct wd_env_config_per_numa *config_cur = config->config_per_numa;
	int start = 0;

	for (; config_cur < config_numa; config_cur++)
		start += config_cur->sync_ctx_num + config_cur->async_ctx_num;

	return start;
}

static void set_ctx_index(struct wd_env_config_per_numa *config_numa,
			  __u8 mode, int *start)
{
	struct wd_ctx_range **ctx_table = config_numa->ctx_table;
	int size, i, sum = 0;

	for (i = 0; i < config_numa->op_type_num; i++)
		sum += ctx_table[mode][i].size;

	if (mode)
		config_numa->async_ctx_num = sum;
	else
		config_numa->sync_ctx_num = sum;

	if (!sum)
		return;

	for (i = 0; i < config_numa->op_type_num; i++) {
		size = ctx_table[mode][i].size;
		if (!size)
			continue;
		ctx_table[mode][i].begin = *start;
		ctx_table[mode][i].end = *start + size - 1;
		*start += size;
	}
}

static void wd_fill_ctx_table(struct wd_env_config *config)
{
	struct wd_env_config_per_numa *config_numa;
	int start, i, j;

	FOREACH_NUMA(i, config, config_numa) {
		if (!config_numa->ctx_table)
			continue;

		start = get_start_ctx_index(config, config_numa);
		for (j = 0; j < CTX_MODE_MAX; j++)
			set_ctx_index(config_numa, j, &start);
	}
}

static int parse_ctx_num(struct wd_env_config *config, const char *s)
{
	char *left, *section, *start;
	int ret;

	start = strdup(s);
	if (!start)
		return -WD_ENOMEM;

	left = start;

	while ((section = strsep(&left, ","))) {
		ret = wd_parse_section(config, section);
		if (ret)
			goto err_free_ctx_table;
	}

	wd_fill_ctx_table(config);
	free(start);

	return 0;

err_free_ctx_table:
	wd_free_ctx_table(config);
	free(start);
	return ret;
}

int wd_parse_ctx_num(struct wd_env_config *config, const char *s)
{
	return parse_ctx_num(config, s);
}

static int wd_parse_env(struct wd_env_config *config)
{
	const struct wd_config_variable *var;
	const char *var_s;
	int ret;
	__u32 i;

	for (i = 0; i < config->table_size; i++) {
		var = config->table + i;

		var_s = secure_getenv(var->name);
		if (!var_s || !strlen(var_s)) {
			var_s = var->def_val;
			WD_INFO("no %s environment variable! Use default: %s\n",
				var->name, var->def_val);
		}

		ret = var->parse_fn(config, var_s);
		if (ret) {
			WD_ERR("failed to parse %s environment variable!\n",
			       var->name);
			return -WD_EINVAL;
		}
	}

	return 0;
}

static int wd_parse_ctx_attr(struct wd_env_config *env_config,
			     struct wd_ctx_attr *attr)
{
	struct wd_env_config_per_numa *config_numa;
	int ret;

	config_numa = wd_get_config_numa(env_config, attr->node);
	if (!config_numa)
		return -WD_EINVAL;

	config_numa->op_type_num = env_config->op_type_num;
	ret = wd_alloc_ctx_table_per_numa(config_numa);
	if (ret)
		return ret;

	config_numa->ctx_table[attr->mode][attr->type].size = attr->num;
	wd_fill_ctx_table(env_config);

	/* Use default sched and disable internal poll */
	env_config->sched = NULL;

	return 0;
}

static int wd_init_env_config(struct wd_env_config *config,
			      struct wd_ctx_attr *attr,
			      const struct wd_alg_ops *ops,
			      const struct wd_config_variable *table,
			      __u32 table_size)
{
	config->op_type_num = ops->op_type_num;
	config->table_size = table_size;
	config->table = table;

	return attr ? wd_parse_ctx_attr(config, attr) : wd_parse_env(config);
}

static void wd_uninit_env_config(struct wd_env_config *config)
{
	wd_free_ctx_table(config);

	config->op_type_num = 0;
	config->table_size = 0;
	config->table = NULL;
}

static __u8 get_ctx_mode(struct wd_env_config_per_numa *config, __u32 idx)
{
	struct wd_ctx_range **ctx_table = config->ctx_table;
	__u32 i;

	for (i = 0; i < config->op_type_num; i++) {
		if ((idx >= ctx_table[CTX_MODE_SYNC][i].begin) &&
		    (idx <= ctx_table[CTX_MODE_SYNC][i].end) &&
		    ctx_table[CTX_MODE_SYNC][i].size)
			return CTX_MODE_SYNC;
	}
	return CTX_MODE_ASYNC;
}

static int get_op_type(struct wd_env_config_per_numa *config,
		       __u32 idx, __u8 ctx_mode)
{
	struct wd_ctx_range **ctx_table = config->ctx_table;
	__u32 i;

	if (config->op_type_num == 1)
		return 0;

	for (i = 0; i < config->op_type_num; i++) {
		if ((idx >= ctx_table[ctx_mode][i].begin) &&
		    (idx <= ctx_table[ctx_mode][i].end) &&
		    ctx_table[ctx_mode][i].size)
			return i;
	}

	WD_ERR("failed to get op type!\n");
	return -WD_EINVAL;
}

static handle_t request_ctx_on_numa(struct wd_env_config_per_numa *config)
{
	struct uacce_dev *dev;
	handle_t h_ctx;
	int i, ctx_num;

	for (i = 0; i < config->dev_num; i++) {
		dev = config->dev + i;
		ctx_num = wd_get_avail_ctx(dev);
		if (ctx_num <= 0)
			continue;

		h_ctx = wd_request_ctx(dev);
		if (h_ctx)
			return h_ctx;
	}

	return 0;
}

static int wd_get_wd_ctx(struct wd_env_config_per_numa *config,
			 struct wd_ctx_config *ctx_config, __u32 start)
{
	int ctx_num = config->sync_ctx_num + config->async_ctx_num;
	handle_t h_ctx;
	__u32 i, j;
	int ret;

	if (!ctx_num)
		return 0;

	for (i = start; i < start + ctx_num; i++) {
		h_ctx = request_ctx_on_numa(config);
		if (!h_ctx) {
			ret = -WD_EBUSY;
			WD_ERR("failed to request more ctxs!\n");
			goto free_ctx;
		}

		ctx_config->ctxs[i].ctx = h_ctx;
		ctx_config->ctxs[i].ctx_mode = get_ctx_mode(config, i);
		ret = get_op_type(config, i, ctx_config->ctxs[i].ctx_mode);
		if (ret < 0) {
			wd_release_ctx(ctx_config->ctxs[i].ctx);
			goto free_ctx;
		}

		ctx_config->ctxs[i].op_type = ret;
	}

	return 0;

free_ctx:
	for (j = start; j < i; j++)
		wd_release_ctx(ctx_config->ctxs[j].ctx);
	return ret;
}

static void wd_put_wd_ctx(struct wd_ctx_config *ctx_config, __u32 ctx_num)
{
	__u32 i;

	for (i = 0; i < ctx_num; i++)
		wd_release_ctx(ctx_config->ctxs[i].ctx);
}

static int wd_alloc_ctx(struct wd_env_config *config)
{
	struct wd_env_config_per_numa *config_numa;
	struct wd_ctx_config *ctx_config;
	__u32 i, ctx_num = 0, start = 0;
	int ret;

	config->ctx_config = calloc(1, sizeof(*ctx_config));
	if (!config->ctx_config)
		return -WD_ENOMEM;

	ctx_config = config->ctx_config;

	FOREACH_NUMA(i, config, config_numa)
		ctx_num += config_numa->sync_ctx_num + config_numa->async_ctx_num;

	ctx_config->ctxs = calloc(ctx_num, sizeof(struct wd_ctx));
	if (!ctx_config->ctxs) {
		ret = -WD_ENOMEM;
		goto err_free_ctx_config;
	}
	ctx_config->ctx_num = ctx_num;

	FOREACH_NUMA(i, config, config_numa) {
		ret = wd_get_wd_ctx(config_numa, ctx_config, start);
		if (ret)
			goto err_free_ctxs;

		start += config_numa->sync_ctx_num + config_numa->async_ctx_num;
	}

	return 0;

err_free_ctxs:
	wd_put_wd_ctx(ctx_config, start);
	free(ctx_config->ctxs);
err_free_ctx_config:
	free(ctx_config);
	config->ctx_config = NULL;
	return ret;
}

static void wd_free_ctx(struct wd_env_config *config)
{
	struct wd_ctx_config *ctx_config;

	if (!config->ctx_config)
		return;

	ctx_config = config->ctx_config;
	wd_put_wd_ctx(ctx_config, ctx_config->ctx_num);
	free(ctx_config->ctxs);
	free(ctx_config);
	config->ctx_config = NULL;
}

static int wd_sched_fill_table(struct wd_env_config_per_numa *config_numa,
			       struct wd_sched *sched, __u8 mode, int type_num)
{
	struct wd_ctx_range **ctx_table;
	struct sched_params param;
	int i, ret, ctx_num;

	if (mode)
		ctx_num = config_numa->async_ctx_num;
	else
		ctx_num = config_numa->sync_ctx_num;

	ctx_table = config_numa->ctx_table;
	param.numa_id = config_numa->node;
	param.mode = mode;
	for (i = 0; i < type_num && ctx_num; i++) {
		if (!ctx_table[mode][i].size)
			continue;

		param.type = i;
		param.begin = ctx_table[mode][i].begin;
		param.end = ctx_table[mode][i].end;
		param.ctx_prop = UADK_ALG_HW;
		ret = wd_sched_rr_instance(sched, &param);
		if (ret)
			return ret;
	}

	return 0;
}

static void wd_uninit_sched_config(struct wd_env_config *config)
{
	if (!config->sched || !config->internal_sched)
		return;

	wd_sched_rr_release(config->sched);
	config->sched = NULL;
}

static int wd_init_sched_config(struct wd_env_config *config,
				void *alg_poll_ctx)
{
	struct wd_env_config_per_numa *config_numa;
	int i, j, ret, max_node, type_num;

	type_num = config->op_type_num;
	max_node = numa_max_node() + 1;
	if (max_node <= 0)
		return -WD_EINVAL;

	config->internal_sched = false;
	if (!config->sched) {
		WD_ERR("no sched is specified, alloc a default sched!\n");
		config->sched = wd_sched_rr_alloc(SCHED_POLICY_RR, type_num,
						  max_node, alg_poll_ctx);
		if (!config->sched)
			return -WD_ENOMEM;

		config->internal_sched = true;
	}

	config->sched->name = "SCHED_RR";

	FOREACH_NUMA(i, config, config_numa) {
		for (j = 0; j < CTX_MODE_MAX; j++) {
			ret = wd_sched_fill_table(config_numa,
						  config->sched, j,
						  type_num);
			if (ret)
				goto err_release_sched;
		}
	}

	return 0;

err_release_sched:
	wd_uninit_sched_config(config);

	return ret;
}

static int wd_init_resource(struct wd_env_config *config,
			    const struct wd_alg_ops *ops)
{
	int ret;

	ret = wd_alloc_ctx(config);
	if (ret)
		return ret;

	ret = wd_init_sched_config(config, ops->alg_poll_ctx);
	if (ret)
		goto err_uninit_ctx;

	ret = ops->alg_init(config->ctx_config, config->sched);
	if (ret)
		goto err_uninit_sched;

	return 0;

err_uninit_sched:
	wd_uninit_sched_config(config);
err_uninit_ctx:
	wd_free_ctx(config);
	return ret;
}

static void wd_uninit_resource(struct wd_env_config *config,
			       const struct wd_alg_ops *ops)
{
	ops->alg_uninit();
	wd_uninit_sched_config(config);
	wd_free_ctx(config);
}

int wd_alg_env_init(struct wd_env_config *env_config,
		    const struct wd_config_variable *table,
		    const struct wd_alg_ops *ops,
		    __u32 table_size,
		    struct wd_ctx_attr *ctx_attr)
{
	int ret;

	ret = wd_alloc_numa(env_config, ops);
	if (ret)
		return ret;

	ret = wd_init_env_config(env_config, ctx_attr, ops, table, table_size);
	if (ret)
		goto free_numa;

	ret = wd_init_resource(env_config, ops);
	if (ret)
		goto uninit_env_config;

	return 0;

uninit_env_config:
	wd_uninit_env_config(env_config);
free_numa:
	wd_free_numa(env_config);
	return ret;
}

void wd_alg_env_uninit(struct wd_env_config *env_config,
		       const struct wd_alg_ops *ops)
{
	wd_uninit_resource(env_config, ops);
	wd_uninit_env_config(env_config);
	wd_free_numa(env_config);
}

int wd_alg_get_env_param(struct wd_env_config *env_config,
			 struct wd_ctx_attr attr,
			 __u32 *num, __u8 *is_enable)
{
	struct wd_env_config_per_numa *config_numa;

	if (!num || !is_enable) {
		WD_ERR("invalid: input pointer num or is_enable is NULL!\n");
		return -WD_EINVAL;
	}

	*is_enable = 0;

	config_numa = wd_get_config_numa(env_config, attr.node);
	if (!config_numa)
		return -WD_EINVAL;

	*num = (config_numa->ctx_table) ?
	       config_numa->ctx_table[attr.mode][attr.type].size : 0;

	return 0;
}

int wd_set_ctx_attr(struct wd_ctx_attr *ctx_attr,
		     __u32 node, __u32 type, __u8 mode, __u32 num)
{
	if (mode >= CTX_MODE_MAX) {
		WD_ERR("invalid: ctx mode is %u!\n", mode);
		return -WD_EINVAL;
	}

	ctx_attr->node = node;
	ctx_attr->mode = mode;
	ctx_attr->num = num;
	/* If type is CTX_TYPE_INVALID, we need update it to 0. */
	ctx_attr->type = (type == CTX_TYPE_INVALID) ? 0 : type;

	return 0;
}

int wd_check_ctx(struct wd_ctx_config_internal *config, __u8 mode, __u32 idx)
{
	struct wd_ctx_internal *ctx;

	if (unlikely(idx == QUEUE_FULL_POS))
		return -WD_EBUSY;

	if (unlikely(idx >= config->ctx_num)) {
		WD_ERR("failed to pick a proper ctx: idx %u!\n", idx);
		return -WD_EINVAL;
	}

	ctx = config->ctxs + idx;
	if (ctx->ctx_type == UADK_ALG_HW && ctx->ctx_mode != mode) {
		WD_ERR("invalid: ctx(%u) mode is %hhu!\n", idx, ctx->ctx_mode);
		return -WD_EINVAL;
	}

	return 0;
}

int wd_set_epoll_en(const char *var_name, bool *epoll_en)
{
	const char *s;
	int ret;

	s = secure_getenv(var_name);
	if (!s || !strlen(s)) {
		*epoll_en = 0;
		return 0;
	}

	ret = str_to_bool(s, epoll_en);
	if (ret) {
		WD_ERR("failed to parse %s!\n", var_name);
		return ret;
	}

	if (*epoll_en)
		WD_ERR("epoll wait is enabled!\n");

	return 0;
}

int wd_handle_msg_sync(struct wd_msg_handle *msg_handle, handle_t ctx,
		       void *msg, __u64 *balance, bool epoll_en)
{
	__u64 timeout = WD_RECV_MAX_CNT_NOSLEEP;
	__u64 rx_cnt = 0;
	int ret;

	if (balance)
		timeout = WD_RECV_MAX_CNT_SLEEP;

	ret = msg_handle->send(ctx, msg);
	if (unlikely(ret < 0)) {
		WD_ERR("failed to send msg to hw, ret = %d!\n", ret);
		return ret;
	}

	do {
		if (epoll_en) {
			ret = wd_ctx_wait(ctx, POLL_TIME);
			if (unlikely(ret < 0))
				WD_ERR("wd ctx wait timeout(%d)!\n", ret);
		}

		ret = msg_handle->recv(ctx, msg);
		if (ret != -WD_EAGAIN) {
			if (unlikely(ret < 0)) {
				WD_ERR("failed to recv msg: error = %d!\n", ret);
				return ret;
			}
			break;
		}

		rx_cnt++;
		if (unlikely(rx_cnt >= timeout)) {
			WD_ERR("failed to recv msg: timeout!\n");
			return -WD_ETIMEDOUT;
		}

		if (balance && *balance > WD_BALANCE_THRHD)
			usleep(1);
	} while (1);

	if (balance)
		*balance = rx_cnt;

	return ret;
}

int wd_init_param_check(struct wd_ctx_config *config, struct wd_sched *sched)
{
	if (!config || !config->ctxs || !config->ctxs[0].ctx) {
		WD_ERR("invalid: wd_ctx_config is NULL!\n");
		return -WD_EINVAL;
	}

	if (!sched) {
		WD_ERR("invalid: wd_sched is NULL!\n");
		return -WD_EINVAL;
	}

	return 0;
}

int wd_alg_try_init(enum wd_status *status)
{
	enum wd_status expected;
	__u32 count = 0;
	bool ret;

	/*
	 * Here is aimed to protect the security of the initialization interface
	 * in the multi-thread scenario. Only one thread can get the WD_INITING
	 * status to initialize algorithm. Other thread will wait for the result.
	 * And the algorithm initialization interfaces is a liner process.
	 * So the initing thread will return a result to notify other thread go on.
	 */
	do {
		expected = WD_UNINIT;
		ret = __atomic_compare_exchange_n(status, &expected, WD_INITING, true,
						  __ATOMIC_RELAXED, __ATOMIC_RELAXED);
		if (expected == WD_INIT) {
			WD_ERR("The algorithm has been initialized!\n");
			return -WD_EEXIST;
		}
		usleep(WD_INIT_SLEEP_UTIME);

		if (US2S(WD_INIT_SLEEP_UTIME * ++count) >= WD_INIT_RETRY_TIMEOUT) {
			WD_ERR("The algorithm initialize wait timeout!\n");
			return -WD_ETIMEDOUT;
		}
	} while (!ret);

	return 0;
}

static int wd_alg_init_fallback(struct wd_alg_driver *fb_driver)
{
	if (!fb_driver->init) {
		WD_ERR("soft acc driver have no init interface.\n");
		return -WD_EINVAL;
	}

	WD_ERR("debug: call function: %s!\n", __func__);
	fb_driver->init(NULL, NULL);

	return 0;
}

static void wd_alg_uninit_fallback(struct wd_alg_driver *fb_driver)
{
	if (!fb_driver->exit) {
		WD_ERR("soft acc driver have no exit interface.\n");
		return;
	}

	fb_driver->exit(NULL);
}

static int wd_ctx_init_driver(struct wd_ctx_config_internal *config,
	struct wd_ctx_internal *ctx_config)
{
	struct wd_alg_driver *driver = ctx_config->drv;
	void *priv = ctx_config->drv_priv;
	int ret;

	WD_ERR("debug: call function: %s!\n", __func__);
	if (!driver)
		return 0;

	WD_INFO("driver init: drv name: %s, alg_name: %s \n",
			driver->drv_name, driver->alg_name);
	/* Prevent repeated initialization */
	if (driver->init_state)
		return 0;

	if (!driver->priv_size) {
		WD_ERR("invalid: driver priv ctx size is zero!\n");
		return -WD_EINVAL;
	}

	/* Init ctx related resources in specific driver */
	priv = calloc(1, driver->priv_size);
	if (!priv)
		return -WD_ENOMEM;

	if (!driver->init) {
		driver->fallback = 0;
		WD_ERR("driver have no init interface.\n");
		ret = -WD_EINVAL;
		goto err_alloc;
	}
	driver->init_state = 1;

	ret = driver->init(config, priv);
	if (ret < 0) {
		WD_ERR("driver init failed.\n");
		goto err_alloc;
	}
	driver->drv_data = priv;

	if (driver->fallback) {
		ret = wd_alg_init_fallback((struct wd_alg_driver *)driver->fallback);
		if (ret) {
			driver->fallback = 0;
			WD_ERR("soft alg driver init failed.\n");
		}
	}

	return 0;

err_alloc:
	free(priv);
	ctx_config->drv_priv = NULL;
	return ret;
}

static void wd_ctx_uninit_driver(struct wd_ctx_config_internal *config,
	struct wd_ctx_internal *ctx_config)
{
	struct wd_alg_driver *driver = ctx_config->drv;
	void *priv = ctx_config->drv_priv;

	WD_ERR("debug: call function: %s!\n", __func__);
	if (!driver || !priv)
		return;

	/* Prevent repeated uninitialization */
	if (!driver->init_state)
		return;

	driver->exit(priv);
	driver->init_state = 0;
	/* Ctx config just need clear once */
	wd_clear_ctx_config(config);

	if (driver->fallback)
		wd_alg_uninit_fallback((struct wd_alg_driver *)driver->fallback);

	if (priv) {
		free(priv);
		driver->drv_data = NULL;
		ctx_config->drv_priv = NULL;
	}
}

int wd_alg_init_driver(struct wd_ctx_config_internal *config)
{
	__u32 i, j;
	int ret;

	WD_ERR("debug: call function: %s!\n", __func__);
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

void wd_alg_uninit_driver(struct wd_ctx_config_internal *config)
{
	__u32 i;

	for (i = 0; i < config->ctx_num; i++)
		wd_ctx_uninit_driver(config, &config->ctxs[i]);

}

void wd_dlclose_drv(void *dlh_list)
{
	struct drv_lib_list *dlhead = (struct drv_lib_list *)dlh_list;
	struct drv_lib_list *dlnode;

	if (!dlhead) {
		WD_INFO("driver so file list is empty.\n");
		return;
	}

	while (dlhead) {
		dlnode = dlhead;
		dlhead = dlhead->next;
		dlclose(dlnode->dlhandle);
		free(dlnode);
	}
}

static void add_lib_to_list(struct drv_lib_list *head,
			    struct drv_lib_list *node)
{
	struct drv_lib_list *tmp = head;

	while (tmp->next)
		tmp = tmp->next;

	tmp->next = node;
}

static int wd_set_ctx_nums(struct wd_ctx_params *ctx_params, struct uacce_dev_list *list,
			   const char *section, __u32 op_type_num, int is_comp)
{
	struct wd_ctx_nums *ctxs = ctx_params->ctx_set_num;
	int ret, ctx_num, node;
	struct uacce_dev *dev;
	char *ctx_section;
	const char *type;
	__u32 i, j;

	ctx_section = index(section, ':');
	if (!ctx_section) {
		WD_ERR("invalid: ctx section got wrong format: %s!\n", section);
		return -WD_EINVAL;
	}
	ctx_section++;
	ret = parse_num_on_numa(ctx_section, &ctx_num, &node);
	if (ret)
		return ret;

	/* If the number of ctxs is set to 0, skip the configuration */
	if (!ctx_num)
		return 0;

	dev = wd_find_dev_by_numa(list, node);
	if (WD_IS_ERR(dev))
		return -WD_ENODEV;

	for (i = 0; i < CTX_MODE_MAX; i++) {
		for (j = 0; j < op_type_num; j++) {
			type = is_comp ? comp_ctx_type[i][j] : ctx_type[i][0];
			if (strncmp(section, type, strlen(type)))
				continue;

			/* If there're multiple configurations, use the maximum ctx number */
			if (!i)
				ctxs[j].sync_ctx_num = MAX(ctxs[j].sync_ctx_num, (__u32)ctx_num);
			else
				ctxs[j].async_ctx_num = MAX(ctxs[j].async_ctx_num, (__u32)ctx_num);

			/* enable a node here, all enabled nodes share the same configuration */
			numa_bitmask_setbit(ctx_params->bmp, node);
			return 0;
		}
	}

	return -WD_EINVAL;
}

static int wd_env_set_ctx_nums(const char *alg_name, const char *name, const char *var_s,
			       struct wd_ctx_params *ctx_params, __u32 op_type_num)
{
	char alg_type[CRYPTO_MAX_ALG_NAME];
	char *left, *section, *start;
	struct uacce_dev_list *list;
	int is_comp;
	int ret;

	/* COMP environment variable's format is different, mark it */
	is_comp = strncmp(name, "WD_COMP_CTX_NUM", strlen(name)) ? 0 : 1;
	if (is_comp && op_type_num > ARRAY_SIZE(comp_ctx_type))
		return -WD_EINVAL;

	start = strdup(var_s);
	if (!start)
		return -WD_ENOMEM;

	ret = wd_get_alg_type(alg_name, alg_type);
	if (ret)
		return ret;

	list = wd_get_accel_list(alg_type);
	if (!list) {
		WD_ERR("failed to get devices!\n");
		free(start);
		return -WD_ENODEV;
	}

	left = start;
	while ((section = strsep(&left, ","))) {
		ret = wd_set_ctx_nums(ctx_params, list, section, op_type_num, is_comp);
		if (ret < 0)
			break;
	}

	wd_free_list_accels(list);
	free(start);
	return ret;
}

void wd_ctx_param_uninit(struct wd_ctx_params *ctx_params)
{
	numa_free_nodemask(ctx_params->bmp);
}

int wd_ctx_param_init(struct wd_ctx_params *ctx_params,
		      struct wd_ctx_params *user_ctx_params,
		      char *alg, int task_type, enum wd_type type,
		      int max_op_type)
{
	const char *env_name = wd_env_name[type];
	const char *var_s;
	int i, ret;

	ctx_params->bmp = numa_allocate_nodemask();
	if (!ctx_params->bmp) {
		WD_ERR("fail to allocate nodemask.\n");
		return -WD_ENOMEM;
	}

	/* Support env variable for HW and INSTR task types (excludes TASK_MAX_TYPE) */
	var_s = secure_getenv(env_name);
	if (var_s && strlen(var_s) && task_type < TASK_MAX_TYPE) {
		/* environment variable has the highest priority */
		ret = wd_env_set_ctx_nums(alg, env_name, var_s,
					  ctx_params, max_op_type);
		if (ret) {
			WD_ERR("fail to init ctx nums from %s!\n", env_name);
			numa_free_nodemask(ctx_params->bmp);
			return ret;
		}
	} else {
		/* environment variable is not set, try to use user_ctx_params first */
		if (user_ctx_params) {
			copy_bitmask_to_bitmask(user_ctx_params->bmp, ctx_params->bmp);
			if (user_ctx_params->op_type_num > (__u32)max_op_type) {
				WD_ERR("fail to check user op type numbers.\n");
				numa_free_nodemask(ctx_params->bmp);
				return -WD_EINVAL;
			}
			ctx_params->cap = user_ctx_params->cap;
			ctx_params->ctx_set_num = user_ctx_params->ctx_set_num;
			ctx_params->op_type_num = user_ctx_params->op_type_num;

			return 0;
		}
	}

	/* user_ctx_params is also not set, use driver's defalut queue_num */
	numa_bitmask_setall(ctx_params->bmp);
	for (i = 0; i < max_op_type; i++) {
		ctx_params->ctx_set_num[i].sync_ctx_num = max_op_type;
		ctx_params->ctx_set_num[i].async_ctx_num = max_op_type;
		/* Set ctx_prop based on task_type so scheduler domains match
		 * the calc_type of drivers discovered for this task_type.
		 * Without this, all ctxs default to UADK_CTX_HW(0) which
		 * breaks scheduling for TASK_INSTR scenarios where drivers
		 * are CE_INSTR/SVE_INSTR/SOFT.
		 */
		if (task_type == TASK_INSTR)
			ctx_params->ctx_set_num[i].ctx_prop = UADK_CTX_CE_INS;
		else
			ctx_params->ctx_set_num[i].ctx_prop = UADK_CTX_HW;
	}
	ctx_params->op_type_num = max_op_type;

	return 0;
}

static void dladdr_empty(void)
{
}

static int line_check_valid(char *line)
{
	line[strcspn(line, "\n")] = 0;
	if (line[0] == '\0' || line[0] == '#')
		return 0;

	if (!strstr(line, ".so"))
		return 0;

	return 1;
}

static int check_uadk_config_file(const char *wd_dir, const char *lib_file)
{
	char *path_buf, *uadk_cnf_path, *line;
	int ret = -WD_EINVAL;
	FILE *fp;

	path_buf = calloc(WD_PATH_DIR_NUM, PATH_MAX);
	if (!path_buf) {
		WD_ERR("fail to alloc memery for path_buf.\n");
		return -WD_ENOMEM;
	}

	uadk_cnf_path = path_buf;
	line = path_buf + PATH_MAX;

	snprintf(uadk_cnf_path, PATH_MAX, "%s/%s/%s", wd_dir, WD_DRV_LIB_DIR,
		 WD_DRV_CONF_FILE);
	fp = fopen(uadk_cnf_path, "r");
	if (!fp) {
		ret = 0;
		goto free_buf;
	}

	while (fgets(line, PATH_MAX, fp)) {
		if (!line_check_valid(line))
			continue;

		if (strstr(line, lib_file)) {
			ret = 0;
			goto close_fp;
		}
	}

close_fp:
	fclose(fp);
free_buf:
	free(path_buf);
	return ret;
}

int wd_get_lib_file_path(const char *lib_file, char *lib_path, bool is_dir)
{
	char *path_buf, *path, *file_path;
	Dl_info file_info;
	int len, rc, i;
	int ret = 0;

	/* Get libwd.so file's system path */
	rc = dladdr(dladdr_empty, &file_info);
	if (!rc) {
		WD_ERR("fail to get lib file path.\n");
		return -WD_EINVAL;
	}

	path_buf = calloc(WD_PATH_DIR_NUM, PATH_MAX);
	if (!path_buf) {
		WD_ERR("fail to calloc path_buf.\n");
		return -WD_ENOMEM;
	}
	file_path = path_buf;
	path = path_buf + PATH_MAX;
	strncpy(file_path, file_info.dli_fname, PATH_MAX - 1);

	/* Clear the file path's tail file name */
	len = strlen(file_path) - 1;
	for (i = len; i >= 0; i--) {
		if (file_path[i] == '/') {
			memset(&file_path[i], 0, PATH_MAX - i);
			break;
		}
	}

	if (is_dir) {
		len = snprintf(lib_path, PATH_MAX, "%s/%s", file_path, WD_DRV_LIB_DIR);
	} else {
		/* Confirm whether the corresponding file exists in uadk.cnf */
		ret = check_uadk_config_file(file_path, lib_file);
		if (ret)
			goto free_path;

		len = snprintf(lib_path, PATH_MAX, "%s/%s/%s",
			       file_path, WD_DRV_LIB_DIR, lib_file);
	}

	if (len >= PATH_MAX) {
		ret = -WD_EINVAL;
		goto free_path;
	}

	if (!realpath(lib_path, path)) {
		WD_ERR("invalid: %s: no such file or directory!\n", path);
		ret = -WD_EINVAL;
	}

free_path:
	free(path_buf);
	return ret;
}

/*
 * There are many other .so files in this file directory (/root/lib/),
 * and it is necessary to screen out valid uadk driver files
 * through this function.
 */
static int file_check_valid(const char *lib_file)
{
#define MIN_FILE_LEN 6
#define FILE_TAIL_LEN 3
	const char *dot = strrchr(lib_file, '.');
	size_t len;

	/* Check if the filename length is sufficient. */
	len = strlen(lib_file);
	if (len < MIN_FILE_LEN)
		return -EINVAL;

	/* Check if it starts with "lib". */
	if (strncmp(lib_file, "lib", FILE_TAIL_LEN) != 0)
		return -EINVAL;

	/* Check if it ends with ".so". */
	if (!dot || strcmp(dot, ".so") != 0)
		return -EINVAL;

	return 0;
}

static void create_lib_to_list(const char *lib_path, struct drv_lib_list **head)
{
	typedef int (*alg_ops)(struct wd_alg_driver *drv);
	struct drv_lib_list *node;
	alg_ops dl_func;

	node = calloc(1, sizeof(*node));
	if (!node)
		return;

	node->dlhandle = dlopen(lib_path, RTLD_NODELETE | RTLD_NOW);
	if (!node->dlhandle) {
		WD_ERR("failed to open lib file: %s, skipped\n", lib_path);
		free(node);
		return;
	}

	dl_func = dlsym(node->dlhandle, "wd_alg_driver_register");
	if (!dl_func) {
		WD_ERR("dlsym failed for %s: %s\n", lib_path, dlerror());
		dlclose(node->dlhandle);
		free(node);
		return;
	}

	if (!*head) {
		*head = node;
		return;
	}
	add_lib_to_list(*head, node);
}

static struct drv_lib_list *load_libraries_from_config(const char *config_path,
						       const char *lib_dir_path)
{
	char *lib_path, *line;
	struct drv_lib_list *head = NULL;
	FILE *config_file;
	int ret;

	lib_path = calloc(1, PATH_MAX);
	if (!lib_path) {
		WD_ERR("Failed to alloc memery for lib_path.\n");
		return head;
	}

	line = calloc(1, PATH_MAX);
	if (!line) {
		WD_ERR("Failed to alloc memery for lib_line.\n");
		goto free_path;
	}

	config_file = fopen(config_path, "r");
	if (!config_file) {
		WD_ERR("Failed to open config file: %s\n", config_path);
		goto free_line;
	}

	/* Read config file line by line */
	while (fgets(line, PATH_MAX, config_file)) {
		if (!line_check_valid(line))
			continue;

		ret = snprintf(lib_path, PATH_MAX, "%s/%s", lib_dir_path, line);
		if (ret < 0)
			break;

		create_lib_to_list(lib_path, &head);
	}

	fclose(config_file);

free_line:
	free(line);
free_path:
	free(lib_path);
	return head;
}

static struct drv_lib_list *load_all_libraries(DIR *wd_dir, const char *lib_dir_path)
{
	struct drv_lib_list *head = NULL;
	struct dirent *lib_dir;
	char *lib_path;
	int ret;

	lib_path = calloc(1, PATH_MAX);
	if (!lib_path) {
		WD_ERR("fail to alloc memery for lib_path.\n");
		return NULL;
	}

	rewinddir(wd_dir); /* Ensure we're at the start of the directory */

	while ((lib_dir = readdir(wd_dir)) != NULL) {
		if (!strncmp(lib_dir->d_name, ".", LINUX_CRTDIR_SIZE) ||
		    !strncmp(lib_dir->d_name, "..", LINUX_PRTDIR_SIZE))
			continue;

		ret = file_check_valid(lib_dir->d_name);
		if (ret)
			continue;

		ret = snprintf(lib_path, PATH_MAX, "%s/%s", lib_dir_path, lib_dir->d_name);
		if (ret < 0)
			break;

		create_lib_to_list(lib_path, &head);
	}

	free(lib_path);
	return head;
}

void *wd_dlopen_drv(const char *cust_lib_dir)
{
	char *path_buf, *lib_dir_path, *config_path, *lib_path;
	struct drv_lib_list *head = NULL;
	int ret, len;
	DIR *wd_dir;

	path_buf = calloc(WD_PATH_DIR_NUM, PATH_MAX);
	if (!path_buf) {
		WD_ERR("Failed to alloc memory for path_buf buffers.\n");
		return head;
	}

	config_path = calloc(1, PATH_MAX);
	if (!config_path) {
		WD_ERR("Failed to alloc memory for config_path buffers.\n");
		free(path_buf);
		return head;
	}

	lib_dir_path = path_buf;
	lib_path = path_buf + PATH_MAX;

	if (!cust_lib_dir) {
		ret = wd_get_lib_file_path(NULL, lib_dir_path, true);
		if (ret)
			goto free_path;
	} else {
		if (!realpath(cust_lib_dir, lib_path)) {
			WD_ERR("invalid: %s: no such file or directory!\n", lib_path);
			goto free_path;
		}

		len = snprintf(lib_dir_path, PATH_MAX, "%s", cust_lib_dir);
		if (len < 0 || len >= PATH_MAX)
			goto free_path;

		lib_dir_path[PATH_MAX - 1] = '\0';
	}

	wd_dir = opendir(lib_dir_path);
	if (!wd_dir) {
		WD_ERR("UADK driver lib dir: %s not exist!\n", lib_dir_path);
		goto free_path;
	}

	len = snprintf(config_path, PATH_MAX, "%s/%s", lib_dir_path, WD_DRV_CONF_FILE);
	if (len < 0 || len >= PATH_MAX)
		goto close_dir;

	ret = access(config_path, F_OK);
	if (!ret)
		/* Load specified libraries from config file */
		head = load_libraries_from_config(config_path, lib_dir_path);
	else
		/* Load all valid .so files */
		head = load_all_libraries(wd_dir, lib_dir_path);

close_dir:
	closedir(wd_dir);
free_path:
	free(path_buf);
	free(config_path);
	return (void *)head;
}

int wd_ctx_drv_config(char *alg_name,	struct wd_ctx_config_internal *ctx_config)
{
	return 0;
}
void wd_ctx_drv_deconfig(struct wd_ctx_config_internal *ctx_config)
{
}

/**
 * wd_ctx_unbind_drivers() - Phase 2.5 reverse: Unbind drivers from internal ctxs.
 *
 * Decrements driver refcounts and clears all drv pointers.
 *
 * @config: Internal ctx config
 */
void wd_ctx_unbind_drivers(struct wd_ctx_config_internal *config)
{
	__u32 i;

	if (!config || !config->drv_array)
		return;

	wd_alg_drv_ref_dec(config->drv_array, config->drv_count);

	for (i = 0; i < config->ctx_num; i++)
		config->ctxs[i].drv = NULL;
}

/**
 * wd_ctx_bind_drivers() - Bind drivers to internal ctxs via RR.
 *
 * This is the SINGLE WRITE POINT for ctxs[i].drv in the entire lifecycle.
 * Uses RR rule: ctxs[i].drv = drv_array[i % drv_count]
 *
 * Also:
 * - Sets up soft fallback for HW drivers (once per unique HW driver)
 * - Caches drv_array in config for session queries
 * - Increments driver refcounts (deduplicated: each unique driver +1)
 *
 * and overwrote the RR mapping.
 *
 * @config:    Internal ctx config (ctxs[] already copied by wd_init_ctx_config)
 * @drv_array: Discovered unique drivers (from Phase 1)
 * @drv_count: Number of unique drivers
 * Return: 0 on success, negative on failure
 */
int wd_ctx_bind_drivers(struct wd_ctx_config_internal *config,
			struct wd_alg_driver **drv_array, __u32 drv_count)
{
	struct wd_alg_driver *drv;
	__u32 i;

	if (!config || !drv_array || drv_count == 0) {
		WD_ERR("invalid parameters!\n");
		return -WD_EINVAL;
	}

	WD_INFO("Phase 2: drivers array have <%u> drvers.\n", drv_count);
	for (i = 0; i < config->ctx_num; i++) {
		/* In the init process, only one hisi driver will be specified. */
		if (drv_count == 1) {
			config->ctxs[i].drv = drv_array[0];
			config->ctxs[i].ctx_type = config->ctxs[0].drv->calc_type;
		} else {
			/*
			 * RR binding — the ONLY write to ctxs[i].drv in the
			 * entire lifecycle. After this, drv is read-only.
			 */
			config->ctxs[i].drv = drv_array[i % drv_count];
			config->ctxs[i].ctx_type = config->ctxs[i].drv->calc_type;
		}
		WD_INFO("driver bind: drv name: %s, alg_name: %s for ctx<%u>\n",
			config->ctxs[i].drv->drv_name, config->ctxs[i].drv->alg_name, i);

		/* HW driver needs soft fallback — set once per unique driver */
		if (config->ctxs[i].ctx_type == UADK_ALG_HW) {
			drv = config->ctxs[i].drv;
			if (!drv->fallback) {
				drv->fallback = (handle_t)wd_request_drv(
					config->alg_name, ALG_DRV_SOFT);
			}
		}
	}

	/* Cache driver array for session queries */
	config->drv_array = drv_array;
	config->drv_count = drv_count;

	/* Deduplicated refcount increment */
	wd_alg_drv_ref_inc(drv_array, drv_count);

	WD_INFO("Phase 2.5: bound %u ctxs to %u drivers via RR\n",
		config->ctx_num, drv_count);

	return 0;
}

/**
 * wd_ctx_config_uninit() - Release internal_config and driver array.
 *
 * Releases internal_config allocated by wd_ctx_config_init(),
 * including the drv_array allocated by wd_get_drv_array().
 * This is the Phase 1 reverse of wd_ctx_config_init().
 *
 * @attrs: Initialization attributes
 */
void wd_ctx_config_uninit(struct wd_init_attrs *attrs)
{
	struct wd_ctx_config_internal *internal_config;

	if (!attrs)
		return;

	internal_config = attrs->ctx_config_internal;
	if (!internal_config)
		return;

	/* Release drv_array */
	if (internal_config->drv_array) {
		wd_put_drv_array(internal_config->drv_array,
				 internal_config->drv_count);
		internal_config->drv_array = NULL;
		internal_config->drv_count = 0;
	}

	/* Release ctxs array */
	if (internal_config->ctxs) {
		free(internal_config->ctxs);
		internal_config->ctxs = NULL;
	}

	/* Release internal_config */
	free(internal_config);
	attrs->ctx_config_internal = NULL;

	WD_INFO("wd_ctx_config_uninit: complete\n");
}

/**
 * wd_ctx_config_init() - Initialize internal_config and discover drivers.
 *
 * Phase 1 of algorithm initialization:
 * 1. Calculate total context count from ctx_params
 * 2. Discover drivers via wd_get_drv_array()
 * 3. Allocate internal_config + ctxs array
 * 4. Store drv_array/drv_count in internal_config
 * 5. Return ctx_config_internal via attrs->ctx_config_internal
 *
 * @attrs: Initialization attributes (input: alg, task_type, ctx_params)
 * Return: 0 on success, negative on failure
 */
int wd_ctx_config_init(struct wd_init_attrs *attrs)
{
	char alg_type[CRYPTO_MAX_ALG_NAME] = {0};
	struct wd_ctx_config_internal *internal_config;
	struct wd_alg_driver **drv_array = NULL;
	__u32 drv_count = 0;
	__u32 total_ctx_num;
	__u32 sync_num = 0, async_num = 0;
	__u32 i;
	int ret;

	if (!attrs || !attrs->alg[0] || !attrs->ctx_params)
		return -WD_EINVAL;

	/* Step 1: Calculate total context count */
	for (i = 0; i < attrs->ctx_params->op_type_num; i++) {
		sync_num += attrs->ctx_params->ctx_set_num[i].sync_ctx_num;
		async_num += attrs->ctx_params->ctx_set_num[i].async_ctx_num;
	}
	total_ctx_num = sync_num + async_num;
	if (total_ctx_num == 0) {
		WD_ERR("invalid: total_ctx_num is zero!\n");
		return -WD_EINVAL;
	}

	/* Step 2: Normalize alg to alg_type and discover drivers */
	wd_get_alg_type(attrs->alg, alg_type);
	if (!alg_type[0]) {
		WD_ERR("unknown alg type for %s\n", attrs->alg);
		return -WD_EINVAL;
	}

	ret = wd_get_drv_array(alg_type, attrs->task_type, NULL,
			       &drv_array, &drv_count);
	if (ret || !drv_array || drv_count == 0) {
		WD_ERR("failed to get %s's driver array\n", attrs->alg);
		return -WD_EINVAL;
	}

	/* Step 3: Allocate internal_config */
	internal_config = calloc(1, sizeof(*internal_config));
	if (!internal_config) {
		WD_ERR("failed to allocate internal_config!\n");
		wd_put_drv_array(drv_array, drv_count);
		return -WD_ENOMEM;
	}

	internal_config->ctxs = calloc(total_ctx_num, sizeof(struct wd_ctx_internal));
	if (!internal_config->ctxs) {
		WD_ERR("failed to allocate internal ctxs array!\n");
		free(internal_config);
		wd_put_drv_array(drv_array, drv_count);
		return -WD_ENOMEM;
	}

	/* Step 4: Store drv_array/drv_count in internal_config */
	internal_config->drv_array = drv_array;
	internal_config->drv_count = drv_count;
	internal_config->ctx_num = total_ctx_num;

	/* Step 5: Return internal_config via attrs */
	attrs->ctx_config_internal = internal_config;

	WD_INFO("wd_ctx_config_init: %u drivers, %u ctxs (sync=%u, async=%u)\n",
		drv_count, total_ctx_num, sync_num, async_num);

	return 0;
}

static int wd_alloc_ctxs_by_mode(struct wd_ctx_config_internal *internal_config,
				   struct wd_init_attrs *attrs,
				   __u8 ctx_mode,
				   __u32 *ctx_idx_out)
{
	struct wd_ctx_params *ctx_params = attrs->ctx_params;
	struct wd_alg_driver **drv_array = internal_config->drv_array;
	__u32 drv_count = internal_config->drv_count;
	const char *alg = attrs->alg;
	struct wd_drv_ctx_params dparams;
	struct wd_alg_driver *drv;
	handle_t ctx;
	__u32 i, op_type;
	__u32 allocated = 0;
	int ret;

	for (op_type = 0; op_type < ctx_params->op_type_num; op_type++) {
		__u32 num = (ctx_mode == CTX_MODE_SYNC) ?
			    ctx_params->ctx_set_num[op_type].sync_ctx_num :
			    ctx_params->ctx_set_num[op_type].async_ctx_num;

		for (i = 0; i < num; i++) {
			__u32 drv_idx = i % drv_count;
			__u32 ctx_idx = *ctx_idx_out;

			drv = drv_array[drv_idx];
			if (!drv || !drv->alloc_ctx) {
				WD_ERR("Warning: driver-%s alloc_ctx is NULL!\n",
				       drv ? drv->drv_name : "null");
				continue;
			}

			memset(&dparams, 0, sizeof(dparams));
			dparams.ctx_mode = ctx_mode;
			dparams.op_type = op_type;
			dparams.numa_id = 0;
			dparams.idx = ctx_idx;
			dparams.bmp = ctx_params->bmp;
			dparams.epoll_en = false;

			ret = drv->alloc_ctx(alg, &dparams, &ctx);
			if (!ctx || ret) {
				WD_ERR("driver %u (%s) alloc_ctx failed for ctx %u\n",
				       drv_idx, drv->drv_name, ctx_idx);
				return -WD_ENOMEM;
			}

			internal_config->ctxs[ctx_idx].ctx = ctx;
			internal_config->ctxs[ctx_idx].op_type = op_type;
			internal_config->ctxs[ctx_idx].ctx_mode = ctx_mode;
			internal_config->ctxs[ctx_idx].ctx_type = drv->calc_type;
			internal_config->ctxs[ctx_idx].drv = drv;

			(*ctx_idx_out)++;
			allocated++;
		}
	}

	return allocated;
}

static int wd_alloc_ctxs_batch(struct wd_ctx_config_internal *internal_config,
				struct wd_init_attrs *attrs)
{
	struct wd_ctx_params *ctx_params = attrs->ctx_params;
	struct wd_alg_driver **drv_array = internal_config->drv_array;
	__u32 drv_count = internal_config->drv_count;
	const char *alg = attrs->alg;
	__u32 sync_num = 0, async_num = 0;
	__u32 ctx_idx = 0;
	__u32 sync_allocated, async_allocated;
	int ret;

	/* Calculate sync/async counts */
	for (int i = 0; i < ctx_params->op_type_num; i++) {
		sync_num += ctx_params->ctx_set_num[i].sync_ctx_num;
		async_num += ctx_params->ctx_set_num[i].async_ctx_num;
	}

	/*
	 * Allocation strategy:
	 * 1. First allocate all sync ctxs (ctx 0 ~ sync_num-1)
	 * 2. Then allocate all async ctxs (ctx sync_num ~ total-1)
	 * This ensures sync ctxs and async ctxs are each contiguous.
	 */

	ret = wd_alloc_ctxs_by_mode(internal_config, attrs, CTX_MODE_SYNC, &ctx_idx);
	if (ret < 0)
		return ret;
	sync_allocated = ret;

	ret = wd_alloc_ctxs_by_mode(internal_config, attrs, CTX_MODE_ASYNC, &ctx_idx);
	if (ret < 0)
		return ret;
	async_allocated = ret;

	WD_INFO("Allocated %u sync + %u async = %u ctxs\n",
		sync_allocated, async_allocated, ctx_idx);

	return 0;
}

static void wd_free_ctxs_batch(struct wd_ctx_config_internal *internal_config,
				struct wd_init_attrs *attrs)
{
	struct wd_alg_driver *drv;
	__u32 i;

	if (!internal_config || !internal_config->ctxs)
		return;

	for (i = 0; i < internal_config->ctx_num; i++) {
		if (!internal_config->ctxs[i].ctx)
			continue;

		/* Use stored drv pointer from allocation */
		drv = internal_config->ctxs[i].drv;
		if (drv && drv->free_ctx) {
			drv->free_ctx(internal_config->ctxs[i].ctx);
			internal_config->ctxs[i].ctx = 0;
		}
	}
}

static int wd_alg_sched_instance(struct wd_sched *sched,
                                 struct wd_ctx_config_internal *internal_config,
                                 struct wd_ctx_params *ctx_params)
{
	struct sched_params sparams;
	__u32 i = 0;
	int ret;

	if (!sched || !internal_config || !ctx_params) {
		WD_ERR("invalid: sched, internal_config, or ctx_params is NULL!\n");
		return -WD_EINVAL;
	}

	if (!internal_config->ctxs) {
		WD_ERR("invalid: internal_config->ctxs is NULL!\n");
		return -WD_EINVAL;
	}

	WD_INFO("Registering %u internal contexts into scheduler\n",
		internal_config->ctx_num);

	/* Walk the internal ctx array and register contiguous segments
	 * per (ctx_mode, op_type, ctx_type) tuple.
	 */
	while (i < internal_config->ctx_num) {
		__u8 mode = internal_config->ctxs[i].ctx_mode;
		__u32 type = internal_config->ctxs[i].op_type;
		__u8 ctx_type = internal_config->ctxs[i].ctx_type;
		__u32 seg_begin = i;

		/* Find end of this contiguous run */
		while (i < internal_config->ctx_num &&
		       internal_config->ctxs[i].ctx_mode == mode &&
		       internal_config->ctxs[i].op_type == type &&
		       internal_config->ctxs[i].ctx_type == ctx_type)
			i++;

		memset(&sparams, 0, sizeof(sparams));
		sparams.numa_id = 0;
		sparams.type = type;
		sparams.mode = mode;
		sparams.begin = seg_begin;
		sparams.end = i - 1;
		sparams.ctx_prop = ctx_type;
		sparams.dev_id = 0;

		ret = wd_sched_rr_instance(sched, &sparams);
		if (ret) {
			WD_ERR("failed to register ctx range [%u, %u] "
			       "(op_type=%u, mode=%u, ctx_prop=%d)\n",
			       seg_begin, i - 1, type, mode, ctx_type);
			return ret;
		}

		WD_INFO("Registered range [%u, %u]: op_type=%u, mode=%u, "
			"ctx_prop=%d\n", seg_begin, i - 1,
			type, mode, ctx_type);
	}

	return 0;
}

/**
 * wd_alg_ctx_uninit() - Release ctxs, scheduler, ctx_config.
 *
 * Phase 2 reverse (Phase 1 reverse is wd_ctx_config_uninit):
 * 1. Free ctx_config + wd_ctx[] (AUXILIARY)
 * 2. Free all contexts via wd_free_ctxs_batch()
 * 3. Free scheduler
 *
 * NOTE: internal_config is released by wd_ctx_config_uninit() in Phase 1 reverse.
 *
 * @attrs: Initialization attributes
 */
void wd_alg_ctx_uninit(struct wd_init_attrs *attrs)
{
	struct wd_ctx_config *ctx_config;
	struct wd_ctx_config_internal *internal_config;

	if (!attrs)
		return;

	ctx_config = attrs->ctx_config;
	internal_config = attrs->ctx_config_internal;

	WD_INFO("wd_alg_ctx_uninit: starting\n");

	/* Step 1: Free ctx_config + wd_ctx[] (AUXILIARY) */
	if (ctx_config) {
		free(ctx_config->ctxs);
		free(ctx_config);
		attrs->ctx_config = NULL;
	}

	/* Step 2: Free all contexts via wd_free_ctxs_batch() */
	if (internal_config && internal_config->drv_array && internal_config->drv_count > 0) {
		wd_free_ctxs_batch(internal_config, attrs);
	}

	/* Step 3: Free internal_config ctxs array (but NOT internal_config itself) */
	if (internal_config) {
		free(internal_config->ctxs);
		internal_config->ctxs = NULL;
		/* Note: internal_config is released by wd_ctx_config_uninit() */
	}

	/* Step 4: Release scheduler */
	if (attrs->sched) {
		wd_sched_rr_release(attrs->sched);
		attrs->sched = NULL;
	}

	WD_INFO("wd_alg_ctx_uninit: complete\n");
}

/**
 * wd_alg_ctx_init() - Allocate ctxs, scheduler, and do internal copy.
 *
 * Phase 2: Uses drivers from internal_config (allocated in Phase 1).
 * Allocates ctxs via RR: ctx[i] → drv_array[i % drv_count]->alloc_ctx()
 * Then allocates scheduler, registers ctx ranges, and calls alg_init
 * which performs wd_init_ctx_config() (wd_ctx[] → wd_ctx_internal[] copy).
 *
 * On return:
 *   - attrs->ctx_config: user-visible ctx array (populated)
 *   - attrs->sched: scheduler (allocated and populated)
 *   - attrs->ctx_config_internal: already set by wd_ctx_config_init()
 *
 * NOTE: ctxs[i].drv is still NULL after this function — that's set in Phase 2.5.
 *
 * @attrs: Initialization attributes
 * Return: 0 on success, negative on failure
 */
int wd_alg_ctx_init(struct wd_init_attrs *attrs)
{
	struct wd_ctx_config *ctx_config;
	struct wd_ctx_config_internal *internal_config;
	struct wd_ctx_params *ctx_params;
	__u32 sync_num = 0, async_num = 0;
	__u32 total_ctx_num = 0;
	__u32 i;
	__u16 region_num;
	int ret;

	if (!attrs || !attrs->ctx_params) {
		WD_ERR("invalid: attrs or ctx_params is NULL!\n");
		return -WD_EINVAL;
	}

	/* Retrieve internal_config from Phase 1 (wd_ctx_config_init) */
	internal_config = attrs->ctx_config_internal;
	if (!internal_config) {
		WD_ERR("internal_config not initialized! Call wd_ctx_config_init first.\n");
		return -WD_EINVAL;
	}

	/* Use drv_array from internal_config */
	if (!internal_config->drv_array || internal_config->drv_count == 0) {
		WD_ERR("invalid: drv_array is NULL/empty in internal_config!\n");
		return -WD_EINVAL;
	}

	ctx_params = attrs->ctx_params;

	/* Calculate total sync/async context counts */
	for (i = 0; i < ctx_params->op_type_num; i++) {
		sync_num += ctx_params->ctx_set_num[i].sync_ctx_num;
		async_num += ctx_params->ctx_set_num[i].async_ctx_num;
	}
	total_ctx_num = sync_num + async_num;

	if (total_ctx_num == 0) {
		WD_ERR("invalid: total_ctx_num is zero!\n");
		return -WD_EINVAL;
	}

	WD_INFO("Phase 2: %u drivers, %u ctxs (sync=%u, async=%u)\n",
		internal_config->drv_count, total_ctx_num, sync_num, async_num);

	/* Step 1: Allocate all contexts via wd_alloc_ctxs_batch() */
	ret = wd_alloc_ctxs_batch(internal_config, attrs);
	if (ret) {
		WD_ERR("wd_alloc_ctxs_batch failed!\n");
		return ret;
	}

	/* Step 2: Allocate scheduler */
	if (attrs->sched_type == SCHED_POLICY_DEV)
		region_num = DEVICE_REGION_MAX;
	else
		region_num = numa_max_node() + 1;

	attrs->sched = wd_sched_rr_alloc(attrs->sched_type,
					  ctx_params->op_type_num,
					  region_num,
					  attrs->alg_poll_ctx);
	if (!attrs->sched) {
		WD_ERR("failed to allocate scheduler!\n");
		ret = -WD_ENOMEM;
		goto cleanup_alloc;
	}

	/* Step 3: Register contexts to scheduler using internal_config */
	ret = wd_alg_sched_instance(attrs->sched, internal_config, ctx_params);
	if (ret) {
		WD_ERR("failed to register contexts to scheduler!\n");
		goto cleanup_sched;
	}

	/* Step 4: Allocate ctx_config + wd_ctx[] (AUXILIARY) */
	ctx_config = calloc(1, sizeof(*ctx_config));
	if (!ctx_config) {
		WD_ERR("failed to allocate ctx_config!\n");
		ret = -WD_ENOMEM;
		goto cleanup_sched;
	}

	ctx_config->ctxs = calloc(total_ctx_num, sizeof(struct wd_ctx));
	if (!ctx_config->ctxs) {
		WD_ERR("failed to allocate ctxs array!\n");
		free(ctx_config);
		ret = -WD_ENOMEM;
		goto cleanup_sched;
	}
	ctx_config->ctx_num = total_ctx_num;

	/* Step 5: Copy internal data to ctx_config (for alg_init compatibility) */
	for (i = 0; i < total_ctx_num; i++) {
		ctx_config->ctxs[i].ctx = internal_config->ctxs[i].ctx;
		ctx_config->ctxs[i].op_type = internal_config->ctxs[i].op_type;
		ctx_config->ctxs[i].ctx_mode = internal_config->ctxs[i].ctx_mode;
	}

	attrs->ctx_config = ctx_config;
	ctx_config->cap = ctx_params->cap;

	/* Step 6: Call algorithm-specific init */
	ret = attrs->alg_init(ctx_config, attrs->sched, attrs);
	if (ret) {
		WD_ERR("failed to initialize algorithm!\n");
		goto cleanup_ctx_config;
	}

	WD_INFO("Phase 2 complete: %u ctxs from %u drivers\n",
		total_ctx_num, internal_config->drv_count);

	return 0;

	/* Error cleanup (LIFO) */
cleanup_ctx_config:
	free(ctx_config->ctxs);
	free(ctx_config);
	attrs->ctx_config = NULL;
cleanup_sched:
	wd_sched_rr_release(attrs->sched);
	attrs->sched = NULL;
cleanup_alloc:
	wd_free_ctxs_batch(internal_config, attrs);
	return ret;
}

/**
 * wd_alg_attrs_uninit() - Algorithm attribute cleanup.
 *
 * Releases all resources in strict reverse order of init:
 *   Phase 2 reverse:   wd_alg_ctx_uninit()   - ctx_config, scheduler, contexts
 *   Phase 1 reverse:   wd_ctx_config_uninit() - internal_config, drv_array
 *
 * @attrs: Initialization attributes
 */
void wd_alg_attrs_uninit(struct wd_init_attrs *attrs)
{
	if (!attrs)
		return;

	WD_INFO("Algorithm cleanup started: alg=%s\n", attrs->alg);

	/* Phase 2 reverse: release ctxs, scheduler, ctx_config */
	wd_alg_ctx_uninit(attrs);

	/* Phase 1 reverse: release internal_config and drv_array */
	wd_ctx_config_uninit(attrs);

	WD_INFO("Algorithm cleanup complete\n");
}

/**
 * wd_alg_attrs_sched_check() - Validate task_type + sched_type combination.
 *
 * After Phase 1 driver discovery, check that the requested scheduling
 * policy is compatible with the discovered drivers and task type.
 *
 * Invalid combinations:
 *   TASK_HW + SCHED_POLICY_INSTR   - instr poll only polls ctx[0],
 *                                     losing HW async completions.
 *   TASK_INSTR + SCHED_POLICY_DEV  - CE/SVE/SOFT drivers have no
 *                                     dev_id for device-level domains.
 *   SCHED_POLICY_NONE + >1 driver  - NONE always picks ctx[0];
 *                                     different sessions may route to
 *                                     a driver that doesn't support
 *                                     their algorithm.
 *
 * @attrs: Initialization attributes (after Phase 1 drv_count populated)
 * Return: 0 on success, -WD_EINVAL on invalid combination
 */
static int wd_alg_attrs_sched_check(const struct wd_init_attrs *attrs)
{
	__u32 drv_count = attrs->ctx_config_internal->drv_count;

	if (attrs->task_type == TASK_HW &&
	    attrs->sched_type == SCHED_POLICY_INSTR) {
		WD_ERR("invalid: HW tasks must not use INSTR scheduler"
		       " (misses async poll on non-first ctxs)\n");
		return -WD_EINVAL;
	}

	if (attrs->task_type == TASK_INSTR &&
	    attrs->sched_type == SCHED_POLICY_DEV) {
		WD_ERR("invalid: INSTR tasks must not use DEV scheduler"
		       " (instr drivers have no dev_id)\n");
		return -WD_EINVAL;
	}

	if (attrs->sched_type == SCHED_POLICY_NONE &&
	    drv_count > 1) {
		WD_ERR("invalid: NONE scheduler requires single"
		       " driver, but %u drivers discovered\n",
		       drv_count);
		return -WD_EINVAL;
	}

	return 0;
}

/**
 * wd_alg_attrs_init() - Algorithm attribute initialization (V2 path).
 *
 * Orchestrates the 2-phase initialization pipeline:
 *   Phase 1:   wd_ctx_config_init()     — allocate internal_config + discover drivers
 *   Phase 2:   wd_alg_ctx_init()        — allocate ctxs + scheduler + internal copy
 *
 * After this, Phase 2.5 (driver binding) is done by the caller via wd_ctx_bind_drivers(),
 * and Phase 3 (driver init) via wd_alg_init_driver().
 *
 * @attrs: Initialization attributes (input/output)
 * Return: 0 on success, negative on failure
 */
int wd_alg_attrs_init(struct wd_init_attrs *attrs)
{
	int ret;

	if (!attrs || !attrs->ctx_params) {
		WD_ERR("invalid: attrs or ctx_params is NULL!\n");
		return -WD_EINVAL;
	}

	WD_INFO("Algorithm initialization started: alg=%s, task_type=%u\n",
		attrs->alg, attrs->task_type);

	/* Phase 1: Allocate internal_config + discover drivers */
	ret = wd_ctx_config_init(attrs);
	if (ret) {
		WD_ERR("Phase 1: wd_ctx_config_init failed!\n");
		return ret;
	}
	WD_INFO("Phase 1: discovered %u unique drivers",
		attrs->ctx_config_internal->drv_count);

	ret = wd_alg_attrs_sched_check(attrs);
	if (ret)
		goto out_config_uninit;

	/* Phase 2: ctx allocation + scheduler */
	ret = wd_alg_ctx_init(attrs);
	if (ret) {
		WD_ERR("Phase 2: ctx init failed!");
		goto out_config_uninit;
	}

	WD_INFO("Algorithm initialization complete: %u contexts from %u drivers",
		attrs->ctx_config->ctx_num, attrs->ctx_config_internal->drv_count);

	return 0;

out_config_uninit:
	wd_ctx_config_uninit(attrs);
	return ret;
}
