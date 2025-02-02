/* Copyright (c) 2013-2018, Linaro Limited
 * Copyright (c) 2019-2021, Nokia
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

/*
 * Suppress bounds warnings about interior zero length arrays. Such an array
 * is used intentionally in prio_queue_t.
 */
#if __GNUC__ >= 10
#pragma GCC diagnostic ignored "-Wzero-length-bounds"
#endif

#include <odp/api/schedule.h>
#include <odp_schedule_if.h>
#include <odp/api/align.h>
#include <odp/api/shared_memory.h>
#include <odp_debug_internal.h>
#include <odp/api/thread.h>
#include <odp/api/plat/thread_inlines.h>
#include <odp/api/time.h>
#include <odp/api/plat/time_inlines.h>
#include <odp/api/ticketlock.h>
#include <odp/api/hints.h>
#include <odp/api/cpu.h>
#include <odp/api/thrmask.h>
#include <odp_config_internal.h>
#include <odp_align_internal.h>
#include <odp/api/sync.h>
#include <odp/api/packet_io.h>
#include <odp_ring_u32_internal.h>
#include <odp_timer_internal.h>
#include <odp_queue_basic_internal.h>
#include <odp_libconfig_internal.h>
#include <odp/api/plat/queue_inlines.h>
#include <odp_global_data.h>
#include <odp_event_internal.h>

#include <string.h>

/* No synchronization context */
#define NO_SYNC_CONTEXT ODP_SCHED_SYNC_PARALLEL

/* Number of priority levels  */
#define NUM_PRIO 8

/* Number of scheduling groups */
#define NUM_SCHED_GRPS 32

/* Group weight table size */
#define GRP_WEIGHT_TBL_SIZE NUM_SCHED_GRPS

/* Spread balancing frequency. Balance every BALANCE_ROUNDS_M1 + 1 scheduling rounds. */
#define BALANCE_ROUNDS_M1 0xfffff

/* Load of a queue */
#define QUEUE_LOAD 256

/* Margin for load balance hysteresis */
#define QUEUE_LOAD_MARGIN 8

/* Ensure that load calculation does not wrap around */
ODP_STATIC_ASSERT((QUEUE_LOAD * CONFIG_MAX_SCHED_QUEUES) < UINT32_MAX, "Load_value_too_large");

/* Maximum priority queue spread */
#define MAX_SPREAD 8

/* Minimum priority queue spread */
#define MIN_SPREAD 1

/* A thread polls a non preferred sched queue every this many polls
 * of the prefer queue. */
#define MAX_PREFER_WEIGHT 127
#define MIN_PREFER_WEIGHT 1
#define MAX_PREFER_RATIO  (MAX_PREFER_WEIGHT + 1)

/* Spread weight table */
#define SPREAD_TBL_SIZE ((MAX_SPREAD - 1) * MAX_PREFER_RATIO)

/* Random data table size */
#define RANDOM_TBL_SIZE 128

/* Maximum number of packet IO interfaces */
#define NUM_PKTIO ODP_CONFIG_PKTIO_ENTRIES

/* Maximum pktin index. Needs to fit into 8 bits. */
#define MAX_PKTIN_INDEX 255

/* Maximum priority queue ring size. A ring must be large enough to store all
 * queues in the worst case (all queues are scheduled, have the same priority
 * and no spreading). */
#define MAX_RING_SIZE CONFIG_MAX_SCHED_QUEUES

/* For best performance, the number of queues should be a power of two. */
ODP_STATIC_ASSERT(CHECK_IS_POWER2(CONFIG_MAX_SCHED_QUEUES),
		  "Number_of_queues_is_not_power_of_two");

/* Ring size must be power of two, so that mask can be used. */
ODP_STATIC_ASSERT(CHECK_IS_POWER2(MAX_RING_SIZE),
		  "Ring_size_is_not_power_of_two");

/* Thread ID is saved into uint16_t variable */
ODP_STATIC_ASSERT(ODP_THREAD_COUNT_MAX < (64 * 1024),
		  "Max_64k_threads_supported");

/* Mask of queues per priority */
typedef uint8_t prio_q_mask_t;

ODP_STATIC_ASSERT((8 * sizeof(prio_q_mask_t)) >= MAX_SPREAD,
		  "prio_q_mask_t_is_too_small");

/* Start of named groups in group mask arrays */
#define SCHED_GROUP_NAMED (ODP_SCHED_GROUP_CONTROL + 1)

/* Limits for burst size configuration */
#define BURST_MAX  255
#define STASH_SIZE CONFIG_BURST_SIZE

/* Ordered stash size */
#define MAX_ORDERED_STASH 512

/* Storage for stashed enqueue operation arguments */
typedef struct {
	_odp_event_hdr_t *event_hdr[QUEUE_MULTI_MAX];
	odp_queue_t queue;
	int num;
} ordered_stash_t;

/* Ordered lock states */
typedef union {
	uint8_t u8[CONFIG_QUEUE_MAX_ORD_LOCKS];
	uint32_t all;
} lock_called_t;

ODP_STATIC_ASSERT(sizeof(lock_called_t) == sizeof(uint32_t),
		  "Lock_called_values_do_not_fit_in_uint32");

static uint8_t sched_random_u8[] = {
	0x64, 0xe3, 0x64, 0x0a, 0x0a, 0x5b, 0x7e, 0xd7,
	0x43, 0xb7, 0x90, 0x71, 0x76, 0x17, 0x8e, 0x3f,
	0x17, 0x60, 0x7e, 0xfd, 0x99, 0xe3, 0xab, 0x06,
	0x77, 0xf9, 0x45, 0x17, 0x2f, 0x81, 0x9e, 0x7b,
	0x20, 0x1b, 0x36, 0x75, 0x69, 0xc5, 0x69, 0x27,
	0x7a, 0xf6, 0x3f, 0x63, 0x2c, 0x3f, 0x1b, 0xeb,
	0x12, 0xe1, 0x6f, 0xd4, 0xd9, 0x14, 0x97, 0xa6,
	0x2a, 0xe5, 0xb0, 0x45, 0x27, 0xa6, 0x48, 0xbc,
	0x2b, 0xec, 0xd8, 0xda, 0x55, 0xef, 0x15, 0xce,
	0xf8, 0xc2, 0x1e, 0xc8, 0x16, 0x6c, 0xf0, 0x4f,
	0x1a, 0xc7, 0x50, 0x9e, 0x0b, 0xa5, 0xe9, 0xf3,
	0x28, 0x79, 0x2e, 0x18, 0xb0, 0xb4, 0xac, 0xce,
	0x67, 0x04, 0x52, 0x98, 0xce, 0x8c, 0x05, 0x87,
	0xab, 0xc8, 0x94, 0x7e, 0x46, 0x63, 0x60, 0x8d,
	0x3d, 0x8f, 0x14, 0x85, 0x1e, 0x92, 0xd2, 0x40,
	0x2d, 0x42, 0xfe, 0xf1, 0xc2, 0xb6, 0x03, 0x43
};

ODP_STATIC_ASSERT(sizeof(sched_random_u8) == RANDOM_TBL_SIZE, "Bad_random_table_size");

/* Scheduler local data */
typedef struct ODP_ALIGNED_CACHE {
	uint32_t sched_round;
	uint16_t thr;
	uint8_t  pause;
	uint8_t  sync_ctx;
	uint8_t  balance_on;
	uint16_t balance_start;
	uint16_t spread_round;

	struct {
		uint16_t    num_ev;
		uint16_t    ev_index;
		uint32_t    qi;
		odp_queue_t queue;
		ring_u32_t   *ring;
		odp_event_t ev[STASH_SIZE];
	} stash;

	uint32_t grp_epoch;
	uint16_t num_grp;
	uint8_t grp[NUM_SCHED_GRPS];
	uint8_t spread_tbl[SPREAD_TBL_SIZE];
	uint8_t grp_weight[GRP_WEIGHT_TBL_SIZE];

	struct {
		/* Source queue index */
		uint32_t src_queue;
		uint64_t ctx; /**< Ordered context id */
		int stash_num; /**< Number of stashed enqueue operations */
		uint8_t in_order; /**< Order status */
		lock_called_t lock_called; /**< States of ordered locks */
		/** Storage for stashed enqueue operations */
		ordered_stash_t stash[MAX_ORDERED_STASH];
	} ordered;

} sched_local_t;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
/* Priority queue */
typedef struct ODP_ALIGNED_CACHE {
	/* Ring header */
	ring_u32_t ring;

	/* Ring data: queue indexes */
	uint32_t queue_index[MAX_RING_SIZE]; /* overlaps with ring.data[] */

} prio_queue_t;
#pragma GCC diagnostic pop

