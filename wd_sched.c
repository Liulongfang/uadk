// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2020-2021 Huawei Technologies Co.,Ltd. All rights reserved.
 * Copyright 2020-2021 Linaro ltd.
 *
 * Scheduler V7: Simplified Pure Hash Table with Dynamic Context Expansion
 * 
 * Key improvements:
 * - Single global hash table with (region_id, mode, op_type, prop) dimensions
 * - Segment list for non-contiguous ctx ranges
 * - Dual-domain min-heap for session key
 * - Dynamic ctx expansion in HUNGRY mode based on load threshold
 * - Simplified sched_init: only allocate one sync + one async ctx
 * - Removed redundant wd_sched_info layer
 * - Removed dev_id_map
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

#define MAX_POLL_TIMES 1000
#define HUNGRY_LOAD_THRESHOLD 512
#define HUNGRY_MAX_CTX_PER_DOMAIN 4

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
 * Dual-Domain Min-Heap Index Cache
 * ============================================================================
 */

/**
 * wd_sched_domain_idx_cache - Index cache for single domain with min-heap
 * @idx_list: Sorted list of all valid ctx indices in this domain
 * @idx_count: Number of valid ctx indices
 * @min_heap: Min-heap structure for load-based optimal selection
 *   - heap: Binary heap array ordered by load values
 *   - heap_pos: Reverse index mapping ctx -> position in heap
 *   - max_idx: Pre-allocated maximum index range
 * @load_values: Current load value for each ctx index
 * @poll_ptr: Round-robin polling pointer [0..idx_count-1]
 * @version: Data version number for cache validation
 * @lock: Synchronization spinlock
 *
 * Solution for session ctx organization with load awareness.
 * Supports two query modes:
 * - Round-robin: get_next_round_robin() - O(1)
 * - Optimal: get_min_load_ctx() - O(1) via min-heap
 */
struct wd_sched_domain_idx_cache {
	__u32 *idx_list;
	__u32 idx_count;
	
	struct {
		__u32 *heap;
		__u32 *heap_pos;
		__u32 max_idx;
	} min_heap;
	
	__u32 *load_values;
	__u32 poll_ptr;
	
	__u64 version;
	
	pthread_spinlock_t lock;
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
 * @poll_count: Total poll operations
 * @ctx_switch_count: Total context switches
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
	
	__u64 poll_count;
	__u64 ctx_switch_count;
	
	pthread_spinlock_t lock;
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
 * @collision_count: Number of collisions
 * @max_chain_length: Maximum chain length for statistics
 * @lock: Read-write lock for concurrent access
 */
struct wd_sched_domain_hash_table {
	struct wd_sched_domain_hash_node **buckets;
	__u32 bucket_size;
	
	__u32 entry_count;
	__u32 collision_count;
	__u32 max_chain_length;
	
	pthread_rwlock_t lock;
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
	pthread_spinlock_t lock;
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
	
	pthread_spinlock_t lock;
};

#define LOOP_SWITCH_STEP	1
#define LOOP_SWITCH_SLICE	10
#define UADK_SWITCH_PKT_SZ	2048

#define MAX_SKEY_REGION_NUM	64
#define MAX_NUMA_NODES		(NUMA_NUM_NODES >> 5)

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
	struct wd_sched_key *skey[MAX_SKEY_REGION_NUM];
	__u32 poll_tid[MAX_SKEY_REGION_NUM];
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
	table->collision_count = 0;
	table->max_chain_length = 0;

	ret = pthread_rwlock_init(&table->lock, NULL);
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
			
			pthread_spinlock_destroy(&node->domain.lock);
			free(node);
			node = next;
		}
	}

	pthread_rwlock_destroy(&table->lock);
	free(table->buckets);
	free(table);
}

static struct wd_sched_ctx_domain *
wd_sched_hash_table_lookup(struct wd_sched_domain_hash_table *table,
                           int region_id, __u8 mode, __u32 op_type, __u8 prop)
{
	__u32 hash_idx;
	struct wd_sched_domain_hash_node *node;
	struct wd_sched_ctx_domain *domain = NULL;

	if (!table)
		return NULL;

	hash_idx = wd_sched_hash_compute(region_id, mode, op_type, prop, table->bucket_size);

	pthread_rwlock_rdlock(&table->lock);

	node = table->buckets[hash_idx];
	while (node) {
		if (wd_sched_domain_key_match(
			node->domain.region_id, node->domain.mode, node->domain.op_type, node->domain.prop,
			region_id, mode, op_type, prop)) {
			domain = &node->domain;
			break;
		}
		node = node->next;
	}

	pthread_rwlock_unlock(&table->lock);

	return domain;
}

