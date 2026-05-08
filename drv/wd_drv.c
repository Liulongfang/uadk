// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2020-2026 Huawei Technologies Co.,Ltd. All rights reserved.
 */

#include <stdlib.h>

#include "wd_internal.h"
#include "wd_alg.h"
#include "wd_util.h"
#include "wd_drv.h"

int wd_soft_alloc_ctx(char *alg_name, void *params, handle_t *ctx)
{
	struct wd_drv_ctx_params *ctx_params = (struct wd_drv_ctx_params *)params;
	struct wd_soft_ctx *sfctx;

	if (!params || !ctx) {
		WD_ERR("invalid: params, or ctx is NULL!\n");
		return -WD_EINVAL;
	}

	/* Allocate ONE software context structure */
	sfctx = calloc(1, sizeof(struct wd_soft_ctx));
	if (!sfctx) {
		WD_ERR("failed to alloc ctx!\n");
		return -WD_ENOMEM;
	}

	/* Initialize as software context */
	sfctx->fd = -1;
	pthread_spin_init(&sfctx->slock, PTHREAD_PROCESS_SHARED);
	pthread_spin_init(&sfctx->rlock, PTHREAD_PROCESS_SHARED);

	/* Return context handle */
	*ctx = (handle_t)sfctx;

	WD_INFO("SW context allocated: alg=%s, type=%d, mode=%d\n",
	        alg_name, ctx_params->op_type, ctx_params->ctx_mode);

	return 0;
}

void wd_soft_free_ctx(handle_t ctx)
{
	struct wd_soft_ctx *sfctx = (struct wd_soft_ctx *)ctx;

	if (!sfctx) {
		WD_ERR("invalid: ctx is NULL!\n");
		return;
	}

	/* Simply free the allocated wd_ctx_h structure */
	pthread_spin_destroy(&sfctx->slock);
	pthread_spin_destroy(&sfctx->rlock);
	free(sfctx);

	WD_INFO("SW context released\n");
}

struct uacce_dev_list *wd_get_usable_list(struct uacce_dev_list *list, struct bitmask *bmp)
{
	struct uacce_dev_list *p, *node, *result = NULL;
	struct uacce_dev *dev;
	int numa_id, ret;

	if (!bmp) {
		WD_ERR("invalid: bmp is NULL!\n");
		return WD_ERR_PTR(-WD_EINVAL);
	}

	p = list;
	while (p) {
		dev = p->dev;
		numa_id = dev->numa_id;
		ret = numa_bitmask_isbitset(bmp, numa_id);
		if (!ret) {
			p = p->next;
			continue;
		}

		node = calloc(1, sizeof(*node));
		if (!node) {
			result = WD_ERR_PTR(-WD_ENOMEM);
			goto out_free_list;
		}

		node->dev = wd_clone_dev(dev);
		if (!node->dev) {
			result = WD_ERR_PTR(-WD_ENOMEM);
			goto out_free_node;
		}

		if (!result)
			result = node;
		else
			wd_add_dev_to_list(result, node);

		p = p->next;
	}

	return result ? result : WD_ERR_PTR(-WD_ENODEV);

out_free_node:
	free(node);
out_free_list:
	wd_free_list_accels(result);
	return result;
}

/**
 * wd_hw_alloc_ctx() - HW driver's alloc_ctx callback.
 *
 * Allocates ONE hardware context from UACCE device.
 * This is a driver callback, called by framework for each context.
 *
 * @alg: The alg name
 * @params: Minimal allocation parameters (ctx_mode, op_type, bmp)
 * @result: (output) Allocated context information
 *
 * Return: 0 on success, negative on failure
 */