/* Order context of a queue */
typedef struct ODP_ALIGNED_CACHE {
	/* Current ordered context id */
	odp_atomic_u64_t ctx ODP_ALIGNED_CACHE;

	/* Next unallocated context id */
	odp_atomic_u64_t next_ctx;

	/* Array of ordered locks */
	odp_atomic_u64_t lock[CONFIG_QUEUE_MAX_ORD_LOCKS];

} order_context_t;

typedef struct {
	struct {
		uint8_t burst_default[NUM_PRIO];
		uint8_t burst_max[NUM_PRIO];
		uint8_t num_spread;
		uint8_t prefer_ratio;
	} config;

	uint8_t          load_balance;
	uint16_t         max_spread;
	uint32_t         ring_mask;
	odp_atomic_u32_t grp_epoch;
	odp_shm_t        shm;
	odp_ticketlock_t mask_lock[NUM_SCHED_GRPS];
	prio_q_mask_t    prio_q_mask[NUM_SCHED_GRPS][NUM_PRIO];

	struct {
		uint8_t grp;
		/* Inverted prio value (max = 0) vs API (min = 0)*/
		uint8_t prio;
		uint8_t spread;
		uint8_t sync;
		uint8_t order_lock_count;
		uint8_t poll_pktin;
		uint8_t pktio_index;
		uint8_t pktin_index;
	} queue[CONFIG_MAX_SCHED_QUEUES];

	/* Scheduler priority queues */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
	prio_queue_t prio_q[NUM_SCHED_GRPS][NUM_PRIO][MAX_SPREAD];
#pragma GCC diagnostic pop
	uint32_t prio_q_count[NUM_SCHED_GRPS][NUM_PRIO][MAX_SPREAD];

	odp_thrmask_t  mask_all;
	odp_ticketlock_t grp_lock;

	struct {
		char           name[ODP_SCHED_GROUP_NAME_LEN];
		odp_thrmask_t  mask;
		uint16_t       spread_thrs[MAX_SPREAD];
		uint8_t        allocated;
	} sched_grp[NUM_SCHED_GRPS];

	struct {
		int num_pktin;
	} pktio[NUM_PKTIO];
	odp_ticketlock_t pktio_lock;

	order_context_t order[CONFIG_MAX_SCHED_QUEUES];

	/* Scheduler interface config options (not used in fast path) */
	schedule_config_t config_if;
	uint32_t max_queues;
	odp_atomic_u32_t next_rand;

} sched_global_t;

/* Check that queue[] variables are large enough */
ODP_STATIC_ASSERT(NUM_SCHED_GRPS  <= 256, "Group_does_not_fit_8_bits");
ODP_STATIC_ASSERT(NUM_PRIO        <= 256, "Prio_does_not_fit_8_bits");
ODP_STATIC_ASSERT(MAX_SPREAD      <= 256, "Spread_does_not_fit_8_bits");
ODP_STATIC_ASSERT(CONFIG_QUEUE_MAX_ORD_LOCKS <= 256,
		  "Ordered_lock_count_does_not_fit_8_bits");
ODP_STATIC_ASSERT(NUM_PKTIO        <= 256, "Pktio_index_does_not_fit_8_bits");
ODP_STATIC_ASSERT(CHECK_IS_POWER2(GRP_WEIGHT_TBL_SIZE), "Not_power_of_2");

/* Global scheduler context */
static sched_global_t *sched;

/* Thread local scheduler context */
static __thread sched_local_t sched_local;

static int read_config_file(sched_global_t *sched)
{
	const char *str;
	int i;
	int burst_val[NUM_PRIO];
	int val = 0;

	ODP_PRINT("Scheduler config:\n");

	str = "sched_basic.prio_spread";
	if (!_odp_libconfig_lookup_int(str, &val)) {
		ODP_ERR("Config option '%s' not found.\n", str);
		return -1;
	}

	if (val > MAX_SPREAD || val < MIN_SPREAD) {
		ODP_ERR("Bad value %s = %u [min: %u, max: %u]\n", str, val,
			MIN_SPREAD, MAX_SPREAD);
		return -1;
	}

	sched->config.num_spread = val;
	ODP_PRINT("  %s: %i\n", str, val);

	str = "sched_basic.prio_spread_weight";
	if (!_odp_libconfig_lookup_int(str, &val)) {
		ODP_ERR("Config option '%s' not found.\n", str);
		return -1;
	}

	if (val > MAX_PREFER_WEIGHT || val < MIN_PREFER_WEIGHT) {
		ODP_ERR("Bad value %s = %u [min: %u, max: %u]\n", str, val,
			MIN_PREFER_WEIGHT, MAX_PREFER_WEIGHT);
		return -1;
	}

	sched->config.prefer_ratio = val + 1;
	ODP_PRINT("  %s: %i\n", str, val);

	str = "sched_basic.load_balance";
	if (!_odp_libconfig_lookup_int(str, &val)) {
		ODP_ERR("Config option '%s' not found.\n", str);
		return -1;
	}

	if (val > 1 || val < 0) {
		ODP_ERR("Bad value %s = %i\n", str, val);
		return -1;
	}
	ODP_PRINT("  %s: %i\n", str, val);

	sched->load_balance = 1;
	if (val == 0 || sched->config.num_spread == 1)
		sched->load_balance = 0;

	str = "sched_basic.burst_size_default";
	if (_odp_libconfig_lookup_array(str, burst_val, NUM_PRIO) !=
	    NUM_PRIO) {
		ODP_ERR("Config option '%s' not found.\n", str);
		return -1;
	}

	ODP_PRINT("  %s[] =", str);
	for (i = 0; i < NUM_PRIO; i++) {
		val = burst_val[i];
		sched->config.burst_default[i] = val;
		ODP_PRINT(" %3i", val);

		if (val > STASH_SIZE || val < 1) {
			ODP_ERR("Bad value %i\n", val);
			return -1;
		}
	}
	ODP_PRINT("\n");

	str = "sched_basic.burst_size_max";
	if (_odp_libconfig_lookup_array(str, burst_val, NUM_PRIO) !=
	    NUM_PRIO) {
		ODP_ERR("Config option '%s' not found.\n", str);
		return -1;
	}

	ODP_PRINT("  %s[] =    ", str);
	for (i = 0; i < NUM_PRIO; i++) {
		val = burst_val[i];
		sched->config.burst_max[i] = val;
		ODP_PRINT(" %3i", val);

		if (val > BURST_MAX || val < 1) {
			ODP_ERR("Bad value %i\n", val);
			return -1;
		}
	}

	ODP_PRINT("\n");

	str = "sched_basic.group_enable.all";
	if (!_odp_libconfig_lookup_int(str, &val)) {
		ODP_ERR("Config option '%s' not found.\n", str);
		return -1;
	}

	sched->config_if.group_enable.all = val;
	ODP_PRINT("  %s: %i\n", str, val);

	str = "sched_basic.group_enable.worker";
	if (!_odp_libconfig_lookup_int(str, &val)) {
		ODP_ERR("Config option '%s' not found.\n", str);
		return -1;
	}

	sched->config_if.group_enable.worker = val;
	ODP_PRINT("  %s: %i\n", str, val);

	str = "sched_basic.group_enable.control";
	if (!_odp_libconfig_lookup_int(str, &val)) {
		ODP_ERR("Config option '%s' not found.\n", str);
		return -1;
	}

	sched->config_if.group_enable.control = val;
	ODP_PRINT("  %s: %i\n", str, val);

	ODP_PRINT("  dynamic load balance: %s\n", sched->load_balance ? "ON" : "OFF");

	ODP_PRINT("\n");

	return 0;
}

/* Spread from thread or other index */
static inline uint8_t spread_from_index(uint32_t index)
{
	return index % sched->config.num_spread;
}

static void sched_local_init(void)
{
	int i;
	uint8_t spread, prefer_ratio;
	uint8_t num_spread = sched->config.num_spread;
	uint8_t offset = 1;

	memset(&sched_local, 0, sizeof(sched_local_t));

	sched_local.thr         = odp_thread_id();
	sched_local.sync_ctx    = NO_SYNC_CONTEXT;
	sched_local.stash.queue = ODP_QUEUE_INVALID;

	spread = spread_from_index(sched_local.thr);
	prefer_ratio = sched->config.prefer_ratio;

	for (i = 0; i < SPREAD_TBL_SIZE; i++) {
		sched_local.spread_tbl[i] = spread;

		if (num_spread > 1 && (i % prefer_ratio) == 0) {
			sched_local.spread_tbl[i] = spread_from_index(spread + offset);
			offset++;
			if (offset == num_spread)
				offset = 1;
		}
	}
}

