// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2020-2026 Huawei Technologies Co.,Ltd. All rights reserved.
 * Copyright 2020-2021 Linaro ltd.
 *
 * Scheduler: Simplified Pure Hash Table with Dynamic Context Expansion
 *
 * Key improvements:
 * - Single global hash table with (region_id, mode, op_type, prop) dimensions
 * - Segment list for non-contiguous ctx ranges
 * - Dual-domain queues for session key.
 * - Dynamic ctx expansion in HUNGRY mode based on load threshold
 * - Packet reception is handled through the active queues in the session key.
 * - Simplified sched_init: only allocate one sync + one async ctx
 * - Removed redundant wd_sched_info layer
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sched.h>
#include <numa.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include "wd_sched.h"

#define MAX_POLL_TIMES			1000
#define HUNGRY_LOAD_THRESHOLD		256
#define SKEY_CTX_MAX_NUM		16
#define SKEY_MAX_THREAD_NUM		64
#define SKEY_LOAD_UPDATE_INTERVAL 128

#define MAX_NUMA_NODES		(NUMA_NUM_NODES >> 5)

/* ============================================================================
 * Hash Table Configuration
 * ============================================================================
 */
#define WD_SCHED_MAX_BUCKETS		512
#define WD_SCHED_MIN_BUCKETS		32
#define WD_SCHED_LOAD_FACTOR		0.75f
#define HASH_PRIME1			73
#define HASH_PRIME2			13
#define HASH_PRIME3			7
#define HASH_PRIME4			11

/* ============================================================================
 * Scheduling Region Mode
 * ============================================================================
 */
enum sched_region_mode {
	SCHED_MODE_SYNC = 0,
	SCHED_MODE_ASYNC = 1,
	SCHED_MODE_BUTT
};

/* ============================================================================
 * Segment List for Domain Index Organization
 * ============================================================================
 */

/**
 * wd_sched_ctx_segment - Contiguous segment of ctx indices in domain
 * @begin: Start index of this segment
 * @end: End index of this segment (inclusive)
 * @next: Pointer to next segment in the linked list
 *
 * Supports non-contiguous ctx ranges via segment list.
 */
struct wd_sched_ctx_segment {
	__u32 begin;
	__u32 end;
	struct wd_sched_ctx_segment *next;
};

/* ============================================================================
 * Session key domain cache processing.
 * ============================================================================
 */

/**
 * wd_sched_domain_idx_cache - Simplified fixed array cache for skey domains
 *
 * Design principles:
 * - Fixed array for cache-friendly memory layout
 * - Atomic operations for lock-free load tracking
 * - Simple RR and load balancing strategies
 * - Maximum 16 queues per thread (typical usage)
 */
struct wd_sched_domain_idx_cache {
	/* === Queue index array === */
	__u32 idx_list[SKEY_CTX_MAX_NUM];	    /* Array of ctx indices */
	atomic_uint load_values[SKEY_CTX_MAX_NUM];  /* Atomic load counters */
	__u32 valid_count;			    /* Number of valid queues */

	/* === Scheduling state === */
	atomic_uint rr_ptr;			    /* Round-robin pointer */
	atomic_uint min_load_idx;		    /* Cached min load index */
	atomic_uint op_counter; 		    /* Operation counter for updates */

	/* === Configuration === */
	__u32 update_interval;			    /* Min load update interval */
	__u8 policy;		    		    /* Scheduling policy */

	/* === Synchronization === */
	pthread_mutex_t cache_lock;		    /* Lock for structure modifications */
};

/**
 * wd_sched_ctx_domain - Scheduling domain with four dimensions
 * @region_id: Region identifier (numa_id or device_id)
 * @mode: Context mode (SYNC/ASYNC)
 * @op_type: Operation type
 * @prop: Property (e.g., device type: HW, CE, SOFT)
 * @segments: Linked list of context ranges
 * @segment_count: Number of segments
 * @total_ctx_count: Total contexts across all segments
 * @current_segment: Current segment pointer for round-robin
 * @current_pos: Current position within segment
 * @valid: Domain validity flag
 * @lock: Synchronization spinlock
 */
struct wd_sched_ctx_domain {
	int region_id;
	__u8 mode;
	__u32 op_type;
	__u8 prop;

	struct wd_sched_ctx_segment *segments;
	__u32 segment_count;
	__u32 total_ctx_count;

	struct wd_sched_ctx_segment *current_segment;
	__u32 current_pos;
	bool valid;

	pthread_mutex_t lock;
};

/**
 * wd_sched_domain_hash_node - Hash table collision chain node
 */
struct wd_sched_domain_hash_node {
	struct wd_sched_ctx_domain domain;
	struct wd_sched_domain_hash_node *next;
};

/**
 * wd_sched_domain_hash_table - Pure dynamic hash table for scheduling domains
 * @buckets: Hash table bucket array
 * @bucket_size: Number of buckets
 * @entry_count: Total entries in table
 * @max_chain_length: Maximum chain length for statistics
 * @lock: Read-write lock for concurrent access
 */
struct wd_sched_domain_hash_table {
	struct wd_sched_domain_hash_node **buckets;
	__u32 bucket_size;
	__u32 entry_count;
	__u32 max_chain_length;

	pthread_mutex_t lock;
};

/* ============================================================================
 * Dual-Domain Structure for Session Key
 * ============================================================================
 */

/**
 * wd_sched_key_domain - Session domain with min-heap
 * @idx_cache: Index cache with min-heap for load-based selection
 * @lock: Synchronization spinlock
 * @expanded_count: Track how many times ctx has been expanded
 */
struct wd_sched_key_domain {
	struct wd_sched_domain_idx_cache idx_cache;
	pthread_mutex_t lock;
	__u32 expanded_count;
};

/**
 * wd_sched_key - Session-level scheduling key
 * @region_id: Region identifier
 * @type: Operation type
 * @mode: Current mode (SYNC/ASYNC)
 * @dev_id: Device identifier (for SCHED_POLICY_DEV)
 * @ctx_prop: Context property
 * @is_stream: Stream mode flag
 * @prio_mode: Priority mode
 * @pkt_size: Current packet size
 * @sync_domain: Min-heap domain for sync contexts
 * @async_domain: Min-heap domain for async contexts
 * @lock: Synchronization spinlock
 */
struct wd_sched_key {
	int region_id;
	__u8 type;
	__u8 mode;
	__u32 dev_id;
	__u8 ctx_prop;
	__u16 is_stream;
	__u16 prio_mode;
	__u32 pkt_size;

	struct wd_sched_key_domain sync_domain;
	struct wd_sched_key_domain async_domain;

	pthread_mutex_t lock;
};

/**
 * wd_sched_ctx - Main scheduler context
 * @policy: Scheduling policy type
 * @type_num: Number of operation types
 * @mode_num: Number of modes (SYNC/ASYNC)
 * @region_num: Number of regions (numa or devices)
 * @poll_func: Poll function for receiving responses
 * @domain_hash_table: Global hash table for all domains
 * @skey_num: Number of active session keys
 * @skey_lock: Lock for skey array
 * @skey: Array of session keys
 * @poll_tid: Thread IDs for polling
 */
struct wd_sched_ctx {
	__u32 policy;
	__u32 type_num;
	__u32 mode_num;
	__u16 region_num;

	user_poll_func poll_func;
	struct wd_sched_domain_hash_table *domain_hash_table;

	__u32 skey_num;
	pthread_mutex_t skey_lock;
	struct wd_sched_key *skey[SKEY_MAX_THREAD_NUM];
	__u32 poll_tid[SKEY_MAX_THREAD_NUM];
};

/* ============================================================================
 * Hash Table Core Operations
 * ============================================================================
 */

static bool wd_sched_is_prime(__u32 n)
{
	__u32 i;

	if (n <= 1)
		return false;
	if (n <= 3)
		return true;
	if (n % 2 == 0 || n % 3 == 0)
		return false;

	for (i = 5; i * i <= n; i += 6) {
		if (n % i == 0 || n % (i + 2) == 0)
			return false;
	}

	return true;
}

static __u32 wd_sched_find_prime(__u32 n)
{
	while (!wd_sched_is_prime(n))
		n++;
	return n;
}