static struct wd_sched_ctx_domain *
wd_sched_hash_table_insert(struct wd_sched_domain_hash_table *table,
                           int region_id, __u8 mode, __u32 op_type, __u8 prop)
{
	__u32 hash_idx;
	struct wd_sched_domain_hash_node *node, *new_node;
	struct wd_sched_ctx_domain *existing;
	__u32 chain_length;
	int ret;

	if (!table)
		return NULL;

	existing = wd_sched_hash_table_lookup(table, region_id, mode, op_type, prop);
	if (existing)
		return existing;

	hash_idx = wd_sched_hash_compute(region_id, mode, op_type, prop, table->bucket_size);

	new_node = calloc(1, sizeof(*new_node));
	if (!new_node)
		return NULL;

	pthread_rwlock_wrlock(&table->lock);

	node = table->buckets[hash_idx];
	chain_length = 0;
	while (node) {
		chain_length++;
		if (wd_sched_domain_key_match(
			node->domain.region_id, node->domain.mode, node->domain.op_type, node->domain.prop,
			region_id, mode, op_type, prop)) {
			pthread_rwlock_unlock(&table->lock);
			free(new_node);
			return &node->domain;
		}
		node = node->next;
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
	new_node->domain.poll_count = 0;
	new_node->domain.ctx_switch_count = 0;

	ret = pthread_spinlock_init(&new_node->domain.lock, PTHREAD_PROCESS_PRIVATE);
	if (ret) {
		pthread_rwlock_unlock(&table->lock);
		free(new_node);
		return NULL;
	}

	new_node->next = table->buckets[hash_idx];
	table->buckets[hash_idx] = new_node;

	table->entry_count++;
	if (new_node->next) {
		table->collision_count++;
		chain_length++;
		if (chain_length > table->max_chain_length)
			table->max_chain_length = chain_length;
	}

	pthread_rwlock_unlock(&table->lock);

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

	pthread_spinlock_lock(&domain->lock);

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

	pthread_spinlock_unlock(&domain->lock);

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
	__u32 pos;

	if (!domain || !domain->segments || domain->total_ctx_count == 0)
		return INVALID_POS;

	pthread_spinlock_lock(&domain->lock);

	if (!domain->current_segment)
		domain->current_segment = domain->segments;

	pos = domain->current_pos;

	/* Move to next position */
	if (pos < domain->current_segment->end) {
		domain->current_pos++;
	} else {
		/* Move to next segment */
		if (domain->current_segment->next) {
			domain->current_segment = domain->current_segment->next;
			domain->current_pos = domain->current_segment->begin;
			pos = domain->current_segment->begin;
			domain->current_pos++;
		} else {
			/* Loop back to beginning */
			domain->current_segment = domain->segments;
			domain->current_pos = domain->segments->begin + 1;
			pos = domain->segments->begin;
		}
	}

	domain->ctx_switch_count++;
	domain->poll_count++;

	pthread_spinlock_unlock(&domain->lock);

	return pos;
}

/* ============================================================================
 * Min-Heap Operations
 * ============================================================================
 */

/**
 * _min_heap_up - Bubble up element in min-heap
 */
static void _min_heap_up(struct wd_sched_domain_idx_cache *cache, __u32 pos)
{
	__u32 parent_pos, temp_idx;

	while (pos > 0) {
		parent_pos = (pos - 1) >> 1;
		
		if (cache->load_values[cache->min_heap.heap[pos]] < 
		    cache->load_values[cache->min_heap.heap[parent_pos]]) {
			/* Swap */
			temp_idx = cache->min_heap.heap[pos];
			cache->min_heap.heap[pos] = cache->min_heap.heap[parent_pos];
			cache->min_heap.heap[parent_pos] = temp_idx;
			
			/* Update heap_pos */
			cache->min_heap.heap_pos[cache->min_heap.heap[pos]] = pos;
			cache->min_heap.heap_pos[cache->min_heap.heap[parent_pos]] = parent_pos;
			
			pos = parent_pos;
		} else {
			break;
		}
	}
}

/**
 * _min_heap_down - Bubble down element in min-heap
 */
static void _min_heap_down(struct wd_sched_domain_idx_cache *cache, __u32 pos)
{
	__u32 smallest, left, right, temp_idx;

	while (true) {
		smallest = pos;
		left = (pos << 1) + 1;
		right = (pos << 1) + 2;
		
		if (left < cache->idx_count &&
		    cache->load_values[cache->min_heap.heap[left]] < 
		    cache->load_values[cache->min_heap.heap[smallest]]) {
			smallest = left;
		}
		
		if (right < cache->idx_count &&
		    cache->load_values[cache->min_heap.heap[right]] < 
		    cache->load_values[cache->min_heap.heap[smallest]]) {
			smallest = right;
		}
		
		if (smallest != pos) {
			/* Swap */
			temp_idx = cache->min_heap.heap[pos];
			cache->min_heap.heap[pos] = cache->min_heap.heap[smallest];
			cache->min_heap.heap[smallest] = temp_idx;
			
			/* Update heap_pos */
			cache->min_heap.heap_pos[cache->min_heap.heap[pos]] = pos;
			cache->min_heap.heap_pos[cache->min_heap.heap[smallest]] = smallest;
			
			pos = smallest;
		} else {
			break;
		}
	}
}

/**
 * wd_sched_domain_idx_cache_init - Initialize index cache with min-heap
 * @cache: Cache to initialize
 * @idx_list: Array of context indices
 * @idx_count: Number of indices
 * @max_idx: Maximum index value for allocation
 *
 * Builds min-heap in O(n) time during initialization.
 */
static int wd_sched_domain_idx_cache_init(struct wd_sched_domain_idx_cache *cache,
                                          __u32 *idx_list, __u32 idx_count,
                                          __u32 max_idx)
{
	__u32 i;
	int ret;

	if (!cache || !idx_list || idx_count == 0)
		return -WD_EINVAL;

	/* Pre-allocate idx_list */
	cache->idx_list = calloc(idx_count, sizeof(__u32));
	if (!cache->idx_list)
		return -WD_ENOMEM;

	memcpy(cache->idx_list, idx_list, idx_count * sizeof(__u32));
	cache->idx_count = idx_count;

	/* Pre-allocate min-heap */
	cache->min_heap.heap = calloc(idx_count, sizeof(__u32));
	if (!cache->min_heap.heap) {
		free(cache->idx_list);
		return -WD_ENOMEM;
	}

	/* Pre-allocate heap_pos and load_values */
	cache->min_heap.heap_pos = calloc(max_idx, sizeof(__u32));
	if (!cache->min_heap.heap_pos) {
		free(cache->idx_list);
		free(cache->min_heap.heap);
		return -WD_ENOMEM;
	}

	cache->load_values = calloc(max_idx, sizeof(__u32));
	if (!cache->load_values) {
		free(cache->idx_list);
		free(cache->min_heap.heap);
		free(cache->min_heap.heap_pos);
		return -WD_ENOMEM;
	}

	cache->min_heap.max_idx = max_idx;

	/* Initialize heap from index list */
	for (i = 0; i < idx_count; i++)
		cache->min_heap.heap[i] = cache->idx_list[i];

	/* Heapify: O(n) */
	for (i = (idx_count - 2) >> 1; i >= 0; i--) {
		_min_heap_down(cache, i);
	}

	/* Initialize heap_pos reverse index */
	for (i = 0; i < idx_count; i++) {
		__u32 idx = cache->min_heap.heap[i];
		if (idx < max_idx)
			cache->min_heap.heap_pos[idx] = i;
	}

	cache->poll_ptr = 0;
	cache->version = 1;

	ret = pthread_spinlock_init(&cache->lock, PTHREAD_PROCESS_PRIVATE);
	if (ret) {
		free(cache->idx_list);
		free(cache->min_heap.heap);
		free(cache->min_heap.heap_pos);
		free(cache->load_values);
		return ret;
	}

	return 0;
}

/**
 * wd_sched_domain_idx_cache_destroy - Release index cache resources
 */
static void wd_sched_domain_idx_cache_destroy(struct wd_sched_domain_idx_cache *cache)
{
	if (!cache)
		return;

	pthread_spinlock_destroy(&cache->lock);
	free(cache->idx_list);
	free(cache->min_heap.heap);
	free(cache->min_heap.heap_pos);
	free(cache->load_values);
}

/**
 * wd_sched_domain_idx_cache_get_min_load - Get context with minimum load
 * @cache: Source cache
 *
 * Returns: Context index with minimum load
 * Time complexity: O(1)
 */
static inline __u32 wd_sched_domain_idx_cache_get_min_load(
    struct wd_sched_domain_idx_cache *cache)
{
	if (!cache || cache->idx_count == 0)
		return INVALID_POS;

	return cache->min_heap.heap[0];
}

/**
 * wd_sched_domain_idx_cache_get_next_rr - Get next context via round-robin
 * @cache: Source cache
 *
 * Returns: Next context index
 * Time complexity: O(1)
 */
static __u32 wd_sched_domain_idx_cache_get_next_rr(
    struct wd_sched_domain_idx_cache *cache)
{
	__u32 idx;

	if (!cache || cache->idx_count == 0)
		return INVALID_POS;

	pthread_spinlock_lock(&cache->lock);

	idx = cache->idx_list[cache->poll_ptr];
	cache->poll_ptr++;
	if (cache->poll_ptr >= cache->idx_count)
		cache->poll_ptr = 0;

	pthread_spinlock_unlock(&cache->lock);

	return idx;
}

/**
 * wd_sched_domain_idx_cache_update_load - Update context load value
 * @cache: Target cache
 * @idx: Context index
 * @new_load: New load value
 *
 * Updates load and rebalances heap. Time complexity: O(log n)
 */
static void wd_sched_domain_idx_cache_update_load(
    struct wd_sched_domain_idx_cache *cache,
    __u32 idx, __u32 new_load)
{
	__u32 pos;

	if (!cache || idx >= cache->min_heap.max_idx)
		return;

	pthread_spinlock_lock(&cache->lock);

	cache->load_values[idx] = new_load;

	/* Get position of this index in heap */
	pos = cache->min_heap.heap_pos[idx];

	/* Rebalance heap */
	if (pos > 0 && cache->load_values[cache->min_heap.heap[pos]] <
	               cache->load_values[cache->min_heap.heap[(pos - 1) >> 1]]) {
		_min_heap_up(cache, pos);
	} else {
		_min_heap_down(cache, pos);
	}

	cache->version++;

	pthread_spinlock_unlock(&cache->lock);
}

/**
 * wd_sched_domain_idx_cache_add_ctx - Add context to cache for HUNGRY scheduler
 * @cache: Target cache
 * @ctx_id: Context ID to add
 *
 * Dynamically expands cache when load threshold exceeded.
 * Time complexity: O(n) where n is current cache size (realloc + rebuild heap)
 */
static int wd_sched_domain_idx_cache_add_ctx(
    struct wd_sched_domain_idx_cache *cache, __u32 ctx_id)
{
	__u32 *new_idx_list, *new_heap;
	__u32 i;

	if (!cache || cache->idx_count >= HUNGRY_MAX_CTX_PER_DOMAIN)
		return -WD_EINVAL;

	/* Reallocate idx_list */
	new_idx_list = realloc(cache->idx_list, 
	                        (cache->idx_count + 1) * sizeof(__u32));
	if (!new_idx_list)
		return -WD_ENOMEM;
	cache->idx_list = new_idx_list;

	/* Reallocate heap */
	new_heap = realloc(cache->min_heap.heap,
	                   (cache->idx_count + 1) * sizeof(__u32));
	if (!new_heap)
		return -WD_ENOMEM;
	cache->min_heap.heap = new_heap;

	/* Add new context */
	cache->idx_list[cache->idx_count] = ctx_id;
	cache->min_heap.heap[cache->idx_count] = ctx_id;
	cache->min_heap.heap_pos[ctx_id] = cache->idx_count;
	cache->load_values[ctx_id] = 0;
	cache->idx_count++;

	/* Rebuild heap */
	for (i = (cache->idx_count - 2) >> 1; i >= 0; i--) {
		_min_heap_down(cache, i);
	}

	/* Reinit heap_pos */
	for (i = 0; i < cache->idx_count; i++) {
		__u32 idx = cache->min_heap.heap[i];
		cache->min_heap.heap_pos[idx] = i;
	}

	cache->version++;

	return 0;
}

/* ============================================================================
 * Session Key Domain Initialization
 * ============================================================================
 */

/**
 * wd_sched_key_domain_init - Initialize session domain with min-heap
 * @key_domain: Target key domain
 * @ctx_list: Array of context indices
 * @ctx_count: Number of contexts
 * @max_idx: Maximum index for allocation
 *
 * Initializes dual-domain structure for session.
 */
static int wd_sched_key_domain_init(struct wd_sched_key_domain *key_domain,
                                    __u32 *ctx_list, __u32 ctx_count,
                                    __u32 max_idx)
{
	int ret;

	if (!key_domain || !ctx_list || ctx_count == 0)
		return -WD_EINVAL;

	ret = wd_sched_domain_idx_cache_init(&key_domain->idx_cache,
	                                      ctx_list, ctx_count, max_idx);
	if (ret)
		return ret;

	ret = pthread_spinlock_init(&key_domain->lock, PTHREAD_PROCESS_PRIVATE);
	if (ret) {
		wd_sched_domain_idx_cache_destroy(&key_domain->idx_cache);
		return ret;
	}

	key_domain->expanded_count = 0;

	return 0;
}

/**
 * wd_sched_key_domain_destroy - Release session domain resources
 */
static void wd_sched_key_domain_destroy(struct wd_sched_key_domain *key_domain)
{
	if (!key_domain)
		return;

	pthread_spinlock_destroy(&key_domain->lock);
	wd_sched_domain_idx_cache_destroy(&key_domain->idx_cache);
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
	for (i = 0; i < MAX_SKEY_REGION_NUM; i++) {
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

static struct wd_sched_key *sched_get_poll_skey(struct wd_sched_ctx *sched_ctx)
{
	__u32 tid = pthread_self();
	__u16 i, tidx = 0;

	delay_us(tid % 17);

	for (i = 0; i < sched_ctx->skey_num; i++) {
		if (sched_ctx->poll_tid[i] == tid) {
			tidx = i;
			break;
		} else if (sched_ctx->poll_tid[i] == 0) {
			pthread_mutex_lock(&sched_ctx->skey_lock);
			if (sched_ctx->poll_tid[i] == 0) {
				sched_ctx->poll_tid[i] = tid;
				tidx = i;
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

static bool sched_key_valid(struct wd_sched_ctx *sched_ctx, const struct wd_sched_key *key)
{
	if (key->region_id >= sched_ctx->region_num || key->mode >= SCHED_MODE_BUTT ||
	    key->type >= sched_ctx->type_num) {
		WD_ERR("invalid: sched key's region: %d, mode: %u, type: %u!\n",
		       key->region_id, key->mode, key->type);
		return false;
	}

	return true;
}

/**
 * sched_get_domain_from_table - Query scheduling domain from hash table
 * @sched_ctx: Scheduler context
 * @region_id: Region identifier
 * @mode: Context mode
 * @op_type: Operation type
 * @prop: Property
 *
 * Returns: Scheduling domain or NULL if not found
 */
static struct wd_sched_ctx_domain *
sched_get_domain_from_table(struct wd_sched_ctx *sched_ctx,
                            int region_id, __u8 mode, __u32 op_type, __u8 prop)
{
	if (!sched_ctx->domain_hash_table)
		return NULL;

	return wd_sched_hash_table_lookup(sched_ctx->domain_hash_table,
	                                  region_id, mode, op_type, prop);
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
	    op_type >= sched_ctx->type_num) {
		WD_ERR("invalid: region: %d, mode: %u, type: %u!\n",
		       region_id, sched_mode, op_type);
		return INVALID_POS;
	}

	domain = sched_get_domain_from_table(sched_ctx, region_id, sched_mode, op_type, prop);
	if (!domain)
		return INVALID_POS;

	return wd_sched_domain_get_next_rr(domain);
}

/* ============================================================================
 * Scheduler Policy Functions
 * ============================================================================
 */

/**
 * session_sched_init - Initialize session with single sync and async ctx
 * @h_sched_ctx: Scheduler handle (cannot modify per API contract)
 * @sched_param: Scheduling parameters (cannot modify per API contract)
 *
 * Allocates session key and pre-fetches one sync and one async context.
 */
static handle_t session_sched_init(handle_t h_sched_ctx, void *sched_param)
{
	struct wd_sched_ctx *sched_ctx = (struct wd_sched_ctx *)h_sched_ctx;
	struct sched_params *param = (struct sched_params *)sched_param;
	struct wd_sched_key *skey;
	unsigned int node;
	__u32 sync_ctx, async_ctx;
	__u32 max_idx;
	int region_id = 0;

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
		region_id = 0;
		if (wd_need_debug())
			WD_DEBUG("session don't set scheduler parameters!\n");
	} else {
		if (param->numa_id >= 0) {
			region_id = param->numa_id;
		} else {
			region_id = 0;
		}
		skey->type = param->type;
		skey->ctx_prop = param->ctx_prop;
	}

	skey->region_id = region_id;
	max_idx = sched_ctx->region_num ? sched_ctx->region_num : 256;

	/* Pre-fetch one sync context with default prop */
	sync_ctx = session_sched_init_ctx(sched_ctx, region_id, skey->type, 
	                                  UADK_CTX_HW, SCHED_MODE_SYNC);
	
	/* Pre-fetch one async context with default prop */
	async_ctx = session_sched_init_ctx(sched_ctx, region_id, skey->type,
	                                   UADK_CTX_HW, SCHED_MODE_ASYNC);

	if (sync_ctx == INVALID_POS && async_ctx == INVALID_POS) {
		WD_ERR("failed to get valid sync_ctx or async_ctx!\n");
		goto out;
	}

	WD_DEBUG("Got sync_ctx=%u, async_ctx=%u\n", sync_ctx, async_ctx);

	/* Initialize dual-domain for this session */
	if (sync_ctx != INVALID_POS) {
		if (wd_sched_key_domain_init(&skey->sync_domain, &sync_ctx, 1, max_idx) != 0) {
			WD_ERR("failed to init sync domain!\n");
			goto out;
		}
	}

	if (async_ctx != INVALID_POS) {
		if (wd_sched_key_domain_init(&skey->async_domain, &async_ctx, 1, max_idx) != 0) {
			WD_ERR("failed to init async domain!\n");
			goto out;
		}
	}

	return (handle_t)skey;

out:
	free(skey);
	return (handle_t)(-WD_EINVAL);
}

/**
 * session_sched_pick_next_ctx - Pick context with load-based selection
 * @h_sched_ctx: Scheduler handle (cannot modify per API contract)
 * @sched_key: Session key (cannot modify per API contract)
 * @sched_mode: Mode (cannot modify per API contract)
 *
 * Returns: Context index with minimum load
 * Time complexity: O(1)
 */
static __u32 session_sched_pick_next_ctx(handle_t h_sched_ctx, void *sched_key,
					 const int sched_mode)
{
	struct wd_sched_key *key = (struct wd_sched_key *)sched_key;
	struct wd_sched_key_domain *domain;

	if (unlikely(!h_sched_ctx || !key)) {
		WD_ERR("invalid: sched ctx or key is NULL!\n");
		return INVALID_POS;
	}

	if (sched_mode == SCHED_MODE_SYNC) {
		domain = &key->sync_domain;
	} else {
		domain = &key->async_domain;
	}

	return wd_sched_domain_idx_cache_get_min_load(&domain->idx_cache);
}

static int session_poll_segment(struct wd_sched_ctx *sched_ctx,
                               struct wd_sched_ctx_segment *seg,
                               __u32 expect, __u32 *count)
{
	__u32 poll_num = 0;
	__u32 i;
	int ret;

	for (i = seg->begin; i <= seg->end; i++) {
		ret = sched_ctx->poll_func(i, 1, &poll_num);
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
 * session_poll_policy_rr - Poll all async domains
 * @sched_ctx: Scheduler context
 * @expect: Expected number of responses
 * @count: Actual response count (output)
 *
 * Traverses all domains using segment lists.
 */
static int session_poll_policy_rr(struct wd_sched_ctx *sched_ctx,
				  __u32 expect, __u32 *count)
{
	struct wd_sched_domain_hash_table *hash_table;
	struct wd_sched_domain_hash_node *node;
	struct wd_sched_ctx_segment *seg;
	__u32 i;
	int ret;

	hash_table = sched_ctx->domain_hash_table;
	if (!hash_table)
		return 0;

	pthread_rwlock_rdlock(&hash_table->lock);

	/* Traverse all domains in hash table */
	for (i = 0; i < hash_table->bucket_size; i++) {
		node = hash_table->buckets[i];
		while (node) {
			if (node->domain.valid && node->domain.mode == SCHED_MODE_ASYNC) {
				/* Traverse all segments in domain */
				seg = node->domain.segments;
				while (seg) {
					pthread_rwlock_unlock(&hash_table->lock);
					ret = session_poll_segment(sched_ctx, seg, expect, count);
					pthread_rwlock_rdlock(&hash_table->lock);
					
					if (unlikely(ret)) {
						pthread_rwlock_unlock(&hash_table->lock);
						return ret;
					}
					
					if (*count >= expect) {
						pthread_rwlock_unlock(&hash_table->lock);
						return 0;
					}
					
					seg = seg->next;
				}
			}
			node = node->next;
		}
	}

	pthread_rwlock_unlock(&hash_table->lock);
	return 0;
}

/**
 * session_sched_poll_policy - Poll policy for session scheduler
 * @h_sched_ctx: Scheduler handle (cannot modify per API contract)
 * @expect: Expected number of responses (cannot modify per API contract)
 * @count: Actual response count (cannot modify per API contract)
 *
 * Returns: Status code
 */
static int session_sched_poll_policy(handle_t h_sched_ctx, __u32 expect, __u32 *count)
{
	struct wd_sched_ctx *sched_ctx = (struct wd_sched_ctx *)h_sched_ctx;
	__u32 loop_time = 0;
	__u32 last_count = 0;
	int ret;

	if (unlikely(!count || !sched_ctx || !sched_ctx->poll_func)) {
		WD_ERR("invalid: sched ctx or poll_func is NULL or count is zero!\n");
		return -WD_EINVAL;
	}

	while (++loop_time < MAX_POLL_TIMES) {
		last_count = *count;
		ret = session_poll_policy_rr(sched_ctx, expect, count);
		if (unlikely(ret))
			return ret;

		if (expect == *count)
			return 0;

		if (last_count == *count)
			break;
	}

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
	int cpu = sched_getcpu();
	int node = numa_node_of_cpu(cpu);
	struct wd_sched_key *skey;
	__u32 sync_ctx, async_ctx;
	__u32 max_idx;
	int region_id = 0;

	if (node < 0) {
		WD_ERR("invalid: failed to get numa node!\n");
		return (handle_t)(-WD_EINVAL);
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
		region_id = 0;
		skey->ctx_prop = UADK_CTX_HW;
		WD_INFO("loop don't set scheduler parameters!\n");
	} else if (param->numa_id < 0) {
		region_id = 0;
		skey->type = param->type;
		skey->ctx_prop = param->ctx_prop;
	} else {
		region_id = param->numa_id;
		skey->type = param->type;
		skey->ctx_prop = param->ctx_prop;
	}

	skey->region_id = region_id;
	max_idx = sched_ctx->region_num ? sched_ctx->region_num : 256;

	/* Pre-fetch one sync context */
	sync_ctx = session_sched_init_ctx(sched_ctx, region_id, skey->type,
	                                  skey->ctx_prop, SCHED_MODE_SYNC);
	
	/* Pre-fetch one async context */
	async_ctx = session_sched_init_ctx(sched_ctx, region_id, skey->type,
	                                   skey->ctx_prop, SCHED_MODE_ASYNC);

	if (sync_ctx == INVALID_POS && async_ctx == INVALID_POS) {
		WD_ERR("failed to get valid sync_ctx or async_ctx!\n");
		goto out;
	}

	WD_ERR("Got sync_ctx=%u, async_ctx=%u\n", sync_ctx, async_ctx);

	if (sync_ctx != INVALID_POS) {
		if (wd_sched_key_domain_init(&skey->sync_domain, &sync_ctx, 1, max_idx) != 0) {
			WD_ERR("failed to init sync domain!\n");
			goto out;
		}
	}

	if (async_ctx != INVALID_POS) {
		if (wd_sched_key_domain_init(&skey->async_domain, &async_ctx, 1, max_idx) != 0) {
			WD_ERR("failed to init async domain!\n");
			goto out;
		}
	}

	return (handle_t)skey;

out:
	free(skey);
	return (handle_t)(-WD_EINVAL);
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
	struct wd_sched_key *key = (struct wd_sched_key *)sched_key;

	if (unlikely(!h_sched_ctx || !key)) {
		WD_ERR("invalid: sched ctx or key is NULL!\n");
		return INVALID_POS;
	}

	if (sched_mode == SCHED_MODE_SYNC)
		return wd_sched_domain_idx_cache_get_min_load(&key->sync_domain.idx_cache);
	else
		return wd_sched_domain_idx_cache_get_min_load(&key->async_domain.idx_cache);
}

static int loop_sched_poll_policy(handle_t h_sched_ctx, __u32 expect, __u32 *count)
{
	return session_sched_poll_policy(h_sched_ctx, expect, count);
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
	int cpu = sched_getcpu();
	int node = numa_node_of_cpu(cpu);
	struct wd_sched_key *skey;
	__u32 sync_ctx, async_ctx;
	__u32 max_idx;
	int region_id = 0;

	if (node < 0) {
		WD_ERR("invalid: failed to get numa node!\n");
		return (handle_t)(-WD_EINVAL);
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
		region_id = 0;
		skey->ctx_prop = UADK_CTX_HW;
		WD_INFO("hungry don't set scheduler parameters!\n");
	} else if (param->numa_id < 0) {
		region_id = 0;
		skey->type = param->type;
		skey->ctx_prop = param->ctx_prop;
	} else {
		region_id = param->numa_id;
		skey->type = param->type;
		skey->ctx_prop = param->ctx_prop;
	}

	skey->region_id = region_id;
	max_idx = sched_ctx->region_num ? sched_ctx->region_num : 256;

	/* Pre-fetch one sync context */
	sync_ctx = session_sched_init_ctx(sched_ctx, region_id, skey->type,
	                                  skey->ctx_prop, SCHED_MODE_SYNC);
	
	/* Pre-fetch one async context */
	async_ctx = session_sched_init_ctx(sched_ctx, region_id, skey->type,
	                                   skey->ctx_prop, SCHED_MODE_ASYNC);

	if (sync_ctx == INVALID_POS && async_ctx == INVALID_POS) {
		WD_ERR("failed to get valid sync_ctx or async_ctx!\n");
		goto out;
	}

	if (sync_ctx != INVALID_POS) {
		if (wd_sched_key_domain_init(&skey->sync_domain, &sync_ctx, 1, max_idx) != 0) {
			WD_ERR("failed to init sync domain!\n");
			goto out;
		}
	}

	if (async_ctx != INVALID_POS) {
		if (wd_sched_key_domain_init(&skey->async_domain, &async_ctx, 1, max_idx) != 0) {
			WD_ERR("failed to init async domain!\n");
			goto out;
		}
	}

	sched_skey_param_init(sched_ctx, skey);
	WD_ERR("initialized hungry scheduler with sync and async domains\n");

	return (handle_t)skey;

out:
	free(skey);
	return (handle_t)(-WD_EINVAL);
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
	__u32 min_ctx, min_load;
	__u32 new_ctx;
	struct wd_sched_ctx_domain *domain_v7;

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
	min_ctx = wd_sched_domain_idx_cache_get_min_load(&domain->idx_cache);
	if (min_ctx == INVALID_POS)
		return INVALID_POS;

	min_load = domain->idx_cache.load_values[min_ctx];

	/* Check if we need to expand context pool */
	if (min_load > HUNGRY_LOAD_THRESHOLD &&
	    domain->idx_cache.idx_count < HUNGRY_MAX_CTX_PER_DOMAIN &&
	    domain->expanded_count < HUNGRY_MAX_CTX_PER_DOMAIN) {
		
		/* Try to allocate new context from domain */
		domain_v7 = sched_get_domain_from_table(sched_ctx,
		                                        skey->region_id, sched_mode,
		                                        skey->type, skey->ctx_prop);
		if (domain_v7) {
			new_ctx = wd_sched_domain_get_next_rr(domain_v7);
			if (new_ctx != INVALID_POS) {
				if (wd_sched_domain_idx_cache_add_ctx(&domain->idx_cache, new_ctx) == 0) {
					domain->expanded_count++;
					WD_DEBUG("Expanded %s domain: added ctx %u (total=%u)\n",
						sched_mode == SCHED_MODE_SYNC ? "sync" : "async",
						new_ctx, domain->idx_cache.idx_count);
				}
			}
		}
	}

	return min_ctx;
}

/**
 * skey_poll_ctx - Poll contexts for hungry scheduler
 * @sched_ctx: Scheduler context
 * @skey: Session key
 * @expect: Expected number of responses
 * @count: Actual response count (output)
 *
 * Polls all contexts in session domains and updates load values.
 */
static int skey_poll_ctx(struct wd_sched_ctx *sched_ctx, struct wd_sched_key *skey,
			 __u32 expect, __u32 *count)
{
	__u32 poll_num;
	__u32 idx, i;
	int ret;

	/* Poll async domain contexts */
	for (i = 0; i < skey->async_domain.idx_cache.idx_count; i++) {
		idx = skey->async_domain.idx_cache.idx_list[i];
		
		poll_num = 0;
		ret = sched_ctx->poll_func(idx, expect, &poll_num);
		if ((ret < 0) && (ret != -EAGAIN))
			return ret;

		if (poll_num > 0) {
			*count += poll_num;
			/* Update load value for this context */
			__u32 current_load = skey->async_domain.idx_cache.load_values[idx];
			if (current_load > poll_num)
				wd_sched_domain_idx_cache_update_load(&skey->async_domain.idx_cache,
				                                       idx, current_load - poll_num);
			else
				wd_sched_domain_idx_cache_update_load(&skey->async_domain.idx_cache,
				                                       idx, 0);
		}
	}

	return 0;
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

	ret = skey_poll_ctx(sched_ctx, skey, expect, count);
	if (unlikely(ret))
		return ret;

	return 0;
}

static handle_t instr_sched_init(handle_t h_sched_ctx, void *sched_param)
{
	struct wd_sched_ctx *sched_ctx = (struct wd_sched_ctx *)h_sched_ctx;
	struct sched_params *param = (struct sched_params *)sched_param;
	int cpu = sched_getcpu();
	int node = numa_node_of_cpu(cpu);
	struct wd_sched_key *skey;
	__u32 sync_ctx, async_ctx;
	__u32 max_idx;
	int region_id = 0;

	if (node < 0) {
		WD_ERR("invalid: failed to get numa node!\n");
		return (handle_t)(-WD_EINVAL);
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
		region_id = 0;
		skey->ctx_prop = UADK_CTX_CE_INS;
		WD_INFO("instr don't set scheduler parameters!\n");
	} else if (param->numa_id < 0) {
		region_id = 0;
		skey->type = param->type;
		skey->ctx_prop = param->ctx_prop;
	} else {
		region_id = param->numa_id;
		skey->type = param->type;
		skey->ctx_prop = param->ctx_prop;
	}

	skey->region_id = region_id;
	max_idx = sched_ctx->region_num ? sched_ctx->region_num : 256;

	/* Pre-fetch one sync context */
	sync_ctx = session_sched_init_ctx(sched_ctx, region_id, skey->type,
	                                  skey->ctx_prop, SCHED_MODE_SYNC);
	
	/* Pre-fetch one async context */
	async_ctx = session_sched_init_ctx(sched_ctx, region_id, skey->type,
	                                   skey->ctx_prop, SCHED_MODE_ASYNC);

	if (sync_ctx != INVALID_POS) {
		if (wd_sched_key_domain_init(&skey->sync_domain, &sync_ctx, 1, max_idx) != 0) {
			goto out;
		}
	}

	if (async_ctx != INVALID_POS) {
		if (wd_sched_key_domain_init(&skey->async_domain, &async_ctx, 1, max_idx) != 0) {
			goto out;
		}
	}

	sched_skey_param_init(sched_ctx, skey);
	WD_ERR("instr contexts: sync=%u, async=%u\n", sync_ctx, async_ctx);

	return (handle_t)skey;

out:
	free(skey);
	return (handle_t)(-WD_EINVAL);
}

static __u32 instr_sched_pick_next_ctx(handle_t h_sched_ctx, void *sched_key,
				       const int sched_mode)
{
	struct wd_sched_key *key = (struct wd_sched_key *)sched_key;

	if (sched_mode == SCHED_MODE_SYNC)
		return wd_sched_domain_idx_cache_get_min_load(&key->sync_domain.idx_cache);
	else
		return wd_sched_domain_idx_cache_get_min_load(&key->async_domain.idx_cache);
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

static void none_set_param(handle_t h_sched_ctx,
			void *sched_key, void *sched_param)
{
	return;
}

static struct wd_sched sched_table[SCHED_POLICY_BUTT] = {
	{
		.name = "RR scheduler",
		.sched_policy = SCHED_POLICY_RR,
		.sched_init = session_sched_init,
		.pick_next_ctx = session_sched_pick_next_ctx,
		.poll_policy = session_sched_poll_policy,
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
		.sched_init = session_sched_init,
		.pick_next_ctx = session_sched_pick_next_ctx,
		.poll_policy = session_sched_poll_policy,
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

	if (param->ctx_prop < 0 || param->ctx_prop > UADK_CTX_SOFT)
		param->ctx_prop = UADK_CTX_HW;

	/* Insert or get domain from hash table using four dimensions */
	domain = wd_sched_hash_table_insert(sched_ctx->domain_hash_table,
	                                    param->numa_id, mode, param->type,
	                                    param->ctx_prop);
	if (!domain)
		return -WD_ENOMEM;

	if (domain->valid) {
		WD_INFO("domain (region=%d, mode=%u, type=%u, prop=%u) already registered\n",
			param->numa_id, mode, param->type, param->ctx_prop);
		return WD_SUCCESS;
	}

	/* Add context range as new segment */
	ret = wd_sched_domain_add_segment(domain, param->begin, param->end);
	if (ret) {
		WD_ERR("failed to add segment to domain!\n");
		return ret;
	}

	domain->valid = true;

	WD_ERR("instance: region=%d, mode=%u, type=%u, prop=%u, begin=%u, end=%u\n",
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
			wd_sched_key_domain_destroy(&sched_ctx->skey[i]->sync_domain);
			wd_sched_key_domain_destroy(&sched_ctx->skey[i]->async_domain);
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
	int ret, i;

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
		estimated_entries = region_num * type_num * SCHED_MODE_BUTT * UADK_CTX_MAX;
	} else {
		/* NUMA mode: validate region_num */
		if (numa_num_check(region_num))
			goto err_out;
		estimated_entries = region_num * type_num * SCHED_MODE_BUTT * UADK_CTX_MAX;
	}

	/* Create single global hash table */
	sched_ctx->domain_hash_table = wd_sched_hash_table_create(estimated_entries);
	if (!sched_ctx->domain_hash_table) {
		WD_ERR("failed to create hash table!\n");
		goto ctx_out;
	}

simple_ok:
	sched_ctx->poll_func = func;

	for (i = 0; i < MAX_SKEY_REGION_NUM; i++) {
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

	WD_INFO("Scheduler %s allocated: type_num=%u, region_num=%u, mode_num=%u\n",
		sched->name, type_num, region_num, SCHED_MODE_BUTT);

	return sched;

ctx_out:
	free(sched_ctx);
err_out:
	free(sched);
	return NULL;
}