static int schedule_init_global(void)
{
	odp_shm_t shm;
	int i, j, grp;
	int prefer_ratio;
	uint32_t ring_size, num_rings;

	ODP_DBG("Schedule init ... ");

	shm = odp_shm_reserve("_odp_sched_basic_global",
			      sizeof(sched_global_t),
			      ODP_CACHE_LINE_SIZE,
			      0);
	if (shm == ODP_SHM_INVALID) {
		ODP_ERR("Schedule init: Shm reserve failed.\n");
		return -1;
	}

	sched = odp_shm_addr(shm);
	memset(sched, 0, sizeof(sched_global_t));

	if (read_config_file(sched)) {
		odp_shm_free(shm);
		return -1;
	}

	sched->shm = shm;
	prefer_ratio = sched->config.prefer_ratio;

	/* When num_spread == 1, only spread_tbl[0] is used. */
	sched->max_spread = (sched->config.num_spread - 1) * prefer_ratio;

	/* Dynamic load balance may move all queues into a single ring.
	 * Ring size can be smaller with fixed spreading. */
	if (sched->load_balance) {
		ring_size = MAX_RING_SIZE;
		num_rings = 1;
	} else {
		ring_size = MAX_RING_SIZE / sched->config.num_spread;
		num_rings = sched->config.num_spread;
	}

	ring_size = ROUNDUP_POWER2_U32(ring_size);
	ODP_ASSERT(ring_size <= MAX_RING_SIZE);
	sched->ring_mask = ring_size - 1;

	/* Each ring can hold in maximum ring_size-1 queues. Due to ring size round up,
	 * total capacity of rings may be larger than CONFIG_MAX_SCHED_QUEUES. */
	sched->max_queues = sched->ring_mask * num_rings;
	if (sched->max_queues > CONFIG_MAX_SCHED_QUEUES)
		sched->max_queues = CONFIG_MAX_SCHED_QUEUES;

	for (grp = 0; grp < NUM_SCHED_GRPS; grp++) {
		odp_ticketlock_init(&sched->mask_lock[grp]);

		for (i = 0; i < NUM_PRIO; i++) {
			for (j = 0; j < MAX_SPREAD; j++) {
				prio_queue_t *prio_q;

				prio_q = &sched->prio_q[grp][i][j];
				ring_u32_init(&prio_q->ring);
			}
		}
	}

	odp_ticketlock_init(&sched->pktio_lock);
	for (i = 0; i < NUM_PKTIO; i++)
		sched->pktio[i].num_pktin = 0;

	odp_ticketlock_init(&sched->grp_lock);
	odp_atomic_init_u32(&sched->grp_epoch, 0);
	odp_atomic_init_u32(&sched->next_rand, 0);

	for (i = 0; i < NUM_SCHED_GRPS; i++) {
		memset(sched->sched_grp[i].name, 0, ODP_SCHED_GROUP_NAME_LEN);
		odp_thrmask_zero(&sched->sched_grp[i].mask);
	}

	sched->sched_grp[ODP_SCHED_GROUP_ALL].allocated = 1;
	sched->sched_grp[ODP_SCHED_GROUP_WORKER].allocated = 1;
	sched->sched_grp[ODP_SCHED_GROUP_CONTROL].allocated = 1;
	strncpy(sched->sched_grp[ODP_SCHED_GROUP_ALL].name, "__SCHED_GROUP_ALL",
		ODP_SCHED_GROUP_NAME_LEN - 1);
	strncpy(sched->sched_grp[ODP_SCHED_GROUP_WORKER].name, "__SCHED_GROUP_WORKER",
		ODP_SCHED_GROUP_NAME_LEN - 1);
	strncpy(sched->sched_grp[ODP_SCHED_GROUP_CONTROL].name, "__SCHED_GROUP_CONTROL",
		ODP_SCHED_GROUP_NAME_LEN - 1);


	odp_thrmask_setall(&sched->mask_all);

	ODP_DBG("done\n");

	return 0;
}

static int schedule_term_global(void)
{
	int ret = 0;
	int rc = 0;
	int i, j, grp;
	uint32_t ring_mask = sched->ring_mask;

	for (grp = 0; grp < NUM_SCHED_GRPS; grp++) {
		for (i = 0; i < NUM_PRIO; i++) {
			for (j = 0; j < MAX_SPREAD; j++) {
				ring_u32_t *ring;
				uint32_t qi;

				ring = &sched->prio_q[grp][i][j].ring;

				while (ring_u32_deq(ring, ring_mask, &qi)) {
					odp_event_t events[1];
					int num;

					num = _odp_sched_queue_deq(qi, events, 1, 1);

					if (num > 0)
						ODP_ERR("Queue not empty\n");
				}
			}
		}
	}

	ret = odp_shm_free(sched->shm);
	if (ret < 0) {
		ODP_ERR("Shm free failed for odp_scheduler");
		rc = -1;
	}

	return rc;
}

static int schedule_init_local(void)
{
	sched_local_init();
	return 0;
}

static inline void grp_update_mask(int grp, const odp_thrmask_t *new_mask)
{
	odp_thrmask_copy(&sched->sched_grp[grp].mask, new_mask);
	odp_atomic_add_rel_u32(&sched->grp_epoch, 1);
}

static inline int grp_update_tbl(void)
{
	int i;
	int num = 0;
	int thr = sched_local.thr;

	odp_ticketlock_lock(&sched->grp_lock);

	for (i = 0; i < NUM_SCHED_GRPS; i++) {
		if (sched->sched_grp[i].allocated == 0)
			continue;

		if (odp_thrmask_isset(&sched->sched_grp[i].mask, thr)) {
			sched_local.grp[num] = i;
			num++;
		}
	}

	odp_ticketlock_unlock(&sched->grp_lock);

	if (odp_unlikely(num == 0))
		return 0;

	/* Update group weights. Round robin over all thread's groups. */
	for (i = 0; i < GRP_WEIGHT_TBL_SIZE; i++)
		sched_local.grp_weight[i] = i % num;

	sched_local.num_grp = num;
	return num;
}

static uint32_t schedule_max_ordered_locks(void)
{
	return CONFIG_QUEUE_MAX_ORD_LOCKS;
}

static int schedule_min_prio(void)
{
	return 0;
}

static int schedule_max_prio(void)
{
	return NUM_PRIO - 1;
}

static int schedule_default_prio(void)
{
	return schedule_max_prio() / 2;
}

static int schedule_num_prio(void)
{
	return NUM_PRIO;
}

static inline int prio_level_from_api(int api_prio)
{
	return schedule_max_prio() - api_prio;
}

static inline void dec_queue_count(int grp, int prio, int spr)
{
	odp_ticketlock_lock(&sched->mask_lock[grp]);

	sched->prio_q_count[grp][prio][spr]--;

	/* Clear mask bit only when the last queue is removed */
	if (sched->prio_q_count[grp][prio][spr] == 0)
		sched->prio_q_mask[grp][prio] &= (uint8_t)(~(1 << spr));

	odp_ticketlock_unlock(&sched->mask_lock[grp]);
}

static inline void update_queue_count(int grp, int prio, int old_spr, int new_spr)
{
	odp_ticketlock_lock(&sched->mask_lock[grp]);

	sched->prio_q_mask[grp][prio] |= 1 << new_spr;
	sched->prio_q_count[grp][prio][new_spr]++;

	sched->prio_q_count[grp][prio][old_spr]--;

	if (sched->prio_q_count[grp][prio][old_spr] == 0)
		sched->prio_q_mask[grp][prio] &= (uint8_t)(~(1 << old_spr));

	odp_ticketlock_unlock(&sched->mask_lock[grp]);
}

static uint8_t allocate_spread(int grp, int prio)
{
	uint8_t i, num_min, spr;
	uint32_t num;
	uint32_t min = UINT32_MAX;
	uint8_t num_spread = sched->config.num_spread;
	uint8_t min_spr[num_spread];

	num_min = 1;
	min_spr[0] = 0;

	odp_ticketlock_lock(&sched->mask_lock[grp]);

	/* Find spread(s) with the minimum number of queues */
	for (i = 0; i < num_spread; i++) {
		num = sched->prio_q_count[grp][prio][i];
		if (num < min) {
			min = num;
			min_spr[0] = i;
			num_min = 1;
		} else if (num == min) {
			min_spr[num_min] = i;
			num_min++;
		}
	}

	spr = min_spr[0];

	/* When there are multiple minimum spreads, select one randomly */
	if (num_min > 1) {
		uint32_t next_rand = odp_atomic_fetch_inc_u32(&sched->next_rand);
		uint8_t rand = sched_random_u8[next_rand % RANDOM_TBL_SIZE];

		spr = min_spr[rand % num_min];
	}

	sched->prio_q_mask[grp][prio] |= 1 << spr;
	sched->prio_q_count[grp][prio][spr]++;

	odp_ticketlock_unlock(&sched->mask_lock[grp]);

	return spr;
}