static __u32 wd_sched_compute_bucket_size(__u32 estimated_entries)
{
	__u32 target_size;

	target_size = (estimated_entries * 4) / 3;

	if (target_size < WD_SCHED_MIN_BUCKETS)
		target_size = WD_SCHED_MIN_BUCKETS;
	if (target_size > WD_SCHED_MAX_BUCKETS)
		target_size = WD_SCHED_MAX_BUCKETS;

	return wd_sched_find_prime(target_size);
}

/**
 * wd_sched_hash_compute - Compute hash value for four-dimensional domain key
 * @region_id: Region identifier
 * @mode: Context mode
 * @op_type: Operation type
 * @prop: Property
 * @bucket_size: Hash table bucket count
 *
 * Combines four dimensions using prime number multipliers.
 */
static inline __u32 wd_sched_hash_compute(int region_id, __u8 mode,
                                          __u32 op_type, __u8 prop, __u32 bucket_size)
{
	__u32 hash;

	hash = (region_id * HASH_PRIME1) + (mode * HASH_PRIME2) +
	       (op_type * HASH_PRIME3) + (prop * HASH_PRIME4);
	return hash % bucket_size;
}

static inline bool wd_sched_domain_key_match(
	int region_id1, __u8 mode1, __u32 op_type1, __u8 prop1,
	int region_id2, __u8 mode2, __u32 op_type2, __u8 prop2)
{
	return (region_id1 == region_id2 && mode1 == mode2 &&
		op_type1 == op_type2 && prop1 == prop2);
}

/**
 * wd_sched_hash_table_create - Create hash table
 * @estimated_entries: Estimated number of entries
 *
 * Returns: Initialized hash table or NULL on error
 */
static struct wd_sched_domain_hash_table *
wd_sched_hash_table_create(__u32 estimated_entries)
{
	struct wd_sched_domain_hash_table *table;
	__u32 bucket_size;
	int ret;

	table = calloc(1, sizeof(*table));
	if (!table)
		return NULL;

	bucket_size = wd_sched_compute_bucket_size(estimated_entries);
	WD_DEBUG("Hash table: estimated_entries=%u, bucket_size=%u\n",
		 estimated_entries, bucket_size);

	table->buckets = calloc(bucket_size, sizeof(*table->buckets));
	if (!table->buckets) {
		free(table);
		return NULL;
	}

	table->bucket_size = bucket_size;
	table->entry_count = 0;
	table->max_chain_length = 0;

	ret = pthread_mutex_init(&table->lock, NULL);
	if (ret) {
		free(table->buckets);
		free(table);
		return NULL;
	}

	return table;
}

static void wd_sched_hash_table_destroy(struct wd_sched_domain_hash_table *table)
{
	struct wd_sched_domain_hash_node *node, *next;
	__u32 i;

	if (!table)
		return;

	for (i = 0; i < table->bucket_size; i++) {
		node = table->buckets[i];
		while (node) {
			next = node->next;

			/* Release segment linked list */
			struct wd_sched_ctx_segment *seg = node->domain.segments;
			while (seg) {
				struct wd_sched_ctx_segment *next_seg = seg->next;
				free(seg);
				seg = next_seg;
			}

			pthread_mutex_destroy(&node->domain.lock);
			free(node);
			node = next;
		}
	}

	pthread_mutex_destroy(&table->lock);
	free(table->buckets);
	free(table);
}

static struct wd_sched_ctx_domain *
wd_sched_hash_table_lookup(struct wd_sched_domain_hash_table *table,
                           int region_id, __u8 mode, __u32 op_type, __u8 prop)
{
	struct wd_sched_domain_hash_node *node;
	struct wd_sched_ctx_domain *domain = NULL;
	__u32 hash_idx;
	__u8 node_idx = 0;

	if (!table)
		return NULL;

	hash_idx = wd_sched_hash_compute(region_id, mode, op_type, prop, table->bucket_size);

	pthread_mutex_lock(&table->lock);
	node = table->buckets[hash_idx];
	while (node) {
		if (wd_sched_domain_key_match(
			node->domain.region_id, node->domain.mode, node->domain.op_type,
			node->domain.prop,	region_id, mode, op_type, prop)) {
			domain = &node->domain;
			break;
		}
		node = node->next;
		node_idx++;
	}
	pthread_mutex_unlock(&table->lock);
	WD_DEBUG("Get domain hash_idx: %u ------node idx: %u.\n", hash_idx, node_idx);

	return domain;
}

static struct wd_sched_ctx_domain *
wd_sched_hash_table_insert(struct wd_sched_domain_hash_table *table,
                           int region_id, __u8 mode, __u32 op_type, __u8 prop)
{
	struct wd_sched_domain_hash_node *node, *new_node;
	struct wd_sched_ctx_domain *existing;
	__u32 chain_length;
	__u32 hash_idx;
	int ret;

	if (!table)
		return NULL;

	existing = wd_sched_hash_table_lookup(table, region_id, mode, op_type, prop);
	if (existing)
		return existing;

	hash_idx = wd_sched_hash_compute(region_id, mode, op_type, prop, table->bucket_size);
	WD_DEBUG("Instance domain hash_idx: %u\n", hash_idx);

	pthread_mutex_lock(&table->lock);
	/* Alloc and initialize new domain */
	new_node = calloc(1, sizeof(*new_node));
	if (!new_node) {
		pthread_mutex_unlock(&table->lock);
		return NULL;
	}

	/* Initialize new domain */
	new_node->domain.region_id = region_id;
	new_node->domain.mode = mode;
	new_node->domain.op_type = op_type;
	new_node->domain.prop = prop;
	new_node->domain.segments = NULL;
	new_node->domain.segment_count = 0;
	new_node->domain.total_ctx_count = 0;
	new_node->domain.current_segment = NULL;
	new_node->domain.current_pos = 0;
	new_node->domain.valid = false;

	ret = pthread_mutex_init(&new_node->domain.lock, NULL);
	if (ret) {
		pthread_mutex_unlock(&table->lock);
		free(new_node);
		return NULL;
	}

	new_node->next = table->buckets[hash_idx];
	table->buckets[hash_idx] = new_node;

	table->entry_count++;
	if (new_node->next) {
		chain_length++;
		if (chain_length > table->max_chain_length)
			table->max_chain_length = chain_length;
	}

	pthread_mutex_unlock(&table->lock);

	WD_DEBUG("Created new domain: region=%d, mode=%u, op_type=%u, prop=%u\n",
		 region_id, mode, op_type, prop);

	return &new_node->domain;
}

/* ============================================================================
 * Segment List Operations
 * ============================================================================
 */

/**
 * wd_sched_domain_add_segment - Add context range segment to domain
 * @domain: Target domain
 * @begin: Start context index
 * @end: End context index (inclusive)
 *
 * Supports non-contiguous context ranges via segment list.
 */
static int wd_sched_domain_add_segment(struct wd_sched_ctx_domain *domain,
                                       __u32 begin, __u32 end)
{
	struct wd_sched_ctx_segment *seg, *new_seg;

	if (!domain || begin > end)
		return -WD_EINVAL;

	new_seg = calloc(1, sizeof(*new_seg));
	if (!new_seg)
		return -WD_ENOMEM;

	new_seg->begin = begin;
	new_seg->end = end;
	new_seg->next = NULL;

	pthread_mutex_lock(&domain->lock);

	/* Append to segment list tail */
	if (!domain->segments) {
		domain->segments = new_seg;
	} else {
		seg = domain->segments;
		while (seg->next)
			seg = seg->next;
		seg->next = new_seg;
	}

	domain->segment_count++;
	domain->total_ctx_count += (end - begin + 1);

	/* Initialize polling state */
	if (!domain->current_segment)
		domain->current_segment = domain->segments;

	pthread_mutex_unlock(&domain->lock);

	WD_DEBUG("Added segment to domain: begin=%u, end=%u, total_count=%u\n",
		 begin, end, domain->total_ctx_count);

	return 0;
}

/**
 * wd_sched_domain_get_next_rr - Get next context via round-robin from domain
 * @domain: Source domain
 *
 * Returns: Next context index in round-robin order
 * Time complexity: O(1)
 */