int wd_hw_alloc_ctx(char *alg_name, void *params, handle_t *ctx)
{
	struct wd_drv_ctx_params *ctx_params = (struct wd_drv_ctx_params *)params;
	struct uacce_dev_list *dev_list, *used_list = NULL;
	struct bitmask *used_bmp = ctx_params->bmp;
	char alg_type[CRYPTO_MAX_ALG_NAME];
	struct uacce_dev *dev = NULL;
	struct uacce_dev_list *curr;
	struct wd_ctx_h *ctx_h;
	int ret = -WD_EINVAL;

	if (!params || !ctx) {
		WD_ERR("invalid parameters!\n");
		return -WD_EINVAL;
	}

	/* Get algorithm type and device list */
	wd_get_alg_type(alg_name, alg_type);
	dev_list = wd_get_accel_list(alg_type);
	if (!dev_list) {
		WD_ERR("failed to get device list for alg %s\n", alg_name);
		return -WD_ENODEV;
	}

	/* Get usable device list based on NUMA mask */
	used_list = wd_get_usable_list(dev_list, used_bmp);
	if (!used_list) {
		WD_ERR("failed to get usable device list\n");
		ret = -WD_ENODEV;
		goto out;
	}

	/*
	 * After allocating all queues on the current device, proceed to
	 * request queues from the next device to ensure NUMA affinity handling. 
	 *
	 * Try each device in the usable list until success
	 */
	curr = used_list;
	while (curr) {
		dev = curr->dev;
		if (WD_IS_ERR(dev) || !dev) {
			WD_ERR("invalid device in list, skip\n");
			curr = curr->next;
			continue;
		}

		/* Request hardware context from current device */
		ctx_h = wd_request_ctx(dev);
		if (ctx_h) {
			/* Success: return context handle */
			ctx_h->priv = NULL;
			*ctx = (handle_t)ctx_h;
			ret = 0;
			WD_INFO("successful to alloc ctx from device %s, ctx: %p\n", 
				 dev->dev_root, ctx_h);
			goto out;
		}

		WD_DEBUG("failed to request ctx from device %s, try next\n", 
		         dev->dev_root);
		curr = curr->next;
	}

	/* All devices failed */
	WD_ERR("failed to request ctx from all available devices for driver %s\n", 
	      alg_name);
	ret = -WD_EBUSY;

out:
	if (dev_list)
		wd_free_list_accels(dev_list);

	return ret;
}

/**
 * wd_hw_free_ctx() - HW driver's free_ctx callback.
 *
 * Releases ONE hardware context back to UACCE device.
 *
 * @ctx: The context handle to release
 */
void wd_hw_free_ctx(handle_t ctx)
{
	struct wd_ctx_h *ctx_h = (struct wd_ctx_h *)ctx;

	if (!ctx_h) {
		WD_ERR("invalid: ctx is NULL!\n");
		return;
	}

	/* Release hardware context back to device */
	wd_release_ctx(ctx);

	WD_INFO("HW context released\n");
}

int wd_get_sqe_from_queue(struct wd_soft_ctx *sctx, __u32 tag_id)
{
	struct wd_soft_sqe *sqe = NULL;

	pthread_spin_lock(&sctx->slock);
	sqe = &sctx->qfifo[sctx->head];
	if (!sqe->used && !sqe->complete) { // find the next not used sqe
		sctx->head++;
		if (unlikely(sctx->head == MAX_SOFT_QUEUE_LENGTH))
			sctx->head = 0;

		sqe->used = 1;
		sqe->complete = 1;
		sqe->id = tag_id;
		sqe->result = 0;
		__atomic_fetch_add(&sctx->run_num, 0x1, __ATOMIC_ACQUIRE);
		pthread_spin_unlock(&sctx->slock);
	} else {
		pthread_spin_unlock(&sctx->slock);
		return -WD_EBUSY;
	}

	return 0;
}

int wd_put_sqe_to_queue(struct wd_soft_ctx *sctx, __u32 *tag_id, __u8 *result)
{
	struct wd_soft_sqe *sqe = NULL;

	/* The queue is not used */
	if (sctx->run_num < 1)
		return -WD_EAGAIN;

	if (pthread_spin_trylock(&sctx->rlock))
		return -WD_EAGAIN;
	sqe = &sctx->qfifo[sctx->tail];
	if (sqe->used && sqe->complete) { // find a used sqe
		sctx->tail++;
		if (unlikely(sctx->tail == MAX_SOFT_QUEUE_LENGTH))
			sctx->tail = 0;

		*tag_id = sqe->id;
		*result = sqe->result;
		sqe->used = 0x0;
		sqe->complete = 0x0;
		__atomic_fetch_sub(&sctx->run_num, 0x1, __ATOMIC_ACQUIRE);
		pthread_spin_unlock(&sctx->rlock);
	} else {
		pthread_spin_unlock(&sctx->rlock);
		return -WD_EAGAIN;
	}

	return 0;
}

int wd_queue_is_busy(struct wd_soft_ctx *sctx)
{
	/* The queue is not used */
	if (sctx->run_num >= MAX_SOFT_QUEUE_LENGTH - 1)
		return -WD_EBUSY;

	return 0;
}