static int schedule_create_queue(uint32_t queue_index,
				 const odp_schedule_param_t *sched_param)
{
	int i;
	uint8_t spread;
	int grp  = sched_param->group;
	int prio = prio_level_from_api(sched_param->prio);

	if (odp_global_rw->schedule_configured == 0) {
		ODP_ERR("Scheduler has not been configured\n");
		return -1;
	}

	if (grp < 0 || grp >= NUM_SCHED_GRPS) {
		ODP_ERR("Bad schedule group %i\n", grp);
		return -1;
	}
	if (grp == ODP_SCHED_GROUP_ALL && !sched->config_if.group_enable.all) {
		ODP_ERR("Trying to use disabled ODP_SCHED_GROUP_ALL\n");
		return -1;
	}
	if (grp == ODP_SCHED_GROUP_CONTROL && !sched->config_if.group_enable.control) {
		ODP_ERR("Trying to use disabled ODP_SCHED_GROUP_CONTROL\n");
		return -1;
	}
	if (grp == ODP_SCHED_GROUP_WORKER && !sched->config_if.group_enable.worker) {
		ODP_ERR("Trying to use disabled ODP_SCHED_GROUP_WORKER\n");
		return -1;
	}

	odp_ticketlock_lock(&sched->grp_lock);

	if (sched->sched_grp[grp].allocated == 0) {
		odp_ticketlock_unlock(&sched->grp_lock);
		ODP_ERR("Group not created: %i\n", grp);
		return -1;
	}

	odp_ticketlock_unlock(&sched->grp_lock);

	spread = allocate_spread(grp, prio);

	sched->queue[queue_index].grp  = grp;
	sched->queue[queue_index].prio = prio;
	sched->queue[queue_index].spread = spread;
	sched->queue[queue_index].sync = sched_param->sync;
	sched->queue[queue_index].order_lock_count = sched_param->lock_count;
	sched->queue[queue_index].poll_pktin  = 0;
	sched->queue[queue_index].pktio_index = 0;
	sched->queue[queue_index].pktin_index = 0;

	odp_atomic_init_u64(&sched->order[queue_index].ctx, 0);
	odp_atomic_init_u64(&sched->order[queue_index].next_ctx, 0);

	for (i = 0; i < CONFIG_QUEUE_MAX_ORD_LOCKS; i++)
		odp_atomic_init_u64(&sched->order[queue_index].lock[i], 0);

	return 0;
}

static inline uint8_t sched_sync_type(uint32_t queue_index)
{
	return sched->queue[queue_index].sync;
}

static void schedule_destroy_queue(uint32_t queue_index)
{
	int grp  = sched->queue[queue_index].grp;
	int prio = sched->queue[queue_index].prio;
	int spread = sched->queue[queue_index].spread;

	dec_queue_count(grp, prio, spread);

	sched->queue[queue_index].grp    = 0;
	sched->queue[queue_index].prio   = 0;
	sched->queue[queue_index].spread = 0;

	if ((sched_sync_type(queue_index) == ODP_SCHED_SYNC_ORDERED) &&
	    odp_atomic_load_u64(&sched->order[queue_index].ctx) !=
	    odp_atomic_load_u64(&sched->order[queue_index].next_ctx))
		ODP_ERR("queue reorder incomplete\n");
}

static int schedule_sched_queue(uint32_t queue_index)
{
	int grp      = sched->queue[queue_index].grp;
	int prio     = sched->queue[queue_index].prio;
	int spread   = sched->queue[queue_index].spread;
	ring_u32_t *ring = &sched->prio_q[grp][prio][spread].ring;

	ring_u32_enq(ring, sched->ring_mask, queue_index);
	return 0;
}

static void schedule_pktio_start(int pktio_index, int num_pktin,
				 int pktin_idx[], odp_queue_t queue[])
{
	int i;
	uint32_t qi;

	sched->pktio[pktio_index].num_pktin = num_pktin;

	for (i = 0; i < num_pktin; i++) {
		qi = queue_to_index(queue[i]);
		sched->queue[qi].poll_pktin  = 1;
		sched->queue[qi].pktio_index = pktio_index;
		sched->queue[qi].pktin_index = pktin_idx[i];

		ODP_ASSERT(pktin_idx[i] <= MAX_PKTIN_INDEX);

		/* Start polling */
		_odp_sched_queue_set_status(qi, QUEUE_STATUS_SCHED);
		schedule_sched_queue(qi);
	}
}

static inline void release_atomic(void)
{
	uint32_t qi  = sched_local.stash.qi;
	ring_u32_t *ring = sched_local.stash.ring;

	/* Release current atomic queue */
	ring_u32_enq(ring, sched->ring_mask, qi);

	/* We don't hold sync context anymore */
	sched_local.sync_ctx = NO_SYNC_CONTEXT;
}

static void schedule_release_atomic(void)
{
	if (sched_local.sync_ctx == ODP_SCHED_SYNC_ATOMIC &&
	    sched_local.stash.num_ev == 0)
		release_atomic();
}

static inline int ordered_own_turn(uint32_t queue_index)
{
	uint64_t ctx;

	ctx = odp_atomic_load_acq_u64(&sched->order[queue_index].ctx);

	return ctx == sched_local.ordered.ctx;
}

static inline void wait_for_order(uint32_t queue_index)
{
	/* Busy loop to synchronize ordered processing */
	while (1) {
		if (ordered_own_turn(queue_index))
			break;
		odp_cpu_pause();
	}
}

/**
 * Perform stashed enqueue operations
 *
 * Should be called only when already in order.
 */
static inline void ordered_stash_release(void)
{
	int i;

	for (i = 0; i < sched_local.ordered.stash_num; i++) {
		odp_queue_t queue;
		_odp_event_hdr_t **event_hdr;
		int num, num_enq;

		queue = sched_local.ordered.stash[i].queue;
		event_hdr = sched_local.ordered.stash[i].event_hdr;
		num = sched_local.ordered.stash[i].num;

		num_enq = odp_queue_enq_multi(queue,
					      (odp_event_t *)event_hdr, num);

		/* Drop packets that were not enqueued */
		if (odp_unlikely(num_enq < num)) {
			if (odp_unlikely(num_enq < 0))
				num_enq = 0;

			ODP_DBG("Dropped %i packets\n", num - num_enq);
			_odp_event_free_multi(&event_hdr[num_enq], num - num_enq);
		}
	}
	sched_local.ordered.stash_num = 0;
}

static inline void release_ordered(void)
{
	uint32_t qi;
	uint32_t i;

	qi = sched_local.ordered.src_queue;

	wait_for_order(qi);

	/* Release all ordered locks */
	for (i = 0; i < sched->queue[qi].order_lock_count; i++) {
		if (!sched_local.ordered.lock_called.u8[i])
			odp_atomic_store_rel_u64(&sched->order[qi].lock[i],
						 sched_local.ordered.ctx + 1);
	}

	sched_local.ordered.lock_called.all = 0;
	sched_local.ordered.in_order = 0;

	/* We don't hold sync context anymore */
	sched_local.sync_ctx = NO_SYNC_CONTEXT;

	ordered_stash_release();

	/* Next thread can continue processing */
	odp_atomic_add_rel_u64(&sched->order[qi].ctx, 1);
}

static void schedule_release_ordered(void)
{
	if (odp_unlikely((sched_local.sync_ctx != ODP_SCHED_SYNC_ORDERED) ||
			 sched_local.stash.num_ev))
		return;

	release_ordered();
}

static int schedule_term_local(void)
{
	if (sched_local.stash.num_ev) {
		ODP_ERR("Locally pre-scheduled events exist.\n");
		return -1;
	}

	if (sched_local.sync_ctx == ODP_SCHED_SYNC_ATOMIC)
		schedule_release_atomic();
	else if (sched_local.sync_ctx == ODP_SCHED_SYNC_ORDERED)
		schedule_release_ordered();

	return 0;
}