static __u32 wd_sched_domain_get_next_rr(struct wd_sched_ctx_domain *domain)
{
	__u32 pos, next_pos;

	if (!domain || !domain->segments || domain->total_ctx_count == 0)
		return INVALID_POS;

	pthread_mutex_lock(&domain->lock);
	if (!domain->current_segment)
		domain->current_segment = domain->segments;

	pos = domain->current_pos;
	next_pos = pos + 1;
	/* Move to next position */
	if (next_pos <= domain->current_segment->end) {
		domain->current_pos = next_pos;
	} else {
		/* Move to next segment */
		if (domain->current_segment->next) {
			domain->current_segment = domain->current_segment->next;
			domain->current_pos = domain->current_segment->begin;
			next_pos = domain->current_segment->begin;
		} else {
			/* Loop back to beginning */
			domain->current_segment = domain->segments;
			next_pos = domain->segments->begin;
		}
		domain->current_pos = next_pos;
	}

	pthread_mutex_unlock(&domain->lock);
	WD_DEBUG("Get next ctx: pos=%u, current_pos=%u\n", pos, domain->current_pos);

	return pos;
}

/* ============================================================================
 * SKey Domain Cache Management Functions
 * ============================================================================
 */
/**
 * wd_sched_skey_cache_init - Initialize skey domain cache
 * @cache: Pointer to cache structure
 * @policy: Scheduling policy * @sched_type: Scheduling policy type (cannot modify per API contract)
 * 
 * Initialize fixed array cache with invalid positions and zero loads.
 */
static int wd_sched_skey_cache_init(struct wd_sched_domain_idx_cache *cache, __u8 policy)
{
	int i;

	if (!cache) {
		WD_ERR("Invalid cache pointer\n");
		return -WD_EINVAL;
	}

	/* Initialize array with invalid positions */
	for (i = 0; i < SKEY_CTX_MAX_NUM; i++) {
		cache->idx_list[i] = INVALID_POS;
		atomic_store(&cache->load_values[i], 0);
	}

	/* Initialize atomic counters */
	atomic_store(&cache->rr_ptr, 0);
	atomic_store(&cache->min_load_idx, 0);
	atomic_store(&cache->op_counter, 0);

	/* Set configuration */
	cache->valid_count = 0;
	cache->update_interval = SKEY_LOAD_UPDATE_INTERVAL;
	cache->policy = policy;

	/* Initialize structure lock */
	if (pthread_mutex_init(&cache->cache_lock, NULL)) {
		WD_ERR("Failed to init cache lock\n");
		return -WD_EINVAL;
	}

	return WD_SUCCESS;
}
/**
 * wd_sched_skey_cache_uninit - Cleanup skey domain cache
 * @cache: Pointer to cache structure
 * 
 * Release resources and reset cache state.
 */
static void wd_sched_skey_cache_uninit(struct wd_sched_domain_idx_cache *cache)
{
	if (!cache)
		return;

	pthread_mutex_destroy(&cache->cache_lock);

	/* Reset cache state */
	for (int i = 0; i < SKEY_CTX_MAX_NUM; i++) {
		cache->idx_list[i] = INVALID_POS;
		atomic_store(&cache->load_values[i], 0);
	}

	cache->valid_count = 0;
}
/**
 * wd_sched_skey_add_ctx - Add ctx to skey domain cache
 * @cache: Pointer to cache structure  
 * @ctx_id: Context ID to add
 * 
 * Add ctx to next available position in fixed array.
 * Returns 0 on success, negative error code on failure.
 */
static int wd_sched_skey_add_ctx(struct wd_sched_domain_idx_cache *cache, __u32 ctx_id)
{
	__u32 i;

	if (!cache || ctx_id == INVALID_POS) {
		WD_ERR("Invalid parameters\n");
		return -WD_EINVAL;
	}

	pthread_mutex_lock(&cache->cache_lock);
	/* Check if cache is full */
	if (cache->valid_count >= SKEY_CTX_MAX_NUM) {
		pthread_mutex_unlock(&cache->cache_lock);
		WD_ERR("SKey cache full, cannot add more queues\n");
		return -WD_EINVAL;
	}

	/* Check for duplicate ctx_id */
	for (i = 0; i < cache->valid_count; i++) {
		if (cache->idx_list[i] == ctx_id) {
			WD_ERR("Context %u already exists in skey cache at position %d\n", ctx_id, i);
			pthread_mutex_unlock(&cache->cache_lock);
			return -WD_EEXIST;
		}
	}

	/* Update min load index if as the new ctx */
	atomic_store(&cache->min_load_idx, ctx_id);

	/* Add to next available position */
	cache->idx_list[cache->valid_count] = ctx_id;
	atomic_store(&cache->load_values[cache->valid_count], 0);
	cache->valid_count++;
	pthread_mutex_unlock(&cache->cache_lock);

	WD_DEBUG("Added ctx %u to skey cache at position %u, list valid_count: %u\n", 
			ctx_id, cache->valid_count - 1, cache->valid_count);

	return WD_SUCCESS;
}

/**
 * wd_sched_skey_remove_ctx - Remove ctx from skey domain cache
 * @cache: Pointer to cache structure
 * @ctx_id: Context ID to remove
 * 
 * Remove ctx by shifting array elements to maintain continuity.
 * Returns 0 on success, negative error code if not found.
 */
static int wd_sched_skey_remove_ctx(struct wd_sched_domain_idx_cache *cache,
                                   __u32 ctx_id)
{
	int i, found = 0;
	__u32 current_min;

	if (!cache) {
		WD_ERR("Invalid cache pointer\n");
		return -WD_EINVAL;
	}

	pthread_mutex_lock(&cache->cache_lock);
	/* Find and remove the ctx */
	for (i = 0; i < cache->valid_count; i++) {
		if (cache->idx_list[i] == ctx_id) {
			found = 1;
			break;
		}
	}

	if (!found) {
		WD_ERR("Context %u not found in skey cache\n", ctx_id);
		pthread_mutex_unlock(&cache->cache_lock);
		return -WD_ENODEV;
	}

	/* Shift remaining elements to fill the gap */
	for (; i < cache->valid_count - 1; i++) {
		cache->idx_list[i] = cache->idx_list[i + 1];
		atomic_store(&cache->load_values[i], 
		atomic_load(&cache->load_values[i + 1]));
	}

	/* Clear last position */
	cache->idx_list[cache->valid_count - 1] = INVALID_POS;
	atomic_store(&cache->load_values[cache->valid_count - 1], 0);
	cache->valid_count--;

	/* Reset pointers if cache becomes empty */
	if (cache->valid_count == 0) {
		atomic_store(&cache->rr_ptr, 0);
		atomic_store(&cache->min_load_idx, 0);
	} else {
		/* Adjust min load index if necessary */
		current_min = atomic_load(&cache->min_load_idx);
		if (current_min >= cache->valid_count)
	    		atomic_store(&cache->min_load_idx, 0);
	}
	pthread_mutex_unlock(&cache->cache_lock);
	WD_DEBUG("Removed ctx %u from skey cache, valid_count: %u\n",
	     ctx_id, cache->valid_count);

	return WD_SUCCESS;
}

/**
 * wd_sched_update_min_load - Update cached min load index
 * @cache: Pointer to cache structure
 * 
 * Scan valid queues to find the one with minimum load.
 * Called periodically to avoid frequent scanning.
 */
static void wd_sched_update_min_load(struct wd_sched_domain_idx_cache *cache)
{
	__u32 min_load = UINT_MAX;
	__u32 min_idx = 0;
	__u32 i, load;

	if (cache->valid_count == 0)
		return;

	/* Simple linear scan - efficient for small arrays */
	for (i = 0; i < cache->valid_count; i++) {
		load = atomic_load(&cache->load_values[i]);
		if (load < min_load) {
	   		min_load = load;
	    		min_idx = i;
		}
	}

	atomic_store(&cache->min_load_idx, min_idx);
}

/**
 * wd_sched_skey_pick_next - Pick next ctx from skey domain cache
 * @cache: Pointer to cache structure
 * 
 * Select next ctx based on scheduling policy:
 * - RR: Simple round-robin selection
 * - LOAD_BALANCE: Choose ctx with minimum load
 * 
 * Returns selected ctx index, or INVALID_POS if no valid ctx.
 */
