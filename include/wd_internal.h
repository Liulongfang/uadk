/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2025 Huawei Technologies Co.,Ltd. All rights reserved.
 */

#ifndef WD_INTERNAL_H
#define WD_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>
#include "wd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_REGION_MAX		16
#define DECIMAL_NUMBER		10
#define MAX_FD_NUM	65535
#define MAX_SOFT_QUEUE_LENGTH	1024U

struct wd_ctx_h {
	int fd;
	char dev_path[MAX_DEV_NAME_LEN];
	char *dev_name;
	char *drv_name;
	unsigned long qfrs_offs[UACCE_QFRT_MAX];
	void *qfrs_base[UACCE_QFRT_MAX];
	struct uacce_dev *dev;
	void *priv;
};

struct wd_soft_sqe {
	__u8 used;
	__u8 result;
	__u8 complete;
	__u32 id;
};

/**
 * default queue length set to 1024
 */
struct wd_soft_ctx {
	int fd;
	pthread_spinlock_t slock;
	__u32 head;
	struct wd_soft_sqe qfifo[MAX_SOFT_QUEUE_LENGTH];
	pthread_spinlock_t rlock;
	__u32 tail;
	__u32 run_num;
	void *priv;
};

struct wd_ce_ctx {
	int fd;
	char *drv_name;
	void *priv;
};

struct wd_ctx_internal {
	__u8 op_type;
	__u8 ctx_mode;
	__u8 ctx_type;
	__u8 ctx_used;
	handle_t ctx;	// if ctx is first will cause problem
	__u16 sqn;
	pthread_spinlock_t lock;
	struct wd_alg_driver *drv;
	void *drv_priv;
};

struct wd_ctx_config_internal {
	__u32 ctx_num;
	int shmid;
	struct wd_ctx_internal *ctxs;
	void *priv;
	bool epoll_en;
	unsigned long *msg_cnt;
	char *alg_name;
};

struct wd_datalist {
	void *data;
	__u32 len;
	struct wd_datalist *next;
};

#ifdef __cplusplus
}
#endif

#endif