static void schedule_config_init(odp_schedule_config_t *config)
{
	config->num_queues = sched->max_queues;
	config->queue_size = _odp_queue_glb->config.max_queue_size;
	config->sched_group.all = sched->config_if.group_enable.all;
	config->sched_group.control = sched->config_if.group_enable.control;
	config->sched_group.worker = sched->config_if.group_enable.worker;
}

static void schedule_group_clear(odp_schedule_group_t group)
{
	odp_thrmask_t zero;

	odp_thrmask_zero(&zero);

	if (group < 0 || group > ODP_SCHED_GROUP_CONTROL)
		ODP_ABORT("Invalid scheduling group\n");

	grp_update_mask(group, &zero);
	sched->sched_grp[group].allocated = 0;
}

static int schedule_config(const odp_schedule_config_t *config)
{
	odp_ticketlock_lock(&sched->grp_lock);

	sched->config_if.group_enable.all = config->sched_group.all;
	sched->config_if.group_enable.control = config->sched_group.control;
	sched->config_if.group_enable.worker = config->sched_group.worker;

	/* Remove existing threads from predefined scheduling groups. */
	if (!config->sched_group.all)
		schedule_group_clear(ODP_SCHED_GROUP_ALL);

	if (!config->sched_group.worker)
		schedule_group_clear(ODP_SCHED_GROUP_WORKER);

	if (!config->sched_group.control)
		schedule_group_clear(ODP_SCHED_GROUP_CONTROL);

	odp_ticketlock_unlock(&sched->grp_lock);

	return 0;
}

/* Spread load after adding 'num' queues */
static inline uint32_t spread_load(int grp, int prio, int spr, int num)
{
	uint32_t num_q, num_thr;

	num_q   = sched->prio_q_count[grp][prio][spr];
	num_thr = sched->sched_grp[grp].spread_thrs[spr];

	if (num_thr == 0)
		return UINT32_MAX;

	return ((num_q + num) * QUEUE_LOAD) / num_thr;
}

static inline int balance_spread(int grp, int prio, int cur_spr)
{
	int spr;
	uint64_t cur_load, min_load, load;
	int num_spread = sched->config.num_spread;
	int new_spr = cur_spr;

	cur_load = spread_load(grp, prio, cur_spr, 0);
	min_load = cur_load;

	for (spr = 0; spr < num_spread; spr++) {
		if (spr == cur_spr)
			continue;

		load = spread_load(grp, prio, spr, 1);

		/* Move queue if improvement is larger than marginal */
		if ((load + QUEUE_LOAD_MARGIN) < min_load) {
			new_spr  = spr;
			min_load = load;
		}
	}

	return new_spr;
}

static inline int copy_from_stash(odp_event_t out_ev[], unsigned int max)
{
	int i = 0;

	while (sched_local.stash.num_ev && max) {
		out_ev[i] = sched_local.stash.ev[sched_local.stash.ev_index];
		sched_local.stash.ev_index++;
		sched_local.stash.num_ev--;
		max--;
		i++;
	}

	return i;
}

static int schedule_ord_enq_multi(odp_queue_t dst_queue, void *event_hdr[],
				  int num, int *ret)
{
	int i;
	uint32_t stash_num;
	queue_entry_t *dst_qentry;
	uint32_t src_queue;

	/* This check is done for every queue enqueue operation, also for plain
	 * queues. Return fast when not holding a scheduling context. */
	if (odp_likely(sched_local.sync_ctx != ODP_SCHED_SYNC_ORDERED))
		return 0;

	if (sched_local.ordered.in_order)
		return 0;

	dst_qentry = qentry_from_handle(dst_queue);

	if (dst_qentry->s.param.order == ODP_QUEUE_ORDER_IGNORE)
		return 0;

	src_queue  = sched_local.ordered.src_queue;
	stash_num  = sched_local.ordered.stash_num;

	if (ordered_own_turn(src_queue)) {
		/* Own turn, so can do enqueue directly. */
		sched_local.ordered.in_order = 1;
		ordered_stash_release();
		return 0;
	}

	/* Pktout may drop packets, so the operation cannot be stashed. */
	if (dst_qentry->s.pktout.pktio != ODP_PKTIO_INVALID ||
	    odp_unlikely(stash_num >=  MAX_ORDERED_STASH)) {
		/* If the local stash is full, wait until it is our turn and
		 * then release the stash and do enqueue directly. */
		wait_for_order(src_queue);

		sched_local.ordered.in_order = 1;

		ordered_stash_release();
		return 0;
	}

	sched_local.ordered.stash[stash_num].queue = dst_queue;
	sched_local.ordered.stash[stash_num].num = num;
	for (i = 0; i < num; i++)
		sched_local.ordered.stash[stash_num].event_hdr[i] = event_hdr[i];

	sched_local.ordered.stash_num++;

	*ret = num;
	return 1;
}

static inline int queue_is_pktin(uint32_t queue_index)
{
	return sched->queue[queue_index].poll_pktin;
}

static inline int poll_pktin(uint32_t qi, int direct_recv,
			     odp_event_t ev_tbl[], int max_num)
{
	int pktio_index, pktin_index, num, num_pktin;
	_odp_event_hdr_t **hdr_tbl;
	int ret;
	void *q_int;
	_odp_event_hdr_t *b_hdr[CONFIG_BURST_SIZE];

	hdr_tbl = (_odp_event_hdr_t **)ev_tbl;

	if (!direct_recv) {
		hdr_tbl = b_hdr;

		/* Limit burst to max queue enqueue size */
		if (max_num > CONFIG_BURST_SIZE)
			max_num = CONFIG_BURST_SIZE;
	}

	pktio_index = sched->queue[qi].pktio_index;
	pktin_index = sched->queue[qi].pktin_index;

	num = _odp_sched_cb_pktin_poll(pktio_index, pktin_index, hdr_tbl, max_num);

	if (num == 0)
		return 0;

	/* Pktio stopped or closed. Call stop_finalize when we have stopped
	 * polling all pktin queues of the pktio. */
	if (odp_unlikely(num < 0)) {
		odp_ticketlock_lock(&sched->pktio_lock);
		sched->pktio[pktio_index].num_pktin--;
		num_pktin = sched->pktio[pktio_index].num_pktin;
		odp_ticketlock_unlock(&sched->pktio_lock);

		_odp_sched_queue_set_status(qi, QUEUE_STATUS_NOTSCHED);

		if (num_pktin == 0)
			_odp_sched_cb_pktio_stop_finalize(pktio_index);

		return num;
	}

	if (direct_recv)
		return num;

	q_int = qentry_from_index(qi);

	ret = odp_queue_enq_multi(q_int, (odp_event_t *)b_hdr, num);

	/* Drop packets that were not enqueued */
	if (odp_unlikely(ret < num)) {
		int num_enq = ret;

		if (odp_unlikely(ret < 0))
			num_enq = 0;

		ODP_DBG("Dropped %i packets\n", num - num_enq);
		_odp_event_free_multi(&b_hdr[num_enq], num - num_enq);
	}

	return ret;
}