static __u32 wd_sched_skey_pick_next(struct wd_sched_domain_idx_cache *cache, __u32 *ctx_idx)
{
	__u32 selected_idx;
	__u32 op_count;

	if (!cache || cache->valid_count == 0) {
		return INVALID_POS;
	}

	switch (cache->policy) {
	case SCHED_POLICY_RR:
	case SCHED_POLICY_NONE:
	case SCHED_POLICY_SINGLE:
	case SCHED_POLICY_DEV:
	case SCHED_POLICY_LOOP:
	case SCHED_POLICY_INSTR:
		/* Round-robin: atomic increment and modulo */
		selected_idx = atomic_fetch_add(&cache->rr_ptr, 1) % cache->valid_count;
		break;
	case SCHED_POLICY_HUNGRY:
		/* Update min load periodically */
		op_count = atomic_fetch_add(&cache->op_counter, 1);
		if (op_count % cache->update_interval == 0)
			wd_sched_update_min_load(cache);

		/* Load balancing: use cached min load index */
		selected_idx = atomic_load(&cache->min_load_idx);
		break;
	default:
		WD_ERR("Unknown scheduling policy: %d\n", cache->policy);
		selected_idx = INVALID_POS;
		break;
	}

	/* Ensure index is within valid range */
	if (selected_idx >= cache->valid_count)
		return INVALID_POS;

	*ctx_idx = selected_idx;
	return cache->idx_list[selected_idx];
}

/**
 * wd_sched_skey_update_load - Update load for a specific ctx
 * @cache: Pointer to cache structure
 * @ctx_idx: Context index in list
 * @delta: Load delta (positive for send, negative for receive)
 * 
 * Atomically update load counter for the specified ctx.
 * Returns 0 on success, negative error code if ctx not found.
 */
static int wd_sched_skey_update_load(struct wd_sched_domain_idx_cache *cache,
                                    __u32 ctx_idx, int delta)
{
	/* Atomic update without locking */
	if (delta > 0)
		atomic_fetch_add(&cache->load_values[ctx_idx], delta);
	else
		atomic_fetch_sub(&cache->load_values[ctx_idx], -delta);
	return WD_SUCCESS;
}

/* ============================================================================
 * Session Key Domain Initialization
 * ============================================================================
 */

/**
 * wd_sched_skey_domain_init - Initialize session domain with min-heap
 * @key_domain: Target key domain
 * @ctx_idx: context indices idx
 * @policy: current session's policy
 *
 * Initializes dual-domain structure for session.
 */
static int wd_sched_skey_domain_init(struct wd_sched_key_domain *key_domain,
                                    __u32 ctx_idx,    __u8 policy)
{
	int ret;

	if (!key_domain)
		return -WD_EINVAL;

	ret = wd_sched_skey_cache_init(&key_domain->idx_cache, policy);
	if (ret)
		return ret;

	ret = wd_sched_skey_add_ctx(&key_domain->idx_cache, ctx_idx);
	if (ret)
		goto init_err;

	ret = pthread_mutex_init(&key_domain->lock, NULL);
	if (ret) {
		goto add_ctx_err;
	}

	key_domain->expanded_count = 0;

	return 0;

add_ctx_err:
	wd_sched_skey_remove_ctx(&key_domain->idx_cache, ctx_idx);
init_err:
	wd_sched_skey_cache_uninit(&key_domain->idx_cache);
	return ret;
}

/**
 * wd_sched_skey_domain_destroy - Release session domain resources
 */
static void wd_sched_skey_domain_destroy(struct wd_sched_key_domain *key_domain)
{
	if (!key_domain)
		return;

	pthread_mutex_destroy(&key_domain->lock);
	wd_sched_skey_cache_uninit(&key_domain->idx_cache);
}

/**
 * wd_sched_poll_skey - Poll contexts for scheduler session
 * @sched_ctx: Scheduler context
 * @skey: Session key
 * @expect: Expected number of responses
 * @count: Actual response count (output)
 *
 * Polls all contexts in session domains and updates load values.
 */