static inline int do_schedule_grp(odp_queue_t *out_queue, odp_event_t out_ev[],
				  unsigned int max_num, int grp, int first_spr, int balance)
{
	int prio, spr, new_spr, i, ret;
	uint32_t qi;
	uint16_t burst_def;
	int num_spread = sched->config.num_spread;
	uint32_t ring_mask = sched->ring_mask;

	/* Schedule events */
	for (prio = 0; prio < NUM_PRIO; prio++) {
		if (sched->prio_q_mask[grp][prio] == 0)
			continue;

		burst_def = sched->config.burst_default[prio];

		/* Select the first spread based on weights */
		spr = first_spr;

		for (i = 0; i < num_spread;) {
			int num;
			uint8_t sync_ctx, ordered;
			odp_queue_t handle;
			ring_u32_t *ring;
			int pktin;
			uint16_t max_deq = burst_def;
			int stashed = 1;
			odp_event_t *ev_tbl = sched_local.stash.ev;

			if (spr >= num_spread)
				spr = 0;

			/* No queues created for this priority queue */
			if (odp_unlikely((sched->prio_q_mask[grp][prio] & (1 << spr))
			    == 0)) {
				i++;
				spr++;
				continue;
			}

			/* Get queue index from the priority queue */
			ring = &sched->prio_q[grp][prio][spr].ring;

			if (ring_u32_deq(ring, ring_mask, &qi) == 0) {
				/* Priority queue empty */
				i++;
				spr++;
				continue;
			}

			sync_ctx = sched_sync_type(qi);
			ordered  = (sync_ctx == ODP_SCHED_SYNC_ORDERED);

			/* When application's array is larger than default burst
			 * size, output all events directly there. Also, ordered
			 * queues are not stashed locally to improve
			 * parallelism. Ordered context can only be released
			 * when the local cache is empty. */
			if (max_num > burst_def || ordered) {
				uint16_t burst_max;

				burst_max = sched->config.burst_max[prio];
				stashed = 0;
				ev_tbl  = out_ev;
				max_deq = max_num;
				if (max_num > burst_max)
					max_deq = burst_max;
			}

			pktin = queue_is_pktin(qi);

			/* Update queue spread before dequeue. Dequeue changes status of an empty
			 * queue, which enables a following enqueue operation to insert the queue
			 * back into scheduling (with new spread). */
			if (odp_unlikely(balance)) {
				new_spr = balance_spread(grp, prio, spr);

				if (new_spr != spr) {
					sched->queue[qi].spread = new_spr;
					ring = &sched->prio_q[grp][prio][new_spr].ring;
					update_queue_count(grp, prio, spr, new_spr);
				}
			}

			num = _odp_sched_queue_deq(qi, ev_tbl, max_deq, !pktin);

			if (odp_unlikely(num < 0)) {
				/* Destroyed queue. Continue scheduling the same
				 * priority queue. */
				continue;
			}

			if (num == 0) {
				/* Poll packet input. Continue scheduling queue
				 * connected to a packet input. Move to the next
				 * priority to avoid starvation of other
				 * priorities. Stop scheduling queue when pktio
				 * has been stopped. */
				if (pktin) {
					int direct_recv = !ordered;
					int num_pkt;

					num_pkt = poll_pktin(qi, direct_recv,
							     ev_tbl, max_deq);

					if (odp_unlikely(num_pkt < 0))
						continue;

					if (num_pkt == 0 || !direct_recv) {
						ring_u32_enq(ring, ring_mask,
							     qi);
						break;
					}

					/* Process packets from an atomic or
					 * parallel queue right away. */
					num = num_pkt;
				} else {
					/* Remove empty queue from scheduling.
					 * Continue scheduling the same priority
					 * queue. */
					continue;
				}
			}

			if (ordered) {
				uint64_t ctx;
				odp_atomic_u64_t *next_ctx;

				next_ctx = &sched->order[qi].next_ctx;
				ctx = odp_atomic_fetch_inc_u64(next_ctx);

				sched_local.ordered.ctx = ctx;
				sched_local.ordered.src_queue = qi;

				/* Continue scheduling ordered queues */
				ring_u32_enq(ring, ring_mask, qi);
				sched_local.sync_ctx = sync_ctx;

			} else if (sync_ctx == ODP_SCHED_SYNC_ATOMIC) {
				/* Hold queue during atomic access */
				sched_local.stash.qi   = qi;
				sched_local.stash.ring = ring;
				sched_local.sync_ctx   = sync_ctx;
			} else {
				/* Continue scheduling the queue */
				ring_u32_enq(ring, ring_mask, qi);
			}

			handle = queue_from_index(qi);

			if (stashed) {
				sched_local.stash.num_ev   = num;
				sched_local.stash.ev_index = 0;
				sched_local.stash.queue    = handle;
				ret = copy_from_stash(out_ev, max_num);
			} else {
				sched_local.stash.num_ev = 0;
				ret = num;
			}

			/* Output the source queue handle */
			if (out_queue)
				*out_queue = handle;

			return ret;
		}
	}

	return 0;
}

/*
 * Schedule queues
 */
static inline int do_schedule(odp_queue_t *out_queue, odp_event_t out_ev[],
			      unsigned int max_num)
{
	int i, num_grp, ret, spr, grp_id;
	uint32_t sched_round;
	uint16_t spread_round, grp_round;
	uint32_t epoch;
	int balance = 0;

	if (sched_local.stash.num_ev) {
		ret = copy_from_stash(out_ev, max_num);

		if (out_queue)
			*out_queue = sched_local.stash.queue;

		return ret;
	}

	/* Release schedule context */
	if (sched_local.sync_ctx == ODP_SCHED_SYNC_ATOMIC)
		release_atomic();
	else if (sched_local.sync_ctx == ODP_SCHED_SYNC_ORDERED)
		release_ordered();

	if (odp_unlikely(sched_local.pause))
		return 0;

	sched_round = sched_local.sched_round++;
	grp_round   = sched_round & (GRP_WEIGHT_TBL_SIZE - 1);

	/* Each thread prefers a priority queue. Spread weight table avoids
	 * starvation of other priority queues on low thread counts. */
	spread_round = sched_local.spread_round;

	if (odp_likely(sched->load_balance)) {
		/* Spread balance is checked max_spread times in every BALANCE_ROUNDS_M1 + 1
		 * scheduling rounds. */
		if (odp_unlikely(sched_local.balance_on)) {
			balance = 1;

			if (sched_local.balance_start == spread_round)
				sched_local.balance_on = 0;
		}

		if (odp_unlikely((sched_round & BALANCE_ROUNDS_M1) == 0)) {
			sched_local.balance_start = spread_round;
			sched_local.balance_on    = 1;
		}
	}

	if (odp_unlikely(spread_round + 1 >= sched->max_spread))
		sched_local.spread_round = 0;
	else
		sched_local.spread_round = spread_round + 1;

	spr = sched_local.spread_tbl[spread_round];

	epoch = odp_atomic_load_acq_u32(&sched->grp_epoch);
	num_grp = sched_local.num_grp;

	if (odp_unlikely(sched_local.grp_epoch != epoch)) {
		num_grp = grp_update_tbl();
		sched_local.grp_epoch = epoch;
	}

	grp_id = sched_local.grp_weight[grp_round];

	/* Schedule queues per group and priority */
	for (i = 0; i < num_grp; i++) {
		int grp;

		grp = sched_local.grp[grp_id];
		ret = do_schedule_grp(out_queue, out_ev, max_num, grp, spr, balance);

		if (odp_likely(ret))
			return ret;

		grp_id++;
		if (odp_unlikely(grp_id >= num_grp))
			grp_id = 0;
	}

	return 0;
}

static inline int schedule_run(odp_queue_t *out_queue, odp_event_t out_ev[],
			       unsigned int max_num)
{
	timer_run(1);

	return do_schedule(out_queue, out_ev, max_num);
}

static inline int schedule_loop(odp_queue_t *out_queue, uint64_t wait,
				odp_event_t out_ev[], unsigned int max_num)
{
	odp_time_t next, wtime;
	int first = 1;
	int ret;

	while (1) {
		ret = do_schedule(out_queue, out_ev, max_num);
		if (ret) {
			timer_run(2);
			break;
		}
		timer_run(1);

		if (wait == ODP_SCHED_WAIT)
			continue;

		if (wait == ODP_SCHED_NO_WAIT)
			break;

		if (first) {
			wtime = odp_time_local_from_ns(wait);
			next = odp_time_sum(odp_time_local(), wtime);
			first = 0;
			continue;
		}

		if (odp_time_cmp(next, odp_time_local()) < 0)
			break;
	}

	return ret;
}

static odp_event_t schedule(odp_queue_t *out_queue, uint64_t wait)
{
	odp_event_t ev;

	ev = ODP_EVENT_INVALID;

	schedule_loop(out_queue, wait, &ev, 1);

	return ev;
}

static int schedule_multi(odp_queue_t *out_queue, uint64_t wait,
			  odp_event_t events[], int num)
{
	return schedule_loop(out_queue, wait, events, num);
}

static int schedule_multi_no_wait(odp_queue_t *out_queue, odp_event_t events[],
				  int num)
{
	return schedule_run(out_queue, events, num);
}

static int schedule_multi_wait(odp_queue_t *out_queue, odp_event_t events[],
			       int num)
{
	int ret;

	do {
		ret = schedule_run(out_queue, events, num);
	} while (ret == 0);

	return ret;
}

static inline void order_lock(void)
{
	if (sched_local.sync_ctx != ODP_SCHED_SYNC_ORDERED)
		return;

	wait_for_order(sched_local.ordered.src_queue);
}

static void order_unlock(void)
{
	/* Nothing to do */
}

static void schedule_order_lock(uint32_t lock_index)
{
	odp_atomic_u64_t *ord_lock;
	uint32_t queue_index;

	if (sched_local.sync_ctx != ODP_SCHED_SYNC_ORDERED)
		return;

	queue_index = sched_local.ordered.src_queue;

	ODP_ASSERT(lock_index <= sched->queue[queue_index].order_lock_count &&
		   !sched_local.ordered.lock_called.u8[lock_index]);

	ord_lock = &sched->order[queue_index].lock[lock_index];

	/* Busy loop to synchronize ordered processing */
	while (1) {
		uint64_t lock_seq;

		lock_seq = odp_atomic_load_acq_u64(ord_lock);

		if (lock_seq == sched_local.ordered.ctx) {
			sched_local.ordered.lock_called.u8[lock_index] = 1;
			return;
		}
		odp_cpu_pause();
	}
}

static void schedule_order_unlock(uint32_t lock_index)
{
	odp_atomic_u64_t *ord_lock;
	uint32_t queue_index;

	if (sched_local.sync_ctx != ODP_SCHED_SYNC_ORDERED)
		return;

	queue_index = sched_local.ordered.src_queue;

	ODP_ASSERT(lock_index <= sched->queue[queue_index].order_lock_count);

	ord_lock = &sched->order[queue_index].lock[lock_index];

	ODP_ASSERT(sched_local.ordered.ctx == odp_atomic_load_u64(ord_lock));

	odp_atomic_store_rel_u64(ord_lock, sched_local.ordered.ctx + 1);
}

static void schedule_order_unlock_lock(uint32_t unlock_index,
				       uint32_t lock_index)
{
	schedule_order_unlock(unlock_index);
	schedule_order_lock(lock_index);
}

static void schedule_order_lock_start(uint32_t lock_index)
{
	(void)lock_index;
}

static void schedule_order_lock_wait(uint32_t lock_index)
{
	schedule_order_lock(lock_index);
}

static void schedule_pause(void)
{
	sched_local.pause = 1;
}

static void schedule_resume(void)
{
	sched_local.pause = 0;
}

static uint64_t schedule_wait_time(uint64_t ns)
{
	return ns;
}

static inline void spread_thrs_inc(odp_schedule_group_t group, int thr_tbl[], int count)
{
	int thr, i;
	uint8_t spread;

	for (i = 0; i < count; i++) {
		thr = thr_tbl[i];
		spread = spread_from_index(thr);
		sched->sched_grp[group].spread_thrs[spread]++;
	}
}

static inline void spread_thrs_dec(odp_schedule_group_t group, int thr_tbl[], int count)
{
	int thr, i;
	uint8_t spread;

	for (i = 0; i < count; i++) {
		thr = thr_tbl[i];
		spread = spread_from_index(thr);
		sched->sched_grp[group].spread_thrs[spread]--;
	}
}

static inline int threads_from_mask(int thr_tbl[], int count, const odp_thrmask_t *mask)
{
	int i;
	int thr = odp_thrmask_first(mask);

	for (i = 0; i < count; i++) {
		if (thr < 0) {
			ODP_ERR("No more threads in the mask\n");
			return -1;
		}

		thr_tbl[i] = thr;
		thr = odp_thrmask_next(mask, thr);
	}

	return 0;
}

static odp_schedule_group_t schedule_group_create(const char *name,
						  const odp_thrmask_t *mask)
{
	odp_schedule_group_t group = ODP_SCHED_GROUP_INVALID;
	int count, i;

	count = odp_thrmask_count(mask);
	if (count < 0) {
		ODP_ERR("Bad thread count\n");
		return ODP_SCHED_GROUP_INVALID;
	}

	int thr_tbl[count];

	if (count && threads_from_mask(thr_tbl, count, mask))
		return ODP_SCHED_GROUP_INVALID;

	odp_ticketlock_lock(&sched->grp_lock);

	for (i = SCHED_GROUP_NAMED; i < NUM_SCHED_GRPS; i++) {
		if (!sched->sched_grp[i].allocated) {
			char *grp_name = sched->sched_grp[i].name;

			if (name == NULL) {
				grp_name[0] = 0;
			} else {
				strncpy(grp_name, name,
					ODP_SCHED_GROUP_NAME_LEN - 1);
				grp_name[ODP_SCHED_GROUP_NAME_LEN - 1] = 0;
			}

			grp_update_mask(i, mask);
			group = (odp_schedule_group_t)i;
			spread_thrs_inc(group, thr_tbl, count);
			sched->sched_grp[i].allocated = 1;
			break;
		}
	}

	odp_ticketlock_unlock(&sched->grp_lock);
	return group;
}

static int schedule_group_destroy(odp_schedule_group_t group)
{
	odp_thrmask_t zero;
	int i;

	if (group >= NUM_SCHED_GRPS || group < SCHED_GROUP_NAMED) {
		ODP_ERR("Bad group %i\n", group);
		return -1;
	}

	odp_thrmask_zero(&zero);

	odp_ticketlock_lock(&sched->grp_lock);

	if (sched->sched_grp[group].allocated == 0) {
		odp_ticketlock_unlock(&sched->grp_lock);
		ODP_ERR("Group not created: %i\n", group);
		return -1;
	}

	grp_update_mask(group, &zero);

	for (i = 0; i < MAX_SPREAD; i++)
		sched->sched_grp[group].spread_thrs[i] = 0;

	memset(sched->sched_grp[group].name, 0, ODP_SCHED_GROUP_NAME_LEN);
	sched->sched_grp[group].allocated = 0;

	odp_ticketlock_unlock(&sched->grp_lock);
	return 0;
}

static odp_schedule_group_t schedule_group_lookup(const char *name)
{
	odp_schedule_group_t group = ODP_SCHED_GROUP_INVALID;
	int i;

	odp_ticketlock_lock(&sched->grp_lock);

	for (i = SCHED_GROUP_NAMED; i < NUM_SCHED_GRPS; i++) {
		if (strcmp(name, sched->sched_grp[i].name) == 0) {
			group = (odp_schedule_group_t)i;
			break;
		}
	}

	odp_ticketlock_unlock(&sched->grp_lock);
	return group;
}

static int schedule_group_join(odp_schedule_group_t group, const odp_thrmask_t *mask)
{
	int i, count, thr;
	odp_thrmask_t new_mask;

	if (group >= NUM_SCHED_GRPS || group < SCHED_GROUP_NAMED) {
		ODP_ERR("Bad group %i\n", group);
		return -1;
	}

	count = odp_thrmask_count(mask);
	if (count <= 0) {
		ODP_ERR("No threads in the mask\n");
		return -1;
	}

	int thr_tbl[count];

	thr = odp_thrmask_first(mask);
	for (i = 0; i < count; i++) {
		if (thr < 0) {
			ODP_ERR("No more threads in the mask\n");
			return -1;
		}

		thr_tbl[i] = thr;
		thr = odp_thrmask_next(mask, thr);
	}

	odp_ticketlock_lock(&sched->grp_lock);

	if (sched->sched_grp[group].allocated == 0) {
		odp_ticketlock_unlock(&sched->grp_lock);
		ODP_ERR("Bad group status\n");
		return -1;
	}

	spread_thrs_inc(group, thr_tbl, count);

	odp_thrmask_or(&new_mask, &sched->sched_grp[group].mask, mask);
	grp_update_mask(group, &new_mask);

	odp_ticketlock_unlock(&sched->grp_lock);
	return 0;
}

static int schedule_group_leave(odp_schedule_group_t group, const odp_thrmask_t *mask)
{
	int i, count, thr;
	odp_thrmask_t new_mask;

	if (group >= NUM_SCHED_GRPS || group < SCHED_GROUP_NAMED) {
		ODP_ERR("Bad group %i\n", group);
		return -1;
	}

	count = odp_thrmask_count(mask);
	if (count <= 0) {
		ODP_ERR("No threads in the mask\n");
		return -1;
	}

	int thr_tbl[count];

	thr = odp_thrmask_first(mask);
	for (i = 0; i < count; i++) {
		if (thr < 0) {
			ODP_ERR("No more threads in the mask\n");
			return -1;
		}

		thr_tbl[i] = thr;
		thr = odp_thrmask_next(mask, thr);
	}

	odp_thrmask_xor(&new_mask, mask, &sched->mask_all);

	odp_ticketlock_lock(&sched->grp_lock);

	if (sched->sched_grp[group].allocated == 0) {
		odp_ticketlock_unlock(&sched->grp_lock);
		ODP_ERR("Bad group status\n");
		return -1;
	}

	spread_thrs_dec(group, thr_tbl, count);

	odp_thrmask_and(&new_mask, &sched->sched_grp[group].mask, &new_mask);
	grp_update_mask(group, &new_mask);

	odp_ticketlock_unlock(&sched->grp_lock);
	return 0;
}