static int wd_sched_poll_skey(struct wd_sched_ctx *sched_ctx, struct wd_sched_key *skey,
			 __u32 expect, __u32 *count)
{
	__u32 sum_poll_num = 0;
	__u32 current_load;
	__u32 poll_num;
	__u32 idx, i;
	int ret;

	/* Poll async domain contexts */
	for (i = 0; i < skey->async_domain.idx_cache.valid_count; i++) {
		idx = skey->async_domain.idx_cache.idx_list[i];

		poll_num = 0;
		ret = sched_ctx->poll_func(idx, expect, &poll_num);
		if ((ret < 0) && (ret != -EAGAIN))
			return ret;

		if (poll_num > 0)
			sum_poll_num += poll_num;

		/* Update load value for this context */
		if (skey->async_domain.idx_cache.policy == SCHED_POLICY_HUNGRY)
			wd_sched_skey_update_load(&skey->async_domain.idx_cache, i, poll_num);
	}
	*count = sum_poll_num;

	return 0;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================
 */

#define nop() asm volatile("nop")
static void delay_us(int ustime)
{
	int cycle = 2600;
	int i, j;

	for (i = 0; i < ustime; i++) {
		for (j = 0; j < cycle; j++)
			nop();
	}
	usleep(1);
}

static void sched_skey_param_init(struct wd_sched_ctx *sched_ctx, struct wd_sched_key *skey)
{
	__u32 i;

	pthread_mutex_lock(&sched_ctx->skey_lock);
	for (i = 0; i < SKEY_MAX_THREAD_NUM; i++) {
		if (sched_ctx->skey[i] == NULL) {
			sched_ctx->skey[i] = skey;
			sched_ctx->skey_num++;
			pthread_mutex_unlock(&sched_ctx->skey_lock);
			WD_ERR("success: get valid skey node[%u]!\n", i);
			return;
		}
	}
	pthread_mutex_unlock(&sched_ctx->skey_lock);
	WD_ERR("invalid: skey node number is too much!\n");
}

static handle_t sched_session_common_init(struct wd_sched_ctx *sched_ctx,
	struct sched_params *param)
{
	struct wd_sched_key *skey;
	unsigned int node;
	int ret = 0;

	if (getcpu(NULL, &node)) {
		WD_ERR("failed to get node, errno %d!\n", errno);
		return (handle_t)(-errno);
	}

	if (!sched_ctx) {
		WD_ERR("invalid: sched ctx is NULL!\n");
		return (handle_t)(-WD_EINVAL);
	}

	skey = malloc(sizeof(struct wd_sched_key));
	if (!skey) {
		WD_ERR("failed to alloc memory for session sched key!\n");
		return (handle_t)(-WD_ENOMEM);
	}
	memset(skey, 0, sizeof(struct wd_sched_key));

	if (!param) {
		skey->region_id = node;
		if (wd_need_debug())
			WD_DEBUG("session don't set scheduler parameters!\n");
	} else {
		if (param->numa_id >= 0) {
			skey->region_id = param->numa_id;
		} else {
			skey->region_id = node;
		}
		skey->type = param->type;
		skey->ctx_prop = param->ctx_prop;
	}

	return (handle_t)skey;

out:
	free(skey);
	return (handle_t)(-WD_EINVAL);	
}

static struct wd_sched_key *sched_get_poll_skey(struct wd_sched_ctx *sched_ctx)
{
	__u32 ctx_num = sched_ctx->skey_num;
	__u32 tid = pthread_self();
	__u16 tpos, start_pos;
	__u16 i, tidx = 0;

	/* Randomize the initial query position. */
	start_pos = tid % ctx_num;

	for (i = 0; i < ctx_num; i++) {
		/* Each thread re-determines the starting traversal position based on its tid. */
		tpos = (start_pos + i) % ctx_num;
		if (sched_ctx->poll_tid[tpos] == tid) {
			tidx = tpos;
			break;
		} else if (sched_ctx->poll_tid[tpos] == 0) {
			pthread_mutex_lock(&sched_ctx->skey_lock);
			if (sched_ctx->poll_tid[tpos] == 0) {
				sched_ctx->poll_tid[tpos] = tid;
				tidx = tpos;
			} else {
				pthread_mutex_unlock(&sched_ctx->skey_lock);
				return NULL;
			}
			pthread_mutex_unlock(&sched_ctx->skey_lock);
			break;
		}
	}

	return sched_ctx->skey[tidx];
}

/**
 * session_sched_init_ctx - Pre-fetch single context from domain for session
 * @sched_ctx: Scheduler context
 * @region_id: Region identifier
 * @op_type: Operation type
 * @prop: Property
 * @sched_mode: Mode (SYNC/ASYNC)
 *
 * Returns: Context index from domain
 */
static __u32 session_sched_init_ctx(struct wd_sched_ctx *sched_ctx,
                                    int region_id, __u32 op_type, __u8 prop,
                                    const int sched_mode)
{
	struct wd_sched_ctx_domain *domain = NULL;

	if (region_id >= sched_ctx->region_num || sched_mode >= SCHED_MODE_BUTT ||
	     op_type >= sched_ctx->type_num || prop >= UADK_ALG_TYPE_MAX) {
		WD_ERR("invalid: region: %d, mode: %d, type: %u!, prop: %u\n",
		       region_id, sched_mode, op_type, prop);
		return INVALID_POS;
	}

	if (!sched_ctx->domain_hash_table)
		return INVALID_POS;

	domain = wd_sched_hash_table_lookup(sched_ctx->domain_hash_table,
						      region_id, sched_mode, op_type, prop);
	if (!domain || !domain->valid)
		return INVALID_POS;

	return wd_sched_domain_get_next_rr(domain);
}

/**
 * session_sched_domain_destroy - Destroy session domains
 * @skey: Session key to destroy domains for
 *
 * Releases all resources associated with session domains.
 */
static void session_sched_domain_destroy(struct wd_sched_key *skey)
{
	if (!skey)
		return;

	/* Destroy both sync and async domains */
	wd_sched_skey_domain_destroy(&skey->sync_domain);
	wd_sched_skey_domain_destroy(&skey->async_domain);

	WD_DEBUG("Destroyed session domains for skey\n");
}

/**
 * session_sched_domain_init - Initialize session domains with sync/async contexts
 * @sched_ctx: Scheduler context
 * @skey: Session key to initialize
 *
 * Pre-fetches sync and async contexts and initializes corresponding domains.
 * Returns: 0 on success, negative error code on failure.
 */
static int session_sched_domain_init(struct wd_sched_ctx *sched_ctx, 
                                        struct wd_sched_key *skey)
{
	__u32 sync_ctx, async_ctx;

	if (!sched_ctx || !skey) {
		WD_ERR("invalid: sched_ctx or skey is NULL!\n");
		return -WD_EINVAL;
	}

	/* Pre-fetch one sync context */
	sync_ctx = session_sched_init_ctx(sched_ctx, skey->region_id, skey->type,
	                      skey->ctx_prop, SCHED_MODE_SYNC);

	/* Pre-fetch one async context */
	async_ctx = session_sched_init_ctx(sched_ctx, skey->region_id, skey->type,
	                       skey->ctx_prop, SCHED_MODE_ASYNC);

	if (sync_ctx == INVALID_POS && async_ctx == INVALID_POS) {
		WD_ERR("there is no valid sync_ctx or async_ctx domain!\n");
		return -WD_EINVAL;
	}

	WD_DEBUG("Got ctx index sync_ctx=%u, async_ctx=%u\n", 		sync_ctx, async_ctx);

	/* Initialize sync domain if context is valid */
	if (sync_ctx != INVALID_POS) {
		if (wd_sched_skey_domain_init(&skey->sync_domain, sync_ctx, sched_ctx->policy) != 0) {
			WD_ERR("failed to init sync domain!\n");
			return -WD_EINVAL;
		}
	}

	/* Initialize async domain if context is valid */
	if (async_ctx != INVALID_POS) {
		if (wd_sched_skey_domain_init(&skey->async_domain, async_ctx, sched_ctx->policy) != 0) {
			WD_ERR("failed to init async domain!\n");
			/* Cleanup sync domain if async domain init failed */
			if (sync_ctx != INVALID_POS)
				wd_sched_skey_domain_destroy(&skey->sync_domain);
			return -WD_EINVAL;
		}
	}

	return 0;
}

/* ============================================================================
 * Scheduler Policy Functions
 * ============================================================================
 */
/**
 * round_robin_sched_init - Initialize session with single sync and async ctx
 * @h_sched_ctx: Scheduler handle (cannot modify per API contract)
 * @sched_param: Scheduling parameters (cannot modify per API contract)
 *
 * Allocates session key and pre-fetches one sync and one async context.
 */
static handle_t round_robin_sched_init(handle_t h_sched_ctx, void *sched_param)
{
	struct wd_sched_ctx *sched_ctx = (struct wd_sched_ctx *)h_sched_ctx;
	struct sched_params *param = (struct sched_params *)sched_param;
	struct wd_sched_key *skey;
	handle_t hskey;
	int ret = 0;

	hskey = sched_session_common_init(sched_ctx, param);
	if (WD_IS_ERR(hskey)) {
		WD_ERR("failed to init session schedule key!\n");
		return hskey;
	}

	skey = (struct wd_sched_key *)hskey;
	ret = session_sched_domain_init(sched_ctx, skey);
	if (ret != 0) {
		WD_ERR("failed to initialize session domains!\n");
		free(skey);
		return (handle_t)(-WD_EINVAL);
	}

	sched_skey_param_init(sched_ctx, skey);	
	WD_INFO("initialized RR scheduler with sync and async domains\n");

	return hskey;
}

/**
 * round_robin_pick_next_ctx - Pick context with load-based selection
 * @h_sched_ctx: Scheduler handle (cannot modify per API contract)
 * @sched_key: Session key (cannot modify per API contract)
 * @sched_mode: Mode (cannot modify per API contract)
 *
 * Returns: Context index with minimum load
 * Time complexity: O(1)
 */
static __u32 round_robin_pick_next_ctx(handle_t h_sched_ctx, void *sched_key,
					 const int sched_mode)
{
	struct wd_sched_key *skey = (struct wd_sched_key *)sched_key;
	struct wd_sched_key_domain *domain;
	__u32 min_ctx, ctx_idx;
	__u32 new_ctx;

	if (unlikely(!h_sched_ctx || !skey)) {
		WD_ERR("invalid: sched ctx or key is NULL!\n");
		return INVALID_POS;
	}

	if (sched_mode == SCHED_MODE_SYNC) {
		domain = &skey->sync_domain;
	} else {
		domain = &skey->async_domain;
	}

	/* Get current minimum load context */
	min_ctx = wd_sched_skey_pick_next(&domain->idx_cache, &ctx_idx);
	if (min_ctx == INVALID_POS)
		return INVALID_POS;

	return min_ctx;
}

/**
 * round_robin_poll_policy - Poll policy for session scheduler
 * @h_sched_ctx: Scheduler handle (cannot modify per API contract)
 * @expect: Expected number of responses (cannot modify per API contract)
 * @count: Actual response count (cannot modify per API contract)
 *
 * Returns: Status code
 */
static int round_robin_poll_policy(handle_t h_sched_ctx, __u32 expect, __u32 *count)
{
	struct wd_sched_ctx *sched_ctx = (struct wd_sched_ctx *)h_sched_ctx;
	__u32 skey_num = sched_ctx->skey_num;
	struct wd_sched_key *skey;
	__u32 tid = pthread_self();
	__u16 i, tpos, start_pos;
	__u32 poll_num, sum_count = 0;
	int ret;

	if (unlikely(!count || !sched_ctx || !sched_ctx->poll_func)) {
		WD_ERR("invalid: sched ctx or poll_func is NULL or count is zero!\n");
		return -WD_EINVAL;
	}

	/* Randomize the initial query position. */
	start_pos = tid % skey_num;
	/* Query the queues on each skey separately. */
	for (i = 0; i < skey_num; i++) {
		tpos = (start_pos + i) % skey_num;
		skey = sched_ctx->skey[tpos];
		ret = wd_sched_poll_skey(sched_ctx, skey, expect, &poll_num);
		if (unlikely(ret))
			return ret;

		sum_count += poll_num;
	}
	*count = sum_count;

	return 0;
}

static handle_t sched_none_init(handle_t h_sched_ctx, void *sched_param)
{
	return (handle_t)0;
}

static __u32 sched_none_pick_next_ctx(handle_t sched_ctx,
				      void *sched_key, const int sched_mode)
{
	return 0;
}

static int sched_none_poll_policy(handle_t h_sched_ctx,
				  __u32 expect, __u32 *count)
{
	struct wd_sched_ctx *sched_ctx = (struct wd_sched_ctx *)h_sched_ctx;
	__u32 loop_times = MAX_POLL_TIMES + expect;
	__u32 poll_num = 0;
	int ret;

	if (!sched_ctx || !sched_ctx->poll_func) {
		WD_ERR("invalid: sched ctx or poll_func is NULL!\n");
		return -WD_EINVAL;
	}

	while (loop_times > 0) {
		loop_times--;
		ret = sched_ctx->poll_func(0, 1, &poll_num);
		if ((ret < 0) && (ret != -EAGAIN))
			return ret;
		else if (ret == -EAGAIN)
			continue;

		*count += poll_num;
		if (*count == expect)
			break;
	}

	return 0;
}

static handle_t sched_single_init(handle_t h_sched_ctx, void *sched_param)
{
	return (handle_t)0;
}

static __u32 sched_single_pick_next_ctx(handle_t sched_ctx,
					void *sched_key, const int sched_mode)
{
	if (sched_mode)
		return 1;
	else
		return 0;
}

static int sched_single_poll_policy(handle_t h_sched_ctx,
				    __u32 expect, __u32 *count)
{
	struct wd_sched_ctx *sched_ctx = (struct wd_sched_ctx *)h_sched_ctx;
	__u32 loop_times = MAX_POLL_TIMES + expect;
	__u32 poll_num = 0;
	int ret;

	if (!sched_ctx || !sched_ctx->poll_func) {
		WD_ERR("invalid: sched ctx or poll_func is NULL!\n");
		return -WD_EINVAL;
	}

	while (loop_times > 0) {
		loop_times--;
		ret = sched_ctx->poll_func(1, 1, &poll_num);
		if ((ret < 0) && (ret != -EAGAIN))
			return ret;
		else if (ret == -EAGAIN)
			continue;

		*count += poll_num;
		if (*count == expect)
			break;
	}

	return 0;
}

/**
 * skey_sched_init - Initialize hungry scheduler session with dynamic expansion
 * @h_sched_ctx: Scheduler handle (cannot modify per API contract)
 * @sched_param: Scheduling parameters (cannot modify per API contract)
 *
 * Pre-fetches one sync and one async context, supports dynamic expansion.
 */
static handle_t skey_sched_init(handle_t h_sched_ctx, void *sched_param)
{
	struct wd_sched_ctx *sched_ctx = (struct wd_sched_ctx *)h_sched_ctx;
	struct sched_params *param = (struct sched_params *)sched_param;
	struct wd_sched_key *skey;
	__u32 req_ctx_num = 0;
	handle_t hskey;
	int i, ret = 0;
	__u8 def_prop;

	hskey = sched_session_common_init(sched_ctx, param);
	if (WD_IS_ERR(hskey)) {
		WD_ERR("failed to init session schedule key!\n");
		return hskey;
	}

	skey = (struct wd_sched_key *)hskey;
	def_prop = skey->ctx_prop;
	/* Init and get ctx for every ctx mode */
	for (i = 0; i < UADK_ALG_TYPE_MAX; i++) {
		skey->ctx_prop = i;
		ret = session_sched_domain_init(sched_ctx, skey);
		if (ret != 0)
			continue;

		/* Request two Pre_fetch queues each time. */
		WD_INFO("Successful to request prop=%d type ctx!\n", i);
		req_ctx_num += 2;
	}
	if (!req_ctx_num) {
		free(skey);
		return (handle_t)(-WD_EINVAL);
	}

	/* Restore the initialization prop settings. */
	skey->ctx_prop = def_prop;
	sched_skey_param_init(sched_ctx, skey);
	WD_INFO("initialized Hungry scheduler with sync and async domains\n");

	return hskey;
}

/**
 * skey_sched_pick_next_ctx - Pick context from hungry scheduler with load awareness
 * @h_sched_ctx: Scheduler handle (cannot modify per API contract)
 * @sched_key: Session key (cannot modify per API contract)
 * @sched_mode: Mode (cannot modify per API contract)
 *
 * Returns: Context with minimum load, or expands if threshold exceeded
 * Time complexity: O(1) for selection, O(n) if expansion needed
 */
static __u32 skey_sched_pick_next_ctx(handle_t h_sched_ctx, void *sched_key,
				      const int sched_mode)
{
	struct wd_sched_ctx *sched_ctx = (struct wd_sched_ctx *)h_sched_ctx;
	struct wd_sched_key *skey = (struct wd_sched_key *)sched_key;
	struct wd_sched_key_domain *domain;
	__u32 min_ctx, min_load, ctx_idx;
	__u32 new_ctx;

	if (unlikely(!h_sched_ctx || !skey)) {
		WD_ERR("invalid: sched ctx or key is NULL!\n");
		return INVALID_POS;
	}

	if (sched_mode == SCHED_MODE_SYNC) {
		domain = &skey->sync_domain;
	} else {
		domain = &skey->async_domain;
	}

	/* Get current minimum load context */
	min_ctx = wd_sched_skey_pick_next(&domain->idx_cache, &ctx_idx);
	if (min_ctx == INVALID_POS)
		return INVALID_POS;

	/* Update load value for send one task */
	wd_sched_skey_update_load(&skey->async_domain.idx_cache, ctx_idx, 1);
	min_load = domain->idx_cache.load_values[ctx_idx];

	/* Check if we need to expand context pool */
	if (min_load > HUNGRY_LOAD_THRESHOLD) {
		/* Try to allocate new context from domain */
		new_ctx = session_sched_init_ctx(sched_ctx, skey->region_id, skey->type, skey->ctx_prop, sched_mode);
		if (new_ctx != INVALID_POS) {
			if (wd_sched_skey_add_ctx(&domain->idx_cache, new_ctx) == 0) {
				domain->expanded_count++;
				min_ctx = new_ctx;
			}
		}
	}

	return min_ctx;
}

/**
 * skey_sched_poll_policy - Poll policy for hungry scheduler
 * @h_sched_ctx: Scheduler handle (cannot modify per API contract)
 * @expect: Expected number of responses (cannot modify per API contract)
 * @count: Actual response count (cannot modify per API contract)
 *
 * Returns: Status code
 */
static int skey_sched_poll_policy(handle_t h_sched_ctx, __u32 expect, __u32 *count)
{
	struct wd_sched_ctx *sched_ctx = (struct wd_sched_ctx *)h_sched_ctx;
	struct wd_sched_key *skey;
	int ret;

	if (unlikely(!count || !sched_ctx || !sched_ctx->poll_func)) {
		WD_ERR("invalid: sched ctx or poll_func is NULL or count is zero!\n");
		return -WD_EINVAL;
	}

	skey = sched_get_poll_skey(sched_ctx);
	if (!skey)
		return -WD_EAGAIN;

	ret = wd_sched_poll_skey(sched_ctx, skey, expect, count);
	if (unlikely(ret))
		return ret;

	return 0;
}

/**
 * loop_sched_init - Initialize loop scheduler session with single ctx per mode
 * @h_sched_ctx: Scheduler handle (cannot modify per API contract)
 * @sched_param: Scheduling parameters (cannot modify per API contract)
 *
 * Pre-fetches one sync and one async context.
 */
static handle_t loop_sched_init(handle_t h_sched_ctx, void *sched_param)
{
	struct wd_sched_ctx *sched_ctx = (struct wd_sched_ctx *)h_sched_ctx;
	struct sched_params *param = (struct sched_params *)sched_param;
	struct wd_sched_key *skey;
	__u32 req_ctx_num = 0;
	handle_t hskey;
	int i, ret = 0;
	__u8 def_prop;

	hskey = sched_session_common_init(sched_ctx, param);
	if (WD_IS_ERR(hskey)) {
		WD_ERR("failed to init session schedule key!\n");
		return hskey;
	}

	skey = (struct wd_sched_key *)hskey;
	def_prop = skey->ctx_prop;
	/* Init and get ctx for every ctx mode */
	for (i = 0; i < UADK_ALG_TYPE_MAX; i++) {
		skey->ctx_prop = i;
		ret = session_sched_domain_init(sched_ctx, skey);
		if (ret != 0)
			continue;

		/* Request two Pre_fetch queues each time. */
		WD_INFO("Successful to request prop=%d type ctx!\n", i);
		req_ctx_num += 2;
	}
	if (!req_ctx_num) {
		free(skey);
		return (handle_t)(-WD_EINVAL);
	}

	/* Restore the initialization prop settings. */
	skey->ctx_prop = def_prop;
	sched_skey_param_init(sched_ctx, skey);	
	WD_INFO("initialized Loop scheduler with sync and async domains\n");
	return (handle_t)skey;
}

/**
 * loop_sched_pick_next_ctx - Pick context for loop scheduler
 * @h_sched_ctx: Scheduler handle (cannot modify per API contract)
 * @sched_key: Session key (cannot modify per API contract)
 * @sched_mode: Mode (cannot modify per API contract)
 *
 * Returns: Context index with minimum load
 * Time complexity: O(1)
 */
static __u32 loop_sched_pick_next_ctx(handle_t h_sched_ctx, void *sched_key,
				      const int sched_mode)
{
	return round_robin_pick_next_ctx(h_sched_ctx, sched_key, sched_mode);
}

static int loop_sched_poll_policy(handle_t h_sched_ctx, __u32 expect, __u32 *count)
{
	return round_robin_poll_policy(h_sched_ctx, expect, count);
}

static handle_t instr_sched_init(handle_t h_sched_ctx, void *sched_param)
{
	struct wd_sched_ctx *sched_ctx = (struct wd_sched_ctx *)h_sched_ctx;
	struct sched_params *param = (struct sched_params *)sched_param;
	struct wd_sched_key *skey;
	handle_t hskey;
	int ret = 0;

	hskey = sched_session_common_init(sched_ctx, param);
	if (WD_IS_ERR(hskey)) {
		WD_ERR("failed to init session schedule key!\n");
		return hskey;
	}

	skey = (struct wd_sched_key *)hskey;
	ret = session_sched_domain_init(sched_ctx, skey);
	if (ret != 0) {
		WD_ERR("failed to initialize session domains!\n");
		free(skey);
		return (handle_t)(-WD_EINVAL);
	}

	sched_skey_param_init(sched_ctx, skey);

	return (handle_t)skey;
}

static __u32 instr_sched_pick_next_ctx(handle_t h_sched_ctx, void *sched_key,
				       const int sched_mode)
{
	struct wd_sched_key *skey = (struct wd_sched_key *)sched_key;
	struct wd_sched_key_domain *domain;
	__u32 min_ctx, ctx_idx;
	__u32 new_ctx;

	if (unlikely(!h_sched_ctx || !skey)) {
		WD_ERR("invalid: sched ctx or key is NULL!\n");
		return INVALID_POS;
	}

	if (sched_mode == SCHED_MODE_SYNC) {
		domain = &skey->sync_domain;
	} else {
		domain = &skey->async_domain;
	}

	/* Get current minimum load context */
	min_ctx = wd_sched_skey_pick_next(&domain->idx_cache, &ctx_idx);
	if (min_ctx == INVALID_POS)
		return INVALID_POS;

	return min_ctx;
}

static int instr_poll_policy_rr(struct wd_sched_ctx *sched_ctx, struct wd_sched_key *skey,
				__u32 expect, __u32 *count)
{
	__u32 recv_cnt, ctx_id;
	int ret;

	recv_cnt = 0;
	ctx_id = skey->async_domain.idx_cache.idx_list[0];
	ret = sched_ctx->poll_func(ctx_id, expect, &recv_cnt);
	if ((ret < 0) && (ret != -EAGAIN))
		return ret;
	*count += recv_cnt;

	return 0;
}

/**
 * instr_sched_poll_policy - Poll policy for instruction scheduler
 * @h_sched_ctx: Scheduler handle (cannot modify per API contract)
 * @expect: Expected number of responses (cannot modify per API contract)
 * @count: Actual response count (cannot modify per API contract)
 *
 * Returns: Status code
 */
static int instr_sched_poll_policy(handle_t h_sched_ctx, __u32 expect, __u32 *count)
{
	struct wd_sched_ctx *sched_ctx = (struct wd_sched_ctx *)h_sched_ctx;
	struct wd_sched_key *skey;
	int ret;

	if (unlikely(!count || !sched_ctx || !sched_ctx->poll_func)) {
		WD_ERR("invalid: sched ctx or poll_func is NULL or count is zero!\n");
		return -WD_EINVAL;
	}

	skey = sched_get_poll_skey(sched_ctx);
	if (!skey)
		return -WD_EAGAIN;

	ret = instr_poll_policy_rr(sched_ctx, skey, expect, count);
	if (unlikely(ret))
		return ret;

	return ret;
}

static handle_t session_dev_sched_init(handle_t h_sched_ctx, void *sched_param)
{
	struct wd_sched_ctx *sched_ctx = (struct wd_sched_ctx *)h_sched_ctx;
	struct sched_params *param = (struct sched_params *)sched_param;
	struct wd_sched_key *skey;
	handle_t hskey;
	int ret = 0;

	hskey = sched_session_common_init(sched_ctx, param);
	if (WD_IS_ERR(hskey)) {
		WD_ERR("failed to init session schedule key!\n");
		return hskey;
	}

	skey = (struct wd_sched_key *)hskey;
	skey->type = param->type;

	ret = session_sched_domain_init(sched_ctx, skey);
	if (ret != 0) {
		WD_ERR("failed to initialize session domains!\n");
		free(skey);
		return (handle_t)(-WD_EINVAL);
	}

	sched_skey_param_init(sched_ctx, skey);	
	WD_INFO("initialized Dev RR scheduler with sync and async domains\n");
	return (handle_t)skey;
}

/**
 * wd_sched_set_param - Set scheduler parameters
 * @h_sched_ctx: Scheduler handle (cannot modify per API contract)
 * @sched_key: Session key (cannot modify per API contract)
 * @sched_param: Scheduling parameters (cannot modify per API contract)
 */
static void wd_sched_set_param(handle_t h_sched_ctx,
			void *sched_key, void *sched_param)
{
	struct wd_sched_params *params = (struct wd_sched_params *)sched_param;
	struct wd_sched_key *skey = (struct wd_sched_key *)sched_key;

	skey->pkt_size = params->pkt_size;
	skey->is_stream = params->data_mode;
	skey->prio_mode = params->prio_mode;
}

static struct wd_sched sched_table[SCHED_POLICY_BUTT] = {
	{
		.name = "RR scheduler",
		.sched_policy = SCHED_POLICY_RR,
		.sched_init = round_robin_sched_init,
		.pick_next_ctx = round_robin_pick_next_ctx,
		.poll_policy = round_robin_poll_policy,
		.set_param = wd_sched_set_param,
	}, {
		.name = "None scheduler",
		.sched_policy = SCHED_POLICY_NONE,
		.sched_init = sched_none_init,
		.pick_next_ctx = sched_none_pick_next_ctx,
		.poll_policy = sched_none_poll_policy,
		.set_param = wd_sched_set_param,
	}, {
		.name = "Single scheduler",
		.sched_policy = SCHED_POLICY_SINGLE,
		.sched_init = sched_single_init,
		.pick_next_ctx = sched_single_pick_next_ctx,
		.poll_policy = sched_single_poll_policy,
		.set_param = wd_sched_set_param,
	}, {
		.name = "Device RR scheduler",
		.sched_policy = SCHED_POLICY_DEV,
		.sched_init = session_dev_sched_init,
		.pick_next_ctx = round_robin_pick_next_ctx,
		.poll_policy = round_robin_poll_policy,
		.set_param = wd_sched_set_param,
	}, {
		.name = "Loop scheduler",
		.sched_policy = SCHED_POLICY_LOOP,
		.sched_init = loop_sched_init,
		.pick_next_ctx = loop_sched_pick_next_ctx,
		.poll_policy = loop_sched_poll_policy,
		.set_param = wd_sched_set_param,

	}, {
		.name = "Hungry scheduler",
		.sched_policy = SCHED_POLICY_HUNGRY,
		.sched_init = skey_sched_init,
		.pick_next_ctx = skey_sched_pick_next_ctx,
		.poll_policy = skey_sched_poll_policy,
		.set_param = wd_sched_set_param,
	},  {
		.name = "Instr scheduler",
		.sched_policy = SCHED_POLICY_INSTR,
		.sched_init = instr_sched_init,
		.pick_next_ctx = instr_sched_pick_next_ctx,
		.poll_policy = instr_sched_poll_policy,
		.set_param = wd_sched_set_param,
	},
};

static int numa_num_check(__u16 region_num)
{
	int max_node;

	max_node = numa_max_node() + 1;
	if (max_node <= 0) {
		WD_ERR("invalid: numa max node is %d!\n", max_node);
		return -WD_EINVAL;
	}

	if (!region_num || region_num > max_node) {
		WD_ERR("invalid: region number is %u!\n", region_num);
		return -WD_EINVAL;
	}

	return 0;
}

/**
 * wd_sched_rr_instance - External API for scheduling region instance
 * @sched: Scheduler (cannot modify per API contract)
 * @param: Scheduling parameters (cannot modify per API contract)
 *
 * Creates scheduling region for given parameters.
 */
int wd_sched_rr_instance(const struct wd_sched *sched, struct sched_params *param)
{
	struct wd_sched_ctx *sched_ctx = NULL;
	struct wd_sched_ctx_domain *domain;
	__u8 mode;
	int ret;

	if (!sched || !sched->h_sched_ctx || !param) {
		WD_ERR("invalid: sched or sched_params is NULL!\n");
		return -WD_EINVAL;
	}

	if (param->begin > param->end) {
		WD_ERR("invalid: sched_params's begin is larger than end!\n");
		return -WD_EINVAL;
	}

	mode = param->mode;
	sched_ctx = (struct wd_sched_ctx *)sched->h_sched_ctx;

	if (param->numa_id >= sched_ctx->region_num || param->numa_id < 0) {
		WD_ERR("invalid: region_id is %d, region_num is %u!\n",
		       param->numa_id, sched_ctx->region_num);
		return -WD_EINVAL;
	}

	if (param->type >= sched_ctx->type_num) {
		WD_ERR("invalid: type is %u, type_num is %u!\n",
		       param->type, sched_ctx->type_num);
		return -WD_EINVAL;
	}

	if (mode >= SCHED_MODE_BUTT) {
		WD_ERR("invalid: mode is %u, mode_num is %u!\n",
		       mode, sched_ctx->mode_num);
		return -WD_EINVAL;
	}

	if (param->ctx_prop < 0 || param->ctx_prop > UADK_ALG_SOFT)
		param->ctx_prop = UADK_ALG_HW;

	/* Insert or get domain from hash table using four dimensions */
	domain = wd_sched_hash_table_insert(sched_ctx->domain_hash_table,
	                                    param->numa_id, mode, param->type,
	                                    param->ctx_prop);
	if (!domain)
		return -WD_ENOMEM;

	/* Add context range as new segment */
	ret = wd_sched_domain_add_segment(domain, param->begin, param->end);
	if (ret) {
		WD_ERR("failed to add segment to domain!\n");
		return ret;
	}
	domain->valid = true;

	WD_ERR("instance: region=%d, mode=%u, type=%u, prop=%d, begin=%u, end=%u\n",
		param->numa_id, mode, param->type, param->ctx_prop,
		param->begin, param->end);

	return WD_SUCCESS;
}

/**
 * wd_sched_rr_release - External API for scheduler release
 * @sched: Scheduler to release (cannot modify per API contract)
 *
 * Releases all scheduler resources.
 */
void wd_sched_rr_release(struct wd_sched *sched)
{
	struct wd_sched_ctx *sched_ctx;
	__u32 i;

	if (!sched)
		return;

	sched_ctx = (struct wd_sched_ctx *)sched->h_sched_ctx;
	if (!sched_ctx)
		goto ctx_out;

	/* Release all session keys */
	for (i = 0; i < sched_ctx->skey_num; i++) {
		if (sched_ctx->skey[i] != NULL) {
			wd_sched_skey_domain_destroy(&sched_ctx->skey[i]->sync_domain);
			wd_sched_skey_domain_destroy(&sched_ctx->skey[i]->async_domain);
		}
		sched_ctx->skey[i] = NULL;
	}
	sched_ctx->skey_num = 0;

	/* Release hash table */
	if (sched_ctx->domain_hash_table) {
		wd_sched_hash_table_destroy(sched_ctx->domain_hash_table);
		sched_ctx->domain_hash_table = NULL;
	}

	free(sched_ctx);

ctx_out:
	free(sched);
	return;
}

/**
 * wd_sched_rr_alloc - External API for scheduler allocation
 * @sched_type: Scheduling policy type (cannot modify per API contract)
 * @type_num: Number of operation types (cannot modify per API contract)
 * @region_num: Number of regions (cannot modify per API contract)
 * @func: Poll function (cannot modify per API contract)
 *
 * Allocates and initializes scheduler with single global hash table.
 */
struct wd_sched *wd_sched_rr_alloc(__u8 sched_type, __u8 type_num,
				   __u16 region_num, user_poll_func func)
{
	struct wd_sched_ctx *sched_ctx;
	struct wd_sched *sched;
	__u32 estimated_entries;
	__u32 i;

	if (sched_type >= SCHED_POLICY_BUTT || !type_num) {
		WD_ERR("invalid: sched_type is %u or type_num is %u!\n",
		       sched_type, type_num);
		return NULL;
	}

	sched = calloc(1, sizeof(struct wd_sched));
	if (!sched) {
		WD_ERR("failed to alloc memory for wd_sched!\n");
		return NULL;
	}

	sched_ctx = calloc(1, sizeof(struct wd_sched_ctx));
	if (!sched_ctx) {
		WD_ERR("failed to alloc memory for sched_ctx!\n");
		goto err_out;
	}

	/* Cache dimension parameters */
	sched_ctx->type_num = type_num;
	sched_ctx->mode_num = SCHED_MODE_BUTT;
	sched_ctx->region_num = region_num;
	sched_ctx->policy = sched_type;

	if (sched_type == SCHED_POLICY_NONE || sched_type == SCHED_POLICY_SINGLE) {
		/* Simple schedulers don't need hash table */
		goto simple_ok;
	}

	if (sched_type == SCHED_POLICY_DEV) {
		/* Device mode: region_num is actually device count */
		estimated_entries = region_num * type_num * SCHED_MODE_BUTT * UADK_ALG_TYPE_MAX;
	} else {
		/* NUMA mode: validate region_num */
		if (numa_num_check(region_num))
			goto err_out;
		estimated_entries = region_num * type_num * SCHED_MODE_BUTT * UADK_ALG_TYPE_MAX;
	}

	/* Create single global hash table */
	sched_ctx->domain_hash_table = wd_sched_hash_table_create(estimated_entries);
	if (!sched_ctx->domain_hash_table) {
		WD_ERR("failed to create hash table!\n");
		goto ctx_out;
	}

simple_ok:
	sched_ctx->poll_func = func;

	for (i = 0; i < SKEY_MAX_THREAD_NUM; i++) {
		sched_ctx->skey[i] = NULL;
		sched_ctx->poll_tid[i] = 0;
	}
	pthread_mutex_init(&sched_ctx->skey_lock, NULL);
	sched_ctx->skey_num = 0;

	sched->h_sched_ctx = (handle_t)sched_ctx;
	sched->sched_init = sched_table[sched_type].sched_init;
	sched->pick_next_ctx = sched_table[sched_type].pick_next_ctx;
	sched->poll_policy = sched_table[sched_type].poll_policy;
	sched->sched_policy = sched_type;
	sched->name = sched_table[sched_type].name;
	sched->set_param = sched_table[sched_type].set_param;

	WD_INFO("Scheduler %s allocated: type_num=%u, region_num=%u, mode_num=%d\n",
		sched->name, type_num, region_num, SCHED_MODE_BUTT);

	return sched;

ctx_out:
	free(sched_ctx);
err_out:
	free(sched);
	return NULL;
}