static int schedule_group_thrmask(odp_schedule_group_t group,
				  odp_thrmask_t *thrmask)
{
	int ret;

	odp_ticketlock_lock(&sched->grp_lock);

	if (group < NUM_SCHED_GRPS && sched->sched_grp[group].allocated) {
		*thrmask = sched->sched_grp[group].mask;
		ret = 0;
	} else {
		ret = -1;
	}

	odp_ticketlock_unlock(&sched->grp_lock);
	return ret;
}

static int schedule_group_info(odp_schedule_group_t group,
			       odp_schedule_group_info_t *info)
{
	int ret;

	odp_ticketlock_lock(&sched->grp_lock);

	if (group < NUM_SCHED_GRPS && sched->sched_grp[group].allocated) {
		info->name    = sched->sched_grp[group].name;
		info->thrmask = sched->sched_grp[group].mask;
		ret = 0;
	} else {
		ret = -1;
	}

	odp_ticketlock_unlock(&sched->grp_lock);
	return ret;
}

static int schedule_thr_add(odp_schedule_group_t group, int thr)
{
	odp_thrmask_t mask;
	odp_thrmask_t new_mask;

	if (group < 0 || group >= SCHED_GROUP_NAMED)
		return -1;

	odp_thrmask_zero(&mask);
	odp_thrmask_set(&mask, thr);

	odp_ticketlock_lock(&sched->grp_lock);

	if (!sched->sched_grp[group].allocated) {
		odp_ticketlock_unlock(&sched->grp_lock);
		return 0;
	}

	odp_thrmask_or(&new_mask, &sched->sched_grp[group].mask, &mask);
	spread_thrs_inc(group, &thr, 1);
	grp_update_mask(group, &new_mask);

	odp_ticketlock_unlock(&sched->grp_lock);

	return 0;
}

static int schedule_thr_rem(odp_schedule_group_t group, int thr)
{
	odp_thrmask_t mask;
	odp_thrmask_t new_mask;

	if (group < 0 || group >= SCHED_GROUP_NAMED)
		return -1;

	odp_thrmask_zero(&mask);
	odp_thrmask_set(&mask, thr);
	odp_thrmask_xor(&new_mask, &mask, &sched->mask_all);

	odp_ticketlock_lock(&sched->grp_lock);

	if (!sched->sched_grp[group].allocated) {
		odp_ticketlock_unlock(&sched->grp_lock);
		return 0;
	}

	odp_thrmask_and(&new_mask, &sched->sched_grp[group].mask, &new_mask);
	spread_thrs_dec(group, &thr, 1);
	grp_update_mask(group, &new_mask);

	odp_ticketlock_unlock(&sched->grp_lock);

	return 0;
}

static void schedule_prefetch(int num)
{
	(void)num;
}

static int schedule_num_grps(void)
{
	return NUM_SCHED_GRPS - SCHED_GROUP_NAMED;
}

static void schedule_get_config(schedule_config_t *config)
{
	*config = sched->config_if;
}

static int schedule_capability(odp_schedule_capability_t *capa)
{
	memset(capa, 0, sizeof(odp_schedule_capability_t));

	capa->max_ordered_locks = schedule_max_ordered_locks();
	capa->max_groups = schedule_num_grps();
	capa->max_prios = schedule_num_prio();
	capa->max_queues = sched->max_queues;
	capa->max_queue_size = _odp_queue_glb->config.max_queue_size;
	capa->max_flow_id = BUF_HDR_MAX_FLOW_ID;

	return 0;
}

static void schedule_print(void)
{
	int spr, prio, grp;
	uint32_t num_queues, num_active;
	ring_u32_t *ring;
	odp_schedule_capability_t capa;
	int num_spread = sched->config.num_spread;
	const int col_width = 24;

	(void)schedule_capability(&capa);

	ODP_PRINT("\nScheduler debug info\n");
	ODP_PRINT("--------------------\n");
	ODP_PRINT("  scheduler:         basic\n");
	ODP_PRINT("  max groups:        %u\n", capa.max_groups);
	ODP_PRINT("  max priorities:    %u\n", capa.max_prios);
	ODP_PRINT("  num spread:        %i\n", num_spread);
	ODP_PRINT("  prefer ratio:      %u\n", sched->config.prefer_ratio);
	ODP_PRINT("\n");

	ODP_PRINT("  Number of active event queues:\n");
	ODP_PRINT("              spread\n");
	ODP_PRINT("            ");

	for (spr = 0; spr < num_spread; spr++)
		ODP_PRINT(" %7i", spr);

	ODP_PRINT("\n");

	for (prio = 0; prio < NUM_PRIO; prio++) {
		ODP_PRINT("  prio %i", prio);

		for (grp = 0; grp < NUM_SCHED_GRPS; grp++)
			if (sched->prio_q_mask[grp][prio])
				break;

		if (grp == NUM_SCHED_GRPS) {
			ODP_PRINT(":-\n");
			continue;
		}

		ODP_PRINT("\n");

		for (grp = 0; grp < NUM_SCHED_GRPS; grp++) {
			if (sched->sched_grp[grp].allocated == 0)
				continue;

			ODP_PRINT("    group %i:", grp);

			for (spr = 0; spr < num_spread; spr++) {
				num_queues = sched->prio_q_count[grp][prio][spr];
				ring = &sched->prio_q[grp][prio][spr].ring;
				num_active = ring_u32_len(ring);
				ODP_PRINT(" %3u/%3u", num_active, num_queues);
			}
			ODP_PRINT("\n");
		}
	}

	ODP_PRINT("\n  Number of threads per schedule group:\n");
	ODP_PRINT("             name                     spread\n");

	for (grp = 0; grp < NUM_SCHED_GRPS; grp++) {
		if (sched->sched_grp[grp].allocated == 0)
			continue;

		ODP_PRINT("    group %i: %-*s", grp, col_width, sched->sched_grp[grp].name);

		for (spr = 0; spr < num_spread; spr++)
			ODP_PRINT(" %u", sched->sched_grp[grp].spread_thrs[spr]);

		ODP_PRINT("\n");
	}

	ODP_PRINT("\n");
}

/* Returns spread for queue debug prints */
int _odp_sched_basic_get_spread(uint32_t queue_index)
{
	return sched->queue[queue_index].spread;
}

/* Fill in scheduler interface */
const schedule_fn_t _odp_schedule_basic_fn = {
	.pktio_start = schedule_pktio_start,
	.thr_add = schedule_thr_add,
	.thr_rem = schedule_thr_rem,
	.num_grps = schedule_num_grps,
	.create_queue = schedule_create_queue,
	.destroy_queue = schedule_destroy_queue,
	.sched_queue = schedule_sched_queue,
	.ord_enq_multi = schedule_ord_enq_multi,
	.init_global = schedule_init_global,
	.term_global = schedule_term_global,
	.init_local  = schedule_init_local,
	.term_local  = schedule_term_local,
	.order_lock = order_lock,
	.order_unlock = order_unlock,
	.max_ordered_locks = schedule_max_ordered_locks,
	.get_config = schedule_get_config
};

/* Fill in scheduler API calls */
const schedule_api_t _odp_schedule_basic_api = {
	.schedule_wait_time       = schedule_wait_time,
	.schedule_capability      = schedule_capability,
	.schedule_config_init     = schedule_config_init,
	.schedule_config          = schedule_config,
	.schedule                 = schedule,
	.schedule_multi           = schedule_multi,
	.schedule_multi_wait      = schedule_multi_wait,
	.schedule_multi_no_wait   = schedule_multi_no_wait,
	.schedule_pause           = schedule_pause,
	.schedule_resume          = schedule_resume,
	.schedule_release_atomic  = schedule_release_atomic,
	.schedule_release_ordered = schedule_release_ordered,
	.schedule_prefetch        = schedule_prefetch,
	.schedule_min_prio        = schedule_min_prio,
	.schedule_max_prio        = schedule_max_prio,
	.schedule_default_prio    = schedule_default_prio,
	.schedule_num_prio        = schedule_num_prio,
	.schedule_group_create    = schedule_group_create,
	.schedule_group_destroy   = schedule_group_destroy,
	.schedule_group_lookup    = schedule_group_lookup,
	.schedule_group_join      = schedule_group_join,
	.schedule_group_leave     = schedule_group_leave,
	.schedule_group_thrmask   = schedule_group_thrmask,
	.schedule_group_info      = schedule_group_info,
	.schedule_order_lock      = schedule_order_lock,
	.schedule_order_unlock    = schedule_order_unlock,
	.schedule_order_unlock_lock = schedule_order_unlock_lock,
	.schedule_order_lock_start  = schedule_order_lock_start,
	.schedule_order_lock_wait   = schedule_order_lock_wait,
	.schedule_print           = schedule_print
};
