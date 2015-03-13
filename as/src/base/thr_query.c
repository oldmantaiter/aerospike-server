/*
 * thr_query.c
 *
 * Copyright (C) 2012-2014 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

/*
 * This code is responsible for the query execution. Each query received
 * query transaction for the query threads to execute. Query has two parts
 * a) Generator  : This query the Aerospike Index B-tree and creates the digest list and
 *                 queues it up for LOOKUP / UDF / AGGREGATION / MRJ
 * b) Aggregator : This does required processing of the record and send back
 *                 response to the clients.
 *                 LOOKUP:      Read the record from the disk and based on the
 *                              records selected by query packs it into the buffer
 *                              and returns it back to the client
 *                 UDF:         Reads the record from the disk and based on the
 *                              query applies UDF and packs the result back into
 *                              the buffer and returns it back to the client.
 *                 AGGREGATION: Creates istream(on the digstlist) and ostream(
 *                              over the network buffer) and applies aggregator
 *                              functions. For a single query this can be called
 *                              multiple times. The istream interface takes care
 *                              of partition reservation / record opening/ closing
 *                              and object lock synchronization. Whole of which
 *                              is driven by as_stream_read / as_stream_write from
 *                              inside aggregation UDF. ostream keeps sending by
 *                              batched result to the client.
 *                 MRJ        : Creates istream(on the digest list) and ostream
 *                              (over the queue of mapped result throttled one)
 *                              which can then feed into next level of function.
 *                              Modalities <TBD>
 *
 *  Please note all these parts can either be performed under single thread
 *  context or by different set of threads. For the namespace with data on disk
 *  I/O is performed separately in different set of I/O pools
 *
 *  Flow of code looks like
 *
 *  1. thr_tsvc()
 *
 *                 ---------------------------------> as_query__generator
 *                /                                      /|\      |
 *  as_query -----                                        |       |   qtr released
 * (sets up qtr)  \   qtr reserved                        |      \|/
 *                 ----------------> g_query_q ------> as_query__th
 *
 *
 *  2. Query Threads
 *                          ---------------------------------> as_query__process_request
 *                        /                                          /|\      |
 *  as_query__generator --                                            |       |  qtr released
 *  (sets up qreq)        \  qtr reserved                             |      \|/
 *                          --------------> g_query_request_queue -> as_query__th
 *
 *
 *
 *  3. I/O threads
 *                                as_query__process_ioreq  --> as_query__io
 *                               /
 *  as_query__process_request ----- as_query__process_mrjreq  (NOT IMPLEMENTED)
 *                               \
 *                                as_query__process_aggreq --> as_query__agg
 *
 *  (Releases all the resources qtr and qreq if allocated)
 *
 *  A query may be single thread execution or a multi threaded application. In the
 *  single thread execution all the functions are called in the single thread context
 *  and no queue is involved. In case of multi thread context qtr is setup by thr_tsvc
 *  and which is picked up by the query threads which could either service it in single
 *  thread or queue up to the I/O worker thread (done generally in case of data on ssd)
 *
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "aerospike/as_buffer.h"
#include "aerospike/as_integer.h"
#include "aerospike/as_list.h"
#include "aerospike/as_map.h"
#include "aerospike/as_msgpack.h"
#include "aerospike/as_serializer.h"
#include "aerospike/as_stream.h"
#include "aerospike/as_string.h"
#include "aerospike/as_rec.h"
#include "aerospike/as_val.h"
#include "aerospike/mod_lua.h"
#include "citrusleaf/cf_ll.h"

#include "ai.h"
#include "ai_btree.h"
#include "bt.h"
#include "bt_iterator.h"

#include "base/aggr.h"
#include "base/datamodel.h"
#include "base/secondary_index.h"
#include "base/thr_tsvc.h"
#include "base/transaction.h"
#include "base/udf_memtracker.h"
#include "base/udf_rw.h"
#include "base/udf_record.h"
#include "fabric/fabric.h"

// parameter read off from a transaction

extern cf_vector * as_sindex_binlist_from_msg(as_namespace *ns, as_msg *msgp);
extern int as_query__queue(as_query_transaction *qtr);
typedef int (* as_query_ioreq_cb)
		(void *qtr, as_index_ref *r_ref, as_storage_rd *rd);

#define QUERY_BATCH_SIZE              100
#define AS_MAX_NUM_SCRIPT_PARAMS      10
// TODO: make it configurable
#define AS_QUERY_BUF_SIZE             1024 * 128
#define AS_QUERY_MAX_BUFS             256	// That makes it 32 meg max in steady state
#define AS_QUERY_MAX_QREQ             1024	// this is 4 kb

// Cap on query thread and query worker thread some
// number make it smarter
#define AS_QUERY_MAX_THREADS          32
#define AS_QUERY_MAX_WORKER_THREADS   15 * AS_QUERY_MAX_THREADS
#define AS_QUERY_MAX_QREQ_INFLIGHT    100	// worker queue capping per query
#define AS_QUERY_MAX_QUERY            500	// 32 MB be little generous for now!!
#define AS_QUERY_MAX_SHORT_QUEUE_SZ   500	// maximum 500 outstanding short running queries
#define AS_QUERY_MAX_LONG_QUEUE_SZ    500	// maximum 500 outstanding long  running queries
#define AS_QUERY_MAX_UDF_TRANSACTIONS 20	// Higher the value more aggressive it will be

#define QTR_FAILED(qtr) \
	((qtr)->abort || (qtr)->err)

#define AS_QUERY_UNTRACKED_TIME       1000 * 1000 * 1000 // (nanosecond) 1 sec
typedef enum {
	AS_QUERY_LOOKUP = 0,
	AS_QUERY_UDF    = 1,
	AS_QUERY_AGG    = 2,
	AS_QUERY_MRJ    = 3
} as_query_type;

struct as_query_transaction_s {

	// PROPERTIES

	uint64_t          trid;
	as_file_handle  * fd_h;
	as_namespace    * ns;
	char            * setname;
	as_sindex       * si;
	as_sindex_range * srange;
	cl_msg          * msgp;

	// INPUT

	as_query_type     job_type;  // Job type [LOOKUP/AGG/UDF/MRJ]
	cf_vector       * binlist;
	udf_call          call;     // Record UDF Details
	as_aggr_call      agg_call; // Stream UDF Details 
	as_sindex_qctx    qctx;     // Secondary Index details

	// OUTPUT
	int               result_code;
	bool              abort; // user abort rec-count-bound/query-timeout/query-kill
	bool              err;   // Internal system err or parameter err

	// bounds and counters
	int               loop;
	uint64_t          start_time;               // Start time
	uint64_t          end_time;                 // timeout value
	cf_atomic_int     num_records;              // Number of records returned as result
												// if aggregation returns 1 record count
												// is 1, irrelevant of number of record
												// being touched.
	uint32_t          n_digests;                // Digests picked by SIK
												// including record read
	uint64_t          net_io_bytes;
	uint64_t          read_success;
	uint64_t          waiting_time_ns;          // Time spent waiting by query in query_queue
	uint64_t          querying_ai_time_ns;      // Time spent by query to run lookup secondary index trees.
	uint64_t          queued_time_ns;

	// PRIORITY PARAMETERS
	uint32_t          yield_count;              // Number of loops
	cf_atomic32       qreq_in_flight;
	uint32_t          priority;

	// INTERNAL
	as_query_ioreq_cb   req_cb;                 // Call back for the io threads
	uint32_t          buf_reserved;             // for memory tracking
	cf_buf_builder  * bb_r;
	pthread_mutex_t   buf_mutex;
	cf_atomic_int	  udf_runtime_memory_used;  // Currently reserved
												// udf runtime memory
	bool              is_malloc;
	bool			  inited;
	struct ai_obj     bkey;
	bool              short_running;
	bool              track;

	// Record UDF Management
	cf_atomic_int       uit_queued;    				// Throttling: max in flight scan
	cf_atomic_int       uit_completed; 				// Number of udf transactions successfully completed
	cf_atomic64			uit_total_run_time;			// Average transaction processing time for all udf internal transactions
};

typedef enum {
	AS_QUERY_REQTYPE_NONE = -1, // Request for I/O
	AS_QUERY_REQTYPE_IO   =  0, // Request for I/O
	AS_QUERY_REQTYPE_UDF  =  1, // Request for running UDF on query result
	AS_QUERY_REQTYPE_AGG  =  2, // Request for Aggregation
	AS_QUERY_REQTYPE_MRJ  =  3  // Request for MRJ
} as_query_request_type;

typedef struct as_query_request_s {
	as_query_request_type    type;
	byte                     agg_stage;
	as_query_transaction   * qtr;
	cf_ll                  * recl;
} as_query_request;

static cf_atomic32      g_query_init            = 0;
static int              g_current_queries_count = 0;
static pthread_rwlock_t g_query_lock
						= PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP;
pthread_mutex_t         g_query_arr_mutex;

// Query management
static rchash *g_query_job_hash = NULL;

// Histograms

histogram * query_txn_q_wait_hist;               // Histogram to track time spend in trasaction queue
histogram * query_prepare_batch_q_wait_hist;     // Histogram to track time spend waiting in queue for query thread
histogram * query_prepare_batch_hist;            // Histogram to track time spend while preparing batches
histogram * query_batch_io_q_wait_hist;          // Histogram to track time spend waiting in queue for worker thread
histogram * query_batch_io_hist;                 // Histogram to track time spend doing I/O per batch
histogram * query_net_io_hist;                   // Histogram to track time spend sending results to client

#define QUERY_HIST_INSERT_DATA_POINT(type, start_time_ns)              \
do {                                                                   \
	if (g_config.query_enable_histogram && start_time_ns != 0) {       \
		if (type) {                                                    \
			histogram_insert_data_point(type, start_time_ns);          \
		}                                                              \
	}                                                                  \
} while(0);


// forward declarations

// Buf Builder Pool
static cf_queue  * g_query_response_bb_pool  = 0;
int                as_query__bb_poolrelease(cf_buf_builder *bb_r);
cf_buf_builder   * as_query__bb_poolrequest();

// Query request
static cf_queue  * g_query_qreq_pool         = 0;
int                as_query__qreq_poolrelease(as_query_request *qreq);
as_query_request * as_query__qreq_poolrequest();


// GENERATOR
static pthread_t      g_query_threads[AS_QUERY_MAX_THREADS];
static pthread_attr_t g_query_th_attr;
static cf_queue     * g_query_queue           = 0;
static cf_queue     * g_query_long_queue      = 0;
static cf_atomic32    g_query_threadcnt       = 0;
inline void           as_query__generator(as_query_transaction *qtr);
void                * as_query__th(void* q_to_wait_on);

// I/O & AGGREGATOR
static pthread_t      g_query_worker_threads[AS_QUERY_MAX_WORKER_THREADS];
static pthread_attr_t g_query_worker_th_attr;
static cf_queue     * g_query_request_queue    = 0;
static cf_atomic32    g_query_worker_threadcnt = 0;
static int            as_query__push_qreq(as_query_request *qreq);
as_query_request    * as_query__pop_qreq();
void                * as_query__worker_th(void *q_to_wait_on);
int                   as_query__process_ioreq(as_query_request *qio);
int                   as_query__process_udfreq(as_query_request *qudf);
int                   as_query__process_aggreq(as_query_request *qagg);
int                   as_query__process_request(as_query_request *qreqp);

// Client Response Functions
int                  as_query__add_fin(as_query_transaction *qtr);
int                  as_query__send_response(as_query_transaction *qtr);
int                  as_query__add_response(void *qtr,
							as_index_ref *r_ref, as_storage_rd *rd);

// Query transaction functions
void                 as_query__transaction_done(as_query_transaction *qtr);
int                  as_qtr__release(as_query_transaction *qtr, char *fname, int lineno);
int                  as_qtr__reserve(as_query_transaction *qtr, char *fname, int lineno);

#define AS_QUERY_INCREMENT_ERR_COUNT(qtr)   \
	if(qtr->job_type == AS_QUERY_AGG) {    \
		cf_atomic64_incr(&(qtr->si->stats.agg_errs));    \
		cf_atomic64_incr(&g_config.n_agg_errs);         \
	}    \
	else if(qtr->job_type == AS_QUERY_LOOKUP)    \
	{   \
		cf_atomic64_incr(&(qtr->si->stats.lookup_errs));    \
		cf_atomic64_incr(&g_config.n_lookup_errs);    \
	}

#define AS_QUERY_INCREMENT_ABORT_COUNT(qtr)   \
	if(qtr->job_type == AS_QUERY_AGG) {    \
		cf_atomic64_incr(&g_config.n_agg_abort);    \
	}    \
	else if(qtr->job_type == AS_QUERY_LOOKUP)    \
	{   \
		cf_atomic64_incr(&g_config.n_lookup_abort);    \
	}

static inline void
as_query__update_stats(as_query_transaction *qtr)
{
	int rows = qtr->num_records;

	if (qtr->abort) {
		AS_QUERY_INCREMENT_ABORT_COUNT(qtr);
	}
	else if (qtr->err) {
		AS_QUERY_INCREMENT_ERR_COUNT(qtr);
	}

	if( qtr->job_type == AS_QUERY_AGG) {
		if (!QTR_FAILED(qtr))
			cf_atomic64_incr(&g_config.n_agg_success);
		cf_atomic64_incr(&qtr->si->stats.n_aggregation);
		cf_atomic64_add(&qtr->si->stats.agg_response_size, qtr->buf_reserved);
		cf_atomic64_add(&qtr->si->stats.agg_num_records, qtr->num_records);
		cf_atomic64_add(&g_config.agg_response_size, qtr->buf_reserved);
		cf_atomic64_add(&g_config.agg_num_records, qtr->num_records);
	}
	else if( qtr->job_type == AS_QUERY_LOOKUP )
	{
		if (!QTR_FAILED(qtr))
			cf_atomic64_incr(&g_config.n_lookup_success);
		cf_atomic64_incr(&qtr->si->stats.n_lookup);
		cf_atomic64_add(&qtr->si->stats.lookup_response_size, qtr->buf_reserved);
		cf_atomic64_add(&qtr->si->stats.lookup_num_records, qtr->num_records);
		cf_atomic64_add(&g_config.lookup_response_size, qtr->buf_reserved);
		cf_atomic64_add(&g_config.lookup_num_records, qtr->num_records);
	}
	cf_hist_track_insert_raw(g_config.q_rcnt_hist, rows);
	cf_hist_track_insert_data_point(g_config.q_hist, qtr->start_time);
	SINDEX_HIST_INSERT_DATA_POINT(qtr->si, query_hist, qtr->start_time);
	SINDEX_HIST_INSERT_RAW(qtr->si, query_rcnt_hist, qtr->n_digests);
	SINDEX_HIST_INSERT_RAW(qtr->si, query_diff_hist, qtr->n_digests - qtr->num_records);

	QUERY_HIST_INSERT_DATA_POINT(query_prepare_batch_hist, qtr->querying_ai_time_ns);
	QUERY_HIST_INSERT_DATA_POINT(query_prepare_batch_q_wait_hist, qtr->waiting_time_ns);

	uint64_t query_stop_time = cf_getns();
	uint64_t elapsed_us = (query_stop_time - qtr->start_time) / 1000;
	cf_detail(AS_QUERY,
			"Total time elapsed %"PRIu64" us, %d of %d read operations avg latency %"PRIu64" us",
			elapsed_us, rows, qtr->n_digests, rows > 0 ? elapsed_us / rows : 0);
}

#define AS_QUERY_PROCESS_INLINE(qtr) \
	(g_config.query_req_in_query_thread        \
	|| (((qtr))                      \
	   && ((cf_atomic32_get((qtr)->qreq_in_flight) > g_config.query_req_max_inflight) \
			|| ((qtr)->ns->storage_data_in_memory)))) \
	? true                       \
	: false

static int query_aerospike_log(const as_aerospike * a, const char * file, const int line, const int lvl, const char * msg) {
	cf_fault_event(AS_QUERY, lvl, file, NULL, line, (char *) msg);
	return 0;
}

static const as_aerospike_hooks query_aerospike_hooks = {
	.open_subrec      = NULL,
	.close_subrec     = NULL,
	.update_subrec    = NULL,
	.create_subrec    = NULL,
	.rec_update       = NULL,
	.rec_remove       = NULL,
	.rec_exists       = NULL,
	.log              = query_aerospike_log,
	.get_current_time = NULL,
	.destroy          = NULL
};

// INTERNAL FUNCTIONS:
int
ll_recl_reduce_fn(cf_ll_element *ele, void *udata)
{
	return CF_LL_REDUCE_DELETE;
}

void
ll_recl_destroy_fn(cf_ll_element *ele)
{
	ll_recl_element * node = (ll_recl_element *) ele;
	if (node) {
		if (node->dig_arr) {
			cf_free(node->dig_arr);
		}
		cf_free(node);
	}
}

void
as_query_histogram_dumpall()
{
	if (g_config.query_enable_histogram == false) 
	{
		return;
	}

	if (query_txn_q_wait_hist) {
		histogram_dump(query_txn_q_wait_hist);
	}
	if (query_prepare_batch_q_wait_hist) {
		histogram_dump(query_prepare_batch_q_wait_hist);
	}
	if (query_prepare_batch_hist) {
		histogram_dump(query_prepare_batch_hist);
	}
	if (query_batch_io_q_wait_hist) {
		histogram_dump(query_batch_io_q_wait_hist);
	}
	if (query_batch_io_hist) {
		histogram_dump(query_batch_io_hist);
	}
	if (query_net_io_hist) {
		histogram_dump(query_net_io_hist);
	}
}

void
as_query_set_job_tracking(bool enable)
{
	// This is not ref counted but is protected under MUTEX lock for
	// read
	pthread_mutex_lock(&g_query_arr_mutex);
	if (enable && !g_config.query_job_tracking) {
		g_current_queries_count = 0;
		g_config.query_job_tracking = true;
	} else if (!enable && g_config.query_job_tracking) {
		g_current_queries_count = 0;
		g_config.query_job_tracking = false;
	}
	pthread_mutex_unlock(&g_query_arr_mutex);
}

uint32_t
query_job_trid_hash(void *value, uint32_t keylen)
{
	return( *(uint32_t *)value);
}


static void
as_query__release_fd(as_query_transaction *qtr)
{
	if (qtr && qtr->fd_h) {
		qtr->fd_h->fh_info &= ~FH_INFO_DONOT_REAP;
		AS_RELEASE_FILE_HANDLE(qtr->fd_h);
		qtr->fd_h = NULL;
	}
}

/* Requirements
 *  Needed for performing operation on running queries.
 *
 * Arguments
 * 	qtr --> To put qtr in a global hash
 *
 * Returns:
 * 	AS_QUERY_OK     - qtr is successfully added into the rchash.
 *	non-zero values - Otherwise
 *
 */
int
as_query__put_qtr(as_query_transaction * qtr)
{
	if (!qtr->track) {
		return AS_QUERY_CONTINUE;
	}

	int rc = rchash_put_unique(g_query_job_hash, &qtr->trid, sizeof(qtr->trid), qtr);
	if (rc) {
		cf_warning(AS_SINDEX, "QTR Put in hash failed with error %d", rc);
	} else {
		cf_atomic64_incr(&g_config.query_tracked);
	}

	return rc;
}

/* Requirements
 *  Needed for performing operation on running queries.
 *
 * Arguments
 * 	qtr --> To get qtr from the global hash
 *
 * Returns:
 * 	AS_QUERY_OK     - qtr is successfully found in the global rchash.
 *	non-zero values - Otherwise
 *
 */
int
as_query__get_qtr(uint64_t trid, as_query_transaction ** qtr)
{
	int rv = rchash_get(g_query_job_hash, &trid, sizeof(trid), (void **) qtr);
	if (RCHASH_OK != rv) {
		cf_info(AS_SCAN, "Query job with transaction id [%"PRIu64"] does not exist", trid );
	}
	return rv;
}

/* Requirements
 *  Needed for performing operation on running queries.
 *
 * Arguments
 * 	qtr --> To delete qtr from the global hash
 *
 * Returns:
 * 	AS_QUERY_OK     - qtr is successfully deleted from the rchash.
 *	non-zero values - Otherwise
 *
 */
int
as_query__delete_qtr(as_query_transaction *qtr)
{
	if (!qtr->track) {
		return AS_QUERY_CONTINUE;
	}

	int rv = rchash_delete(g_query_job_hash, &qtr->trid, sizeof(qtr->trid));
	if (RCHASH_OK != rv) {
		cf_warning(AS_SINDEX, "Failed to delete qtr from query hash.");
	}
	return rv;
}

/*
 * Function as_query__bb_poolrelease
 *
 * Returns -
 * 		AS_QUERY_OK  - On success.
 * 		AS_QUERY_ERR - On failure.
 *
 */
int
as_query__bb_poolrelease(cf_buf_builder *bb_r)
{
	int ret = AS_QUERY_OK;
	if ((cf_queue_sz(g_query_response_bb_pool) > g_config.query_bufpool_size)
			|| g_config.query_buf_size != cf_buf_builder_size(bb_r)) {
		cf_detail(AS_QUERY, "Freed Buffer of Size %d with", bb_r->alloc_sz + sizeof(as_msg));
		cf_buf_builder_free(bb_r);
	} else {
		cf_detail(AS_QUERY, "Pushed %p %d %d ", bb_r, g_config.query_buf_size, cf_buf_builder_size(bb_r));
		ret = cf_queue_push(g_query_response_bb_pool, &bb_r);
		if (ret != CF_QUEUE_OK) {
			cf_warning(AS_QUERY, "Failed to release bb into the pool.. freeing it ");
			cf_buf_builder_free(bb_r);
		}
	}
	return ret;
}

/*
 * Function as_query__bb_poolrequest
 *
 * Returns -
 * 		cf_buf_builder *bb_r
 *
 * 	Description -
 * 		Tries to pop a buf from the queue/pool.
 * 		If the queue is empty, a new buf is created.
 */
cf_buf_builder *
as_query__bb_poolrequest()
{
	cf_buf_builder *bb_r;
	int rv = cf_queue_pop(g_query_response_bb_pool, &bb_r, CF_QUEUE_NOWAIT);
	if (rv == CF_QUEUE_EMPTY) {
		bb_r = cf_buf_builder_create_size(g_config.query_buf_size);
	} else if (rv == CF_QUEUE_OK) {
		bb_r->used_sz = 0;
		cf_detail(AS_QUERY, "Popped %p", bb_r);
	} else {
		cf_warning(AS_QUERY, "Failed to find response buffer in the pool%d", rv);
		return NULL;
	}
	return bb_r;
};

/*
 * Function as_query__qreq_poolrelease
 *
 * Returns -
 * 		AS_QUERY_OK - In case of success
 * 		AS_QUERY_ERR in case of failure
 */
int
as_query__qreq_poolrelease(as_query_request *qreq)
{
	if (!qreq) return AS_QUERY_OK;
	qreq->qtr   = 0;
	qreq->type  = AS_QUERY_REQTYPE_NONE;
	qreq->agg_stage = -1;

	int ret = AS_QUERY_OK;
	if (cf_queue_sz(g_query_qreq_pool) < AS_QUERY_MAX_QREQ) {
		cf_detail(AS_QUERY, "Pushed qreq %p", qreq);
		ret = cf_queue_push(g_query_qreq_pool, &qreq);
		if (ret != CF_QUEUE_OK) {
			cf_warning(AS_QUERY, "Failed to release bb into the pool.. freeing it ");
			cf_free(qreq);
		}
	} else {
		cf_detail(AS_QUERY, "Freed qreq %p", qreq);
		cf_free(qreq);
	}
	if (ret != CF_QUEUE_OK) ret = AS_QUERY_ERR;
	return ret;
}

as_query_request *
as_query__qreq_poolrequest()
{
	as_query_request *qreq = NULL;
	int rv = cf_queue_pop(g_query_qreq_pool, &qreq, CF_QUEUE_NOWAIT);
	if (rv == CF_QUEUE_EMPTY) {
		qreq = cf_malloc(sizeof(as_query_request));
		if (!qreq) {
			cf_warning(AS_QUERY, "Failed to allocate query request");
			return NULL;
		}
		cf_detail(AS_QUERY, " Created qreq %p", qreq);
		memset(qreq, 0, sizeof(as_query_request));
	} else if (rv == CF_QUEUE_OK) {
		cf_detail(AS_QUERY, " Popped qreq %p", qreq);
	} else {
		cf_warning(AS_QUERY, "Failed to find query request in the pool");
		return NULL;
	}
	qreq->qtr   = 0;
	qreq->type  = AS_QUERY_REQTYPE_NONE;
	qreq->agg_stage = -1;
	return qreq;
};

/*
 * Returns nothing
 * Increments qreq_in_flight
 */

int
as_query__push_qreq(as_query_request *qreq)
{
	// Choose queue carefully here from multiple queues in qtr
	cf_detail(AS_QUERY, "Pushed I/O request [%p,%p]", qreq, qreq->qtr);
	cf_atomic32_incr(&qreq->qtr->qreq_in_flight);
	if (g_config.query_enable_histogram) {
		qreq->qtr->queued_time_ns = cf_getns();
	}
	return cf_queue_push(g_query_request_queue, &qreq);
}

as_query_request *
as_query__pop_qreq()
{
	as_query_request *qreq = NULL;
	if (cf_queue_pop(g_query_request_queue, &qreq, CF_QUEUE_FOREVER) != 0) {
		cf_crash(AS_QUERY, "Failed to pop from Query request worker queue.");
	}
	// Choose queue carefully here from multiple queues in qtr
	cf_detail(AS_QUERY, "Popped I/O request [%p,%p]", qreq, qreq->qtr);
	cf_atomic32_decr(&qreq->qtr->qreq_in_flight);
	return qreq;
}

/*
 * Function as_query__transaction_done
 *
 * Returns -
 * 		nothing
 *
 * Notes -
 * 		Frees the qtr only if it was malloced before.
 *		It is called when ref count of the query becomes zero.
 *
 */
void
as_query__transaction_done(as_query_transaction *qtr)
{
	if (!qtr)
		return;

	if (qtr->uit_queued != 0) {
		cf_warning(AS_QUERY, "QUEUED UDF not equal to zero when query transaction is done");
	}

	// Send out the final data back
	if (qtr->fd_h) {
		as_query__add_fin(qtr);
		uint64_t time_ns        = 0;

		if (g_config.query_enable_histogram) {
			time_ns = cf_getns();
		}

		int brv = as_query__send_response(qtr);

		QUERY_HIST_INSERT_DATA_POINT(query_net_io_hist, time_ns);

		if (brv != AS_QUERY_OK) {
			cf_detail( AS_QUERY,
					"query request: Not sending the fin packet as the connection is closed");
		}
	} else {
		cf_debug( AS_QUERY,
				"query request: Not sending the fin packet as the connection is closed");
	}
	as_query__update_stats(qtr);

	// deleting it from the global hash.
	as_query__delete_qtr(qtr);

	bool do_free = qtr->is_malloc;

	if (qtr->bb_r) {
		as_query__bb_poolrelease(qtr->bb_r);
		qtr->bb_r = NULL;
	}
	cf_detail(AS_QUERY, "Cleaned up qtr %p", qtr);

	as_query__release_fd(qtr);

	if (qtr->srange)      as_sindex_range_free(&qtr->srange);
	if (qtr->si)          AS_SINDEX_RELEASE(qtr->si);
	if (qtr->binlist)     cf_vector_destroy(qtr->binlist);
	if (qtr->setname)     cf_free(qtr->setname);
	if (qtr->msgp)        cf_free(qtr->msgp);
	pthread_mutex_destroy(&qtr->buf_mutex);
	if (do_free) cf_rc_free(qtr);
}


/*
 * Function as_qtr__release
 *
 * Returns -
 * 		AS_QUERY_OK
 *
 * Notes -
 * 		releases the qtr.
 * 		Checks the ref_count == 0
 */
int
as_qtr__release(as_query_transaction *qtr, char *fname, int lineno)
{
	if (qtr) {
		int val = cf_rc_release(qtr);
		cf_detail(AS_QUERY, "Released qtr [%s:%d] %p %d ", fname, lineno, qtr, val);
		if (val == 0) {
			cf_detail(AS_QUERY, "Free qtr ref count is zero");
			as_query__transaction_done(qtr);
		}
	}
	return AS_QUERY_OK;
}

/*
 * Function as_qtr__reserve
 *
 * Returns -
 * 		On success - AS_QUERY_OK
 * 		ON faiilure - AS_QUERY_ERR
 *
 * 	Synchronization -
 * 		Takes a lock before reserving the qtr. why do we need
 * 		it looks unnecessray ??
 */

int
as_qtr__reserve(as_query_transaction *qtr, char *fname, int lineno)
{
	if (!qtr) {
		return AS_QUERY_ERR;
	}
	pthread_rwlock_wrlock(&g_query_lock);
	int val = cf_rc_reserve(qtr);
	pthread_rwlock_unlock(&g_query_lock);
	cf_detail(AS_QUERY, "Reserved qtr [%s:%d] %p %d ", fname, lineno, qtr, val);
	return AS_QUERY_OK;
}

// Split the function up and move function to the correct places like
// proto.c etc.
// This is used by aggregation
int
as_query__add_val_response(void *void_qtr, const as_val *val, bool success)
{
	as_query_transaction *qtr = (as_query_transaction *)void_qtr;
	if( !qtr || qtr->abort )
	{
		cf_debug(AS_QUERY, "Query is in abort state");
		return AS_QUERY_ERR;
	}
	uint32_t msg_sz        = 0;
	as_val_tobuf(val, NULL, &msg_sz);
	if (0 == msg_sz) {
		cf_warning(AS_PROTO, "particle to buf: could not copy data!");
	}


	int ret = 0;

	pthread_mutex_lock(&qtr->buf_mutex);
	cf_buf_builder *bb_r = qtr->bb_r;
	if (bb_r == NULL) {
		// Assert that query is aborted if bb_r is found to be null
		pthread_mutex_unlock(&qtr->buf_mutex);
		return AS_QUERY_ERR;
	}
	if (msg_sz > (bb_r->alloc_sz - bb_r->used_sz)) {
		uint64_t time_ns        = 0;

		if (g_config.query_enable_histogram) {
			time_ns = cf_getns();
		}

		int ret     = as_query__send_response(qtr);
		if (ret != AS_QUERY_OK) {
			pthread_mutex_unlock(&qtr->buf_mutex);
			return ret;
		}

		QUERY_HIST_INSERT_DATA_POINT(query_net_io_hist, time_ns);

		// if sent successfully mark the used_sz as 0, and reuse
		// the buffer
		bb_r->used_sz = 0;
		qtr->buf_reserved = 0;
		cf_detail(AS_QUERY, "Streamed Out");
	}
	qtr->buf_reserved += msg_sz;
	ret = as_msg_make_val_response_bufbuilder(val, &qtr->bb_r, msg_sz, success);
	if (ret != 0) {
		cf_warning(AS_QUERY, "Weird there is space but still the packing failed "
				"available = %d msg size = %d",
				bb_r->alloc_sz - bb_r->used_sz, msg_sz);
	}
	qtr->num_records++;
	pthread_mutex_unlock(&qtr->buf_mutex);
	return ret;
}


/*
 * Function as_query__add_response
 *
 * Returns -
 *		AS_QUERY_OK  - On success.
 *		AS_QUERY_ERR - On failure.
 *
 * Notes -
 *	Basic query call back function. Fills up the client response buffer;
 *	sends out buffer and then
 *	reinitializes the buf for the next set of requests,
 *	In case buffer is full Bail out quick if unable to send response back to client
 *
 *	On success, qtr->num_records is incremented by 1.
 *				qtr->buf_reserved is incremented by msg size
 * Synchronization -
 * 		Takes a lock over qtr->buf
 */
int
as_query__add_response(void *void_qtr, as_index_ref *r_ref, as_storage_rd *rd)
{
	as_record *r = r_ref->r;
	as_query_transaction *qtr = (as_query_transaction *)void_qtr;
	size_t msg_sz = as_msg_response_msgsize(r, rd, false, NULL, false,
			qtr->binlist);
	int ret = 0;

	pthread_mutex_lock(&qtr->buf_mutex);
	cf_buf_builder *bb_r = qtr->bb_r;
	if (bb_r == NULL) {
		// Assert that query is aborted if bb_r is found to be null
		pthread_mutex_unlock(&qtr->buf_mutex);
		return AS_QUERY_ERR;
	}
	if (msg_sz > (bb_r->alloc_sz - bb_r->used_sz)) {
		uint64_t time_ns        = 0;

		if (g_config.query_enable_histogram) {
			time_ns = cf_getns();
		}

		int ret     = as_query__send_response(qtr);
		if (ret != AS_QUERY_OK) {
			pthread_mutex_unlock(&qtr->buf_mutex);
			return ret;
		}

		QUERY_HIST_INSERT_DATA_POINT(query_net_io_hist, time_ns);

		// if sent successfully mark the used_sz as 0, and reuse
		// the buffer
		bb_r->used_sz = 0;
		qtr->buf_reserved = 0;
		cf_detail(AS_QUERY, "Streamed Out");
	}
	qtr->buf_reserved += msg_sz;

	ret = as_msg_make_response_bufbuilder(r, rd, &qtr->bb_r, false,
			NULL, true, true, true, qtr->binlist);
	if (ret != 0) {
		cf_warning(AS_QUERY, "Weird there is space but still the packing failed "
				"available = %d msg size = %d",
				bb_r->alloc_sz - bb_r->used_sz, msg_sz);
	}
	qtr->num_records++;
	pthread_mutex_unlock(&qtr->buf_mutex);
	return ret;
}

void
as_query__add_result(char *res, as_query_transaction *qtr, bool success)
{
	const as_val * v = (as_val *) as_string_new (res, false);
	as_query__add_val_response((void *) qtr, v, success);
	as_val_destroy(v);
}

int
as_query__add_fin(as_query_transaction *qtr)
{
	uint8_t *b;
	// in case of aborted query, the bb_r is already released
	if (qtr->bb_r == NULL) {
		// Assert that query is aborted if bb_r is found to be null
		return AS_QUERY_ERR;
	}
	cf_buf_builder_reserve(&qtr->bb_r, sizeof(as_msg), &b);

	// set up the header
	uint8_t *buf      = b;
	as_msg *msgp      = (as_msg *) buf;
	msgp->header_sz   = sizeof(as_msg);
	msgp->info1       = 0;
	msgp->info2       = 0;
	msgp->info3       = AS_MSG_INFO3_LAST;
	msgp->unused      = 0;
	msgp->result_code = qtr->result_code;
	msgp->generation  = 0;
	msgp->record_ttl  = 0;
	msgp->n_fields    = 0;
	msgp->n_ops       = 0;
	msgp->transaction_ttl = 0;
	as_msg_swap_header(msgp);
	return 0;
}

/*
 * Sends the actual proto response to the requesting client
 *
 * Returns -
 *		AS_QUERY_OK  - On success
 *		AS_QUERY_ERR - On failure
 */
int
as_query__send_response(as_query_transaction *qtr)
{
	if (!qtr)       return AS_QUERY_ERR;
	if (!qtr->bb_r) return AS_QUERY_ERR;
	uint8_t *buf  = qtr->bb_r->buf;
	int      len  = qtr->bb_r->used_sz;
	as_proto proto;
	proto.version = PROTO_VERSION;
	proto.type    = PROTO_TYPE_AS_MSG;
	proto.sz      = len;
	as_proto_swap(&proto);

	if (qtr->fd_h == 0) return AS_QUERY_ERR;

	int pos = 0, rv;
	while (pos < 8) {
		rv = send(qtr->fd_h->fd, ((uint8_t *) &proto) + pos, 8 - pos , MSG_NOSIGNAL | MSG_MORE);
		if (rv <= 0) {
			if (errno != EAGAIN) {
				cf_debug( AS_QUERY, "query send response error returned %d errno %d fd %d", rv, errno, qtr->fd_h->fd);
				as_query__release_fd(qtr);
				return AS_QUERY_ERR;
			}
		}
		else {
			pos += rv;
		}
	};
	pos = 0;
	while (pos < len) {
		rv = send(qtr->fd_h->fd, buf + pos, len - pos, MSG_NOSIGNAL );
		if (rv <= 0) {
			if (errno != EAGAIN) {
				cf_debug( AS_QUERY, "query send response error returned %d errno %d fd %d", rv, errno, qtr->fd_h->fd);
				as_query__release_fd(qtr);
				return AS_QUERY_ERR;
			}
		}
		else {
			pos += rv;
		}
	}
	qtr->net_io_bytes += ( len + 8 );
	return AS_QUERY_OK;
}

#define as_query__check_timeout(qtr) \
do {                                 \
	if ((qtr)                        \
        && ((qtr)->end_time != 0)    \
		&& (cf_getns() > (qtr)->end_time)) { \
		cf_debug(AS_QUERY, "Query Timedout %lu %lu", cf_getns(), (qtr)->end_time); \
        (qtr)->result_code  =  AS_PROTO_RESULT_FAIL_QUERY_TIMEOUT; \
        (qtr)->abort        =  true;           \
		cf_debug(AS_QUERY, "Query %p Aborted at %s:%d", (qtr), __FILE__, __LINE__); \
    } \
} while(0);


void
as_query__recl_cleanup(cf_ll *recl) {
	if (recl) {
		cf_ll_iterator * iter = NULL;
		iter                  = cf_ll_getIterator(recl, true /*forward*/);
		if (iter) {
			cf_ll_element *ele;
			while ((ele = cf_ll_getNext(iter))) {
				ll_recl_element * node;
				node = (ll_recl_element *) ele;
				dig_arr_t * dt = node->dig_arr;
				node->dig_arr =  NULL;
				if (dt) {
					releaseDigArrToQueue((void *)dt);
				}
			}
		}
		cf_ll_releaseIterator(iter);
	}
}

int as_aggr__process(as_aggr_call *ap_call, cf_ll *ap_recl, void *udata, as_result *ap_res);

int
as_query__process_aggreq(as_query_request *qagg)
{
	int ret = AS_QUERY_ERR;
	as_query_transaction *qtr = qagg->qtr;
	if (!qtr)           goto Cleanup;
	as_query__check_timeout(qtr);
	if (QTR_FAILED(qtr))    goto Cleanup;
	as_result   *res    = as_result_new();
	ret                 = as_aggr__process(&qtr->agg_call, qagg->recl, NULL, res);

	if (ret != 0) {
        char *rs = as_module_err_string(ret);
        if (res->value != NULL) {
            as_string * lua_s   = as_string_fromval(res->value);
            char *      lua_err  = (char *) as_string_tostring(lua_s); 
            if (lua_err != NULL) {
                int l_rs_len = strlen(rs);
                rs = cf_realloc(rs,l_rs_len + strlen(lua_err) + 4);
                sprintf(&rs[l_rs_len]," : %s",lua_err);
            }
        }
        as_query__add_result(rs, qtr, false);
        cf_free(rs);
	}
    as_result_destroy(res);

Cleanup:
	as_query__recl_cleanup(qagg->recl);
	if (qagg->recl) {
		cf_ll_reduce(qagg->recl, true /*forward*/, ll_recl_reduce_fn, NULL);
		if (qagg->recl ) cf_free(qagg->recl);
		qagg->recl = NULL;
	}

	return ret;
}

/*
 * Validate record based on its content and query make sure it indeed should
 * be selected. Secondary index does lazy delete for the entries for the record
 * for which data is on ssd. See sindex design doc for details. Hence it is
 * possible that it returns digest for which record may have changed. Do the
 * validation before returning the row.
 */
bool
as_query_record_matches(as_query_transaction *qtr, as_storage_rd *rd)
{
	// TODO: Add counters and make sure it is not a performance hit
	as_sindex_bin *start = &qtr->srange->start;
	as_sindex_bin *end   = &qtr->srange->end;

	//TODO: Make it more general to support sindex over multiple bins
	as_bin * b = as_bin_get(rd, (uint8_t *)qtr->si->imd->bnames[0],
							strlen(qtr->si->imd->bnames[0]));

	if (!b) {
		cf_debug(AS_QUERY , "as_query_record_validation: "
				"Bin name %s not found ", qtr->si->imd->bnames[0]);
		// Possible bin may not be there anymore classic case of
		// bin delete.
		return false;
	}
	uint8_t type = as_bin_get_particle_type(b);

	// Little paranoid matching all the types
	if ((type != as_sindex_pktype_from_sktype(qtr->si->imd->btype[0]))
			|| (type != start->type)
			|| (type != end->type)) {
		cf_debug(AS_QUERY, "as_query_record_matches: "
				"Type mismatch %d!=%d!=%d!=%d  binname=%s index=%s",
				type, start->type, end->type,
				as_sindex_pktype_from_sktype(qtr->si->imd->btype[0]),
				qtr->si->imd->bnames[0], qtr->si->imd->iname);
		return false;
	}

	switch (type) {
		case AS_PARTICLE_TYPE_INTEGER : {
			int64_t   i = 0;
			uint32_t sz = 8;
			as_particle_tobuf(b, (uint8_t *) &i, &sz);
			i = __be64_to_cpu(i);
			if ((i >= start->u.i64)
					&& (i <= end->u.i64)) {
				return true;
			} else {
				cf_detail(AS_QUERY, "as_query_record_matches: "
						"Integer mismatch %ld not in %ld - %ld", i, start->u.i64, end->u.i64);
				return false;
			}
			break;
		}
		case AS_PARTICLE_TYPE_STRING : {
			uint32_t psz = 32;
			as_particle_tobuf(b, NULL, &psz);
			char buf[psz + 1];
			as_particle_tobuf(b, (uint8_t *) buf, &psz);
			buf[psz]     = '\0';
			cf_digest bin_digest;
			cf_digest_compute( buf, psz, &bin_digest);
			if (memcmp(&bin_digest, &start->digest, AS_DIGEST_KEY_SZ)) {
				cf_detail(AS_QUERY, "as_query_record_validation: "
						" String mismatch |%s|  of size %d", buf, psz);
				return false;
			} else {
				return true;
			}
			break;
		}
		default: {
			goto END;
		}
	}
END:
	return false;
}

bool
as_query_aggr_match_record(query_record * qrecord)
{
	as_query_transaction * qtr = qrecord->caller; 
	qtr->read_success++;
	return as_query_record_matches(qtr, qrecord->urecord->rd); 
}

int
as_query__io(as_query_transaction *qtr, cf_digest *dig)
{
	as_partition_reservation rsv;
	as_namespace * ns = qtr->ns;
	AS_PARTITION_RESERVATION_INIT(rsv);

	// We make sure while making digest list that current node is a qnode
	// Attempt the query reservation here as well. If this node is not a
	// query node anymore then no need to return anything
	if (0 != as_partition_reserve_qnode(ns, as_partition_getid(*dig), &rsv)) {
		return AS_QUERY_OK;
	}
	cf_atomic_int_incr(&g_config.dup_tree_count);
	as_index_ref r_ref;
	r_ref.skip_lock = false;
	int rec_rv      = as_record_get(rsv.tree, dig, &r_ref, ns);

	if (rec_rv == 0) {
		as_index *r = r_ref.r;
		// check to see this isn't an expired record waiting to die
		if (r->void_time &&
				r->void_time < as_record_void_time_get()) {
			as_record_done(&r_ref, ns);
			cf_debug(AS_QUERY,
					"build_response: record expired. treat as not found");
			// Not sending error message to client as per the agreement
			// that server will never send a error result code to the query client.
			goto CLEANUP;
		}
		// make sure it's brought in from storage if necessary
		as_storage_rd rd;
		as_storage_record_open(ns, r, &rd, &r->key);
		qtr->read_success += 1;
		rd.n_bins = as_bin_get_n_bins(r, &rd);

		// Note: This array must stay in scope until the response
		//       for this record has been built, since in the get
		//       data w/ record on device case, it's copied by
		//       reference directly into the record descriptor!
		as_bin stack_bins[rd.ns->storage_data_in_memory ? 0 : rd.n_bins];

		// Figure out which bins you want - for now, all
		rd.bins   = as_bin_get_all(r, &rd, stack_bins);
		rd.n_bins = as_bin_inuse_count(&rd);
		// Call Back
		if(!as_query_record_matches(qtr, &rd)) {
			as_storage_record_close(r, &rd);
			as_record_done(&r_ref, ns);
			as_partition_release(&rsv);
			cf_atomic_int_decr(&g_config.dup_tree_count);
			cf_atomic64_incr(&g_config.query_false_positives);
			return AS_QUERY_OK;
		}

		int ret = qtr->req_cb(qtr, &r_ref, &rd);
		if (ret != 0) {
			as_storage_record_close(r, &rd);
			as_record_done(&r_ref, ns);
			cf_debug(AS_QUERY,
					"Query Response send failed !!! Aborting Query...");
			qtr->abort       = true;
			cf_debug(AS_QUERY, "Query %p Aborted at %s:%d", qtr, __FILE__, __LINE__);
			qtr->result_code = AS_PROTO_RESULT_FAIL_QUERY_CBERROR;
			as_partition_release(&rsv);
			cf_atomic_int_decr(&g_config.dup_tree_count);
			return AS_QUERY_ERR;
		}
		as_storage_record_close(r, &rd);
		as_record_done(&r_ref, ns);
	} else {
		// What do we do about empty records?
		// 1. Should gin up an empty record
		// 2. Current error is returned back to the client.
		cf_detail(AS_QUERY, "as_query__generator: "
				"as_record_get returned %d : key %"PRIx64, rec_rv,
				*(uint64_t *)dig);
		goto CLEANUP;
	}
CLEANUP :
	as_partition_release(&rsv);
	cf_atomic_int_decr(&g_config.dup_tree_count);
	return AS_QUERY_OK;
}

// NB: Caller holds a write hash lock _BE_CAREFUL_ if you intend to take
// lock inside this function
int
as_query_udf_tr_complete( as_transaction *tr, int retcode )
{
	as_query_transaction *qtr = (as_query_transaction *)tr->udata.req_udata;
	if (!qtr) {
		cf_warning(AS_QUERY, "Complete called with invalid job id");
		return -1;
	}
	uint64_t start_time = tr->start_time;
	uint64_t processing_time = (cf_getns() - start_time) / 1000000;
	uint64_t completed  = cf_atomic64_incr(&qtr->uit_completed);
	uint64_t queued     = cf_atomic_int_decr(&qtr->uit_queued);

	// Calculate total processing time of udf transactions completed so far
	cf_atomic64_set(&qtr->uit_total_run_time, cf_atomic64_add(&qtr->uit_total_run_time, processing_time));
	cf_detail(AS_QUERY, "UDF: Internal transaction completed %d, remaining %d, processing_time for this transaction %d, total txn processing time %"PRIu64"",
			completed, queued, processing_time, qtr->uit_total_run_time);
	as_qtr__release(qtr, __FILE__, __LINE__);
	return 0;
}

// Creates a internal transaction for per record UDF execution triggered
// from inside generator. The generator could be scan job generating digest
// or query generating digest.
int
as_internal_query_udf_txn_setup(tr_create_data * d)
{
	as_query_transaction *qtr = (as_query_transaction *)d->udata;

	// TODO: can qtr->priority be 0
	while(qtr->uit_queued >= (AS_QUERY_MAX_UDF_TRANSACTIONS * (qtr->priority / 10 + 1))) {
		cf_debug(AS_QUERY, "UDF: scan transactions [%d] exceeded the maximum "
				"configured limit", qtr->uit_queued);

		usleep(g_config.query_sleep);
	}

	as_transaction tr;
	memset(&tr, 0, sizeof(as_transaction));
	// Pass on the create meta-data structure to create an internal transaction.
	if (as_transaction_create(&tr, d)) {
		return -1;
	}

	tr.udata.req_cb     = as_query_udf_tr_complete;
	tr.udata.req_udata  = d->udata;
	tr.udata.req_type   = UDF_QUERY_REQUEST;
	tr.flag            |= AS_TRANSACTION_FLAG_INTERNAL;

	cf_atomic_int_incr(&qtr->uit_queued);
	cf_detail(AS_QUERY, "UDF: [%d] internal transactions enqueued", qtr->uit_queued);

	as_qtr__reserve(qtr, __FILE__, __LINE__);
	// Reset start time
	tr.start_time = cf_getns();
	if (0 != thr_tsvc_enqueue(&tr)) {
		cf_warning(AS_QUERY, "UDF: Failed to queue transaction for digest %"PRIx64", "
				"number of transactions enqueued [%d] .. dropping current "
				"transaction.. ", tr.keyd, qtr->uit_queued);
		cf_free(tr.msgp);
		tr.msgp = 0;
		as_qtr__release(qtr, __FILE__, __LINE__);
		cf_atomic_int_decr(&qtr->uit_queued);
		return -1;
	}
	return 0;
}

int
as_query__process_udfreq(as_query_request *qudf)
{
	int ret               = AS_QUERY_OK;
	cf_ll_element  * ele  = NULL;
	cf_ll_iterator * iter = NULL;
	as_query_transaction *qtr = qudf->qtr;
	if (!qtr)           return AS_QUERY_ERR;
	as_query__check_timeout(qtr);
	if (QTR_FAILED(qtr))     goto Cleanup;
	cf_detail(AS_QUERY, "Performing UDF");
	iter                  = cf_ll_getIterator(qudf->recl, true /*forward*/);
	if (!iter) {
		qtr->result_code = AS_SINDEX_ERR_NO_MEMORY;
		ret              = AS_QUERY_ERR;
		qtr->err         = true;
		cf_debug(AS_QUERY, "Query %p Aborted at %s:%d", qtr, __FILE__, __LINE__);
		goto Cleanup;
	}

	while((ele = cf_ll_getNext(iter))) {
		ll_recl_element * node;
		node            = (ll_recl_element *) ele;
		dig_arr_t *dt   =  node->dig_arr;
		if (!dt) continue;
		node->dig_arr   =  NULL;
		cf_detail(AS_QUERY, "NUMBER OF DIGESTS = %d", dt->num);
		for (int i = 0; i < dt->num; i++) {
			cf_detail(AS_QUERY, "LOOOPING FOR NUMBER OF DIGESTS %d", i);

			// Fill the structure needed by internal transaction create
			tr_create_data d;
			memset(&d, 0, sizeof(tr_create_data));
			d.digest   = dt->digs[i];
			d.ns       = qtr->ns;
			d.call     = &(qtr->call);
			d.msg_type = AS_MSG_INFO2_WRITE;
			d.fd_h     = qtr->fd_h;
			d.udata    = qtr;

			// Setup the internal udf transaction. This includes creating an internal transaction
			// and enqueuing it with throttling.
			as_internal_query_udf_txn_setup(&d);
			qtr->yield_count++;
			if (qtr->yield_count % qtr->priority == 0) {
				usleep(g_config.query_sleep);
			}
		}
		releaseDigArrToQueue((void *)dt);
	}
Cleanup:
	if(iter) {
		cf_ll_releaseIterator(iter);
		iter = NULL;
	}

	as_query__recl_cleanup(qudf->recl);

	if (qudf->recl) {
		cf_ll_reduce(qudf->recl, true /*forward*/, ll_recl_reduce_fn, NULL);
		if (qudf->recl)  cf_free(qudf->recl);
		qudf->recl = NULL;
	}
	return ret;
}

int
as_query__process_ioreq(as_query_request *qio)
{
	as_query_transaction *qtr = qio->qtr;
	if (!qtr) {
		return AS_QUERY_ERR;
	}

	int ret               = AS_QUERY_ERR;
	QUERY_HIST_INSERT_DATA_POINT(query_batch_io_q_wait_hist, qtr->queued_time_ns);

	cf_ll_element * ele   = NULL;
	cf_ll_iterator * iter = NULL;

	cf_detail(AS_QUERY, "Performing IO");
	uint64_t time_ns      = 0;
	if (g_config.query_enable_histogram || qtr->si->enable_histogram) {
		time_ns = cf_getns();
	}
	iter                  = cf_ll_getIterator(qio->recl, true /*forward*/);
	if (!iter) {
		qtr->err          = true;
		qtr->result_code  = AS_SINDEX_ERR_NO_MEMORY;
		goto Cleanup;
	}

	while((ele = cf_ll_getNext(iter))) {
		ll_recl_element * node;
		node              = (ll_recl_element *) ele;
		dig_arr_t *dt     = node->dig_arr;
		if (!dt) continue;
		node->dig_arr     = NULL;
		for (int i = 0; i < dt->num; i++) {
			cf_digest *dig  = &dt->digs[i];
			ret             = as_query__io(qtr, dig);
			if (ret != AS_QUERY_OK) {
				releaseDigArrToQueue((void *)dt);
				goto Cleanup;
			}

			if (++qtr->yield_count % qtr->priority == 0)
			{
				usleep(g_config.query_sleep);
				as_query__check_timeout(qtr);
				if (QTR_FAILED(qtr)) {
					releaseDigArrToQueue((void *)dt);
					goto Cleanup;
				}
			}
		}
		releaseDigArrToQueue((void *)dt);
	}
Cleanup:

	if(iter) {
		cf_ll_releaseIterator(iter);
		iter = NULL;
	}

	as_query__recl_cleanup(qio->recl);

	if (qio->recl) {
		cf_ll_reduce(qio->recl, true /*forward*/, ll_recl_reduce_fn, NULL);
		if (qio->recl)  cf_free(qio->recl);
		qio->recl = NULL;
	}

	QUERY_HIST_INSERT_DATA_POINT(query_batch_io_hist, time_ns);
	SINDEX_HIST_INSERT_DATA_POINT(qtr->si, query_batch_io, time_ns);

	return 0;
}

int
as_query__process_request(as_query_request *qreqp)
{
	cf_detail(AS_QUERY, "Processing Request %d", qreqp->type);
	int ret = AS_QUERY_OK;
	switch(qreqp->type) {
		case AS_QUERY_REQTYPE_IO:
			ret = as_query__process_ioreq(qreqp);
			break;
		case AS_QUERY_REQTYPE_UDF:
			ret = as_query__process_udfreq(qreqp);
			break;
		case AS_QUERY_REQTYPE_AGG:
			ret = as_query__process_aggreq(qreqp);
			break;
		default:
			cf_warning(AS_QUERY, "Unsupported query type %d.. Dropping it", qreqp->type);
			break;
	}
	return ret;
}

void *
as_query__worker_th(void *q_to_wait_on)
{
	unsigned int         thread_id = cf_atomic32_incr(&g_query_worker_threadcnt);
	cf_detail(AS_QUERY, "Created Query Worker Thread %d", thread_id);
	as_query_request   * qreqp     = NULL;
	int                  ret       = AS_QUERY_OK;

	while (1) {
		// Kill self if thread id is greater than that of number of configured
		// Config change should be flag for quick check
		if (thread_id > g_config.query_worker_threads) {
			pthread_rwlock_rdlock(&g_query_lock);
			if (thread_id > g_config.query_worker_threads) {
				cf_atomic32_decr(&g_query_worker_threadcnt);
				pthread_rwlock_unlock(&g_query_lock);
				cf_detail(AS_QUERY, "Query Worker thread %d exited", thread_id);
				return NULL;
			}
			pthread_rwlock_unlock(&g_query_lock);
		}
		qreqp = as_query__pop_qreq();
		// No need to handle ret here .. if ret were not AS_SINDEX_OK
		// then qtr should been set to abort
		ret = as_query__process_request(qreqp);
		if ((ret != AS_QUERY_OK) && (!qreqp->qtr->abort)) {
			cf_debug(AS_QUERY, "Request processing failed but query is not aborted .... ret %d", ret);
		}

		// qtr was reserved before pushing into the qreq queue
		as_qtr__release(qreqp->qtr, __FILE__, __LINE__);
		qreqp->qtr = NULL;
		as_query__qreq_poolrelease(qreqp);
	}

	return NULL;
}

/*
 * Function as_query__generator_get_nextbatch
 *
 * Notes-
 *		Function generates the next batch of digest list after looking up
 * 		secondary index tree. The function populates qctx->recl with the
 * 		digest list.
 *
 * Returns
 * 		AS_QUERY_OK:  If the batch is full qctx->n_bdigs == qctx->bsize. The caller
 *  		   then processes the batch and reset the qctx->recl and qctx->n_bdigs.
 *
 * 		AS_QUERY_CONTINUE:  If the caller should continue calling this function.
 *
 * 		AS_QUERY_ERR: In case of error
 */
int
as_query__generator_get_nextbatch(as_query_transaction *qtr)
{
	int              ret     = AS_QUERY_OK;
	as_sindex       *si      = qtr->si;
	as_sindex_qctx  *qctx    = &qtr->qctx;
	uint64_t         time_ns = 0;
	if (g_config.query_enable_histogram) {
		time_ns = cf_getns();
	}

	if (qctx->pimd_idx == -1) {
		if (!qtr->srange->isrange) {
			qctx->pimd_idx   = ai_btree_key_hash(si->imd, &qtr->srange->start);
		} else {
			qctx->pimd_idx   = 0;
		}
	}

	as_sindex_range *srange  = qtr->srange;
	if (!qctx->recl) {
		qctx->recl = cf_malloc(sizeof(cf_ll));
		cf_ll_init(qctx->recl, ll_recl_destroy_fn, false /*no lock*/);
		if (!qctx->recl) {
			qtr->result_code = AS_SINDEX_ERR_NO_MEMORY;
			qctx->n_bdigs        = 0;
			ret = AS_QUERY_ERR;
			goto batchout;
		}
		qctx->n_bdigs        = 0;
	} else {
		// Following condition may be true if the
		// query has moved from short query pool to
		// long running query pool
		if (qctx->n_bdigs >= qctx->bsize)
			return ret;
	}

	// Query Aerospike Index
	int      qret            = as_sindex_query(qtr->si, srange, &qtr->qctx);
	cf_detail(AS_QUERY, "start %ld end %ld @ %d pimd found %d", srange->start.u.i64, srange->end.u.i64, qctx->pimd_idx, qctx->n_bdigs);

	qctx->new_ibtr           = false;
	if (qret < 0) { // [AS_SINDEX_OK, AS_SINDEX_CONTINUE] -> OK
		qtr->result_code     = as_sindex_err_to_clienterr(qret,
								__FILE__, __LINE__);
		ret = AS_QUERY_ERR;
		goto batchout;
	}

	as_query__check_timeout(qtr);
	if (QTR_FAILED(qtr)) {
		ret = AS_QUERY_ERR;
		goto batchout;
	}

	if (time_ns) {
		qtr->querying_ai_time_ns += cf_getns() - time_ns;
	}
	if (qctx->n_bdigs < qctx->bsize) {
		qctx->new_ibtr       = true;
		qctx->nbtr_done      = false;
		qctx->pimd_idx++;
		cf_detail(AS_QUERY, "All the Data finished moving to next tree %d", qctx->pimd_idx);
		if (!srange->isrange || (qctx->pimd_idx == si->imd->nprts)) {
			qtr->result_code = AS_PROTO_RESULT_OK;
			ret              = AS_QUERY_DONE;
			goto batchout;
		}
		ret = AS_QUERY_CONTINUE;
		goto batchout;
	}
batchout:
	return ret;
}

void
as_query__setup_qreq(as_query_request *qreqp, as_query_transaction *qtr)
{
	qreqp->qtr               = qtr;
	qreqp->recl              = qtr->qctx.recl;
	qtr->queued_time_ns      = 0;
	qtr->qctx.recl           = NULL;
	qtr->n_digests          += qtr->qctx.n_bdigs;
	qtr->qctx.n_bdigs        = 0;
	if (qtr->job_type == AS_QUERY_AGG) {
		qreqp->type          = AS_QUERY_REQTYPE_AGG;
	} else if (qtr->job_type == AS_QUERY_MRJ) {
		cf_warning(AS_QUERY, "MRJ Query not supported ");
		// what to do
	} else if (qtr->job_type == AS_QUERY_UDF) {
		qreqp->type          = AS_QUERY_REQTYPE_UDF;
	} else {
		qreqp->type          = AS_QUERY_REQTYPE_IO;
	}
}

int
as_query__queue_qreq(as_query_request *qreqp)
{
	as_qtr__reserve(qreqp->qtr, __FILE__, __LINE__);
	if (as_query__push_qreq(qreqp)) {
		as_qtr__release(qreqp->qtr, __FILE__, __LINE__);
		as_query__qreq_poolrelease(qreqp);
		qreqp = NULL;
		return -1;
	}
	return 0;
}

/*
 * Function as_query_generator
 *
 * Does the following
 * 1. Calls the sindex layer for fetching digest list
 * 2. If short running query performs I/O inline and for long running query
 *    queues it up for work threads to execute.
 * 3. If the query is short_running and has hit threshold. Requeue it for
 *    long running generator threads
 *
 * Returns -
 * 		Nothing, sets the qtr status accordingly
 */
void
as_query__generator(as_query_transaction *qtr)
{
	// Setup Query Transaction if it not already setup
	if (!qtr->inited) {
		// Aerospike Index object initialization
		qtr->qctx.bkey = &qtr->bkey;
		init_ai_obj(qtr->qctx.bkey);
		bzero(&qtr->qctx.bdig, sizeof(cf_digest));
		qtr->result_code          = AS_PROTO_RESULT_OK;
		// start with the threshold value
		qtr->qctx.bsize           = g_config.query_threshold;
		qtr->qctx.new_ibtr        = true;
		qtr->qctx.nbtr_done       = false;
		qtr->qctx.pimd_idx        = -1;
		qtr->priority             = g_config.query_priority;
		qtr->bb_r                 = as_query__bb_poolrequest();
		qtr->loop                 = 0;

		// Check if bufbuilder request was successful
		if (!qtr->bb_r) {
			goto Cleanup;
		}
		qtr->inited               = true;
	}

	uint64_t time_ns              = 0;
	if (qtr->si->enable_histogram) {
		time_ns                   = cf_getns();
	}

	while (true) {
		// Step 1: Check for timeout
		as_query__check_timeout(qtr);
		if (QTR_FAILED(qtr)) {
			goto Cleanup;
		}

		// If any query run from more than g_config.query_untracked_time
		// 		we are going to track it
		// else no.
		if (!qtr->track) {
			if ((cf_getns() - qtr->start_time) > g_config.query_untracked_time) {
				qtr->track = true;
				int ret = as_query__put_qtr(qtr);
				if (ret != 0 && ret != AS_QUERY_CONTINUE) {
					qtr->err       = true;
					// track should be disabled otherwise at the 
					// qtr cleanup stage some other qtr with the same 
					// trid can get cleaned up.
					qtr->track     = false;
					goto Cleanup;
				}
			}
		}

		// Step 2: Check to see if this is long running query. This is determined by
		// checking number of records read. Please note that it makes sure the false
		// entries in secondary index does not effect this decision. All short running
		// queries perform I/O in the batch thread context.
		if ((qtr->num_records >= g_config.query_threshold)
				&& qtr->short_running) {
			qtr->short_running       = false;
			// Change batch size to the long running job batch size value
			qtr->qctx.bsize          = g_config.query_bsize;
			if (as_query__queue(qtr) != 0) {
				cf_warning(AS_QUERY, "Long running transaction Queueing Error... continue!!");
				qtr->err = true;
				goto Cleanup;
			} else {
				cf_detail(AS_QUERY, "Query Queued Into Long running thread pool");
				return;
			}
		}

		if (qtr->num_records > g_config.query_rec_count_bound) {
			qtr->abort = true;
			qtr->result_code = AS_PROTO_RESULT_FAIL_QUERY_USERABORT;
			goto Cleanup;
		}

		// Step 3: Get Next Batch
		qtr->loop++;

		int qret    = as_query__generator_get_nextbatch(qtr);

		cf_detail(AS_QUERY, "Loop=%d, Selected=%d, ret=%d", qtr->loop, qtr->qctx.n_bdigs, qret);
		switch(qret) {
			case  AS_QUERY_OK:
			case  AS_QUERY_DONE:
				break;
			case  AS_QUERY_ERR:
				goto Cleanup;
			case  AS_QUERY_CONTINUE:
				continue;
			default:
				cf_warning(AS_QUERY, "Unexpected return type");
				continue;
		}

		SINDEX_HIST_INSERT_DATA_POINT(qtr->si, query_batch_lookup, time_ns);
		if (qtr->si->enable_histogram) {
			time_ns = cf_getns();
		}

		// Step 4: Prepare Query Request either to process inline or for
		//         queueing up for offline processing
		if (qtr->short_running || AS_QUERY_PROCESS_INLINE(qtr)) {
			as_query_request qreq;
			as_query__setup_qreq(&qreq, qtr);
			as_query__process_request(&qreq);
		} else {
			as_query_request *qreqp = as_query__qreq_poolrequest();
			if (!qreqp)  {
				cf_warning(AS_QUERY, "Could not allocate query "
						"request structure .. out of memory .. Aborting !!!");
				goto Cleanup;
			}
			as_query__setup_qreq(qreqp, qtr);
			if (as_query__queue_qreq(qreqp)) {
				goto Cleanup;
			}
		}

		if (qret == AS_QUERY_DONE) {
			// In case all physical tree is done return. if not range loop
			// till less than batch size results are returned
			cf_detail(AS_QUERY, "All the Data finished; All tree finished %d %d", qtr->qctx.n_bdigs, qtr->qctx.bsize);
			qtr->result_code = AS_PROTO_RESULT_OK;
			break;
		}
	}

Cleanup:

	as_query__recl_cleanup(qtr->qctx.recl);
	if (qtr->qctx.recl) {
		cf_ll_reduce(qtr->qctx.recl, true /*forward*/, ll_recl_reduce_fn, NULL);
		if (qtr->qctx.recl) cf_free(qtr->qctx.recl);
		qtr->qctx.recl = NULL;
	}
	as_qtr__release(qtr, __FILE__, __LINE__);
}

/*
 * Function as_query_worker
 *
 * Notes -
 * 		Process one queue's Query requests.
 * 			- Immediately fail if query has timed out
 * 			- Maximum queries that can be served is number of threads
 *
 * 		Releases the qtr, which will call as_query_trasaction_done
 *
 * Synchronization -
 * 		Takes a global query lock while
 */
void*
as_query__th(void* q_to_wait_on)
{
	cf_queue *           query_queue = (cf_queue*)q_to_wait_on;
	unsigned int         thread_id    = cf_atomic32_incr(&g_query_threadcnt);
	cf_detail(AS_QUERY, "Query Thread Created %d");
	as_query_transaction *qtr         = NULL;

	while (1) {
		// Kill self if thread id is greater than that of number of configured
		// thread
		if (thread_id > g_config.query_threads) {
			pthread_rwlock_rdlock(&g_query_lock);
			if (thread_id > g_config.query_threads) {
				cf_atomic32_decr(&g_query_threadcnt);
				pthread_rwlock_unlock(&g_query_lock);
				cf_detail(AS_QUERY, "Query thread %d exited", thread_id);
				return NULL;
			}
			pthread_rwlock_unlock(&g_query_lock);
		}
		if (cf_queue_pop(query_queue, &qtr, CF_QUEUE_FOREVER) != 0) {
			cf_crash(AS_QUERY, "Failed to pop from Query worker queue.");
		}
		if (qtr->short_running) {
			cf_atomic64_incr(&g_config.query_short_running);
		}
		else {
			cf_atomic64_incr(&g_config.query_long_running);
			cf_atomic64_decr(&g_config.query_short_running);
		}
		if (qtr->queued_time_ns != 0) {
			qtr->waiting_time_ns += (cf_getns() - qtr->queued_time_ns);
		}
		as_query__generator(qtr);
	}
	return 0;
}

// QUERY QUERY QUERY QUERY QUERY QUERY QUERY QUERY

/*
 *	Description -
 *		Initialize the queries in a system.
 *		Initialize the all the necessary queues and threads needed for the query execution.
 *
 *		Will create a crash event in case of failure.
 *
 *  caller: main
 */
void
as_query_init()
{
	g_current_queries_count = 0;
	if (cf_atomic32_incr(&g_query_init) != 1) {
		cf_warning(AS_QUERY, "Cannot do multiple initialization");
		return;
	}
	cf_detail(AS_QUERY, "Initialize %d Query Worker threads.", g_config.query_threads);

	// global job hash to keep track of the query job
	int rc = rchash_create(&g_query_job_hash, query_job_trid_hash, NULL, sizeof(uint64_t), 64, RCHASH_CR_MT_MANYLOCK);
	if (rc) {
		cf_crash(AS_QUERY, "Failed to create query job hash");
	}

	// I/O threads
	g_query_qreq_pool = cf_queue_create(sizeof(as_query_request *), true);
	if (!g_query_qreq_pool)
		cf_crash(AS_QUERY, "Failed to create query io queue");

	g_query_response_bb_pool = cf_queue_create(sizeof(void *), true);
	if (!g_query_response_bb_pool)
		cf_crash(AS_QUERY, "Failed to create response buffer query");


	g_query_request_queue = cf_queue_create(sizeof(as_query_request *), true);
	if (!g_query_request_queue)
		cf_crash(AS_QUERY, "Failed to create query io queue");

	// Create the query worker threads detatched so we don't need to join with them.
	if (pthread_attr_init(&g_query_worker_th_attr)) {
		cf_crash(AS_SINDEX, "failed to initialize the query worker thread attributes");
	}
	if (pthread_attr_setdetachstate(&g_query_worker_th_attr, PTHREAD_CREATE_DETACHED)) {
		cf_crash(AS_SINDEX, "failed to set the query worker thread attributes to the detached state");
	}
	int max = g_config.query_worker_threads;
	for (int i = 0; i < max; i++) {
		pthread_create(&g_query_worker_threads[i], &g_query_worker_th_attr,
				as_query__worker_th, (void*)g_query_request_queue);
	}

	g_query_queue = cf_queue_create(sizeof(as_query_transaction *), true);
	if (!g_query_queue)
		cf_crash(AS_QUERY, "Failed to create short query transaction queue");

	g_query_long_queue = cf_queue_create(sizeof(as_query_transaction *), true);
	if (!g_query_long_queue)
		cf_crash(AS_QUERY, "Failed to create long query transaction queue");

	// Create the query threads detatched so we don't need to join with them.
	if (pthread_attr_init(&g_query_th_attr)) {
		cf_crash(AS_SINDEX, "failed to initialize the query thread attributes");
	}
	if (pthread_attr_setdetachstate(&g_query_th_attr, PTHREAD_CREATE_DETACHED)) {
		cf_crash(AS_SINDEX, "failed to set the query thread attributes to the detached state");
	}

	max = g_config.query_threads;
	for (int i = 0; i < max; i += 2) {
		if (pthread_create(&g_query_threads[i], &g_query_th_attr,
					as_query__th, (void*)g_query_queue)
				|| pthread_create(&g_query_threads[i + 1], &g_query_th_attr,
						as_query__th, (void*)g_query_long_queue)) {
			cf_crash(AS_QUERY, "Failed to create query transaction threads");
		}
	}
	char hist_name[64];
	sprintf(hist_name, "query_txn_q_wait_us");
	if (NULL == (query_txn_q_wait_hist = histogram_create(hist_name, HIST_MICROSECONDS))) {
		cf_warning(AS_SINDEX, "couldn't create histogram for the time spent in transaction queue by queries.");
	}

	sprintf(hist_name, "query_prepare_batch_q_wait_us");
	if (NULL == (query_prepare_batch_q_wait_hist = histogram_create(hist_name, HIST_MICROSECONDS))) {
		cf_warning(AS_SINDEX, "couldn't create histogram for time spent waiting for batch creation phase");
	}

	sprintf(hist_name, "query_prepare_batch_us");
	if (NULL == (query_prepare_batch_hist = histogram_create(hist_name, HIST_MICROSECONDS))) {
		cf_warning(AS_SINDEX, "couldn't create histogram for query batch creation phase");
	}

	sprintf(hist_name, "query_batch_io_q_wait_us");
	if (NULL == (query_batch_io_q_wait_hist = histogram_create(hist_name, HIST_MICROSECONDS))) {
		cf_warning(AS_SINDEX, "couldn't create histogram for i/o response time for query batches");
	}

	sprintf(hist_name, "query_batch_io_us");
	if (NULL == (query_batch_io_hist = histogram_create(hist_name, HIST_MICROSECONDS))) {
		cf_warning(AS_SINDEX, "couldn't create histogram for i/o of query batches");
	}

	sprintf(hist_name, "query_net_io_us");
	if (NULL == (query_net_io_hist = histogram_create(hist_name, HIST_MICROSECONDS))) {
		cf_warning(AS_SINDEX, "couldn't create histogram for query net-i/o");
	}

	g_config.query_enable_histogram	= false;
}

/*
 * 	Description -
 * 		It tries to set the query_worker_threads to the given value.
 *
 * 	Synchronization -
 * 		Takes a global query lock to protect the config of
 *
 *	Arguments -
 *		set_size - Value which one want to assign to query_threads.
 *
 * 	Returns -
 * 		AS_QUERY_OK  - On successful resize of query threads.
 * 		AS_QUERY_ERR - Either the set_size exceeds AS_QUERY_MAX_THREADS
 * 					   OR Query threads were not intialized on the first place.
 */
int
as_query_worker_reinit(int set_size, int *actual_size)
{
	if (g_query_init == 0) {
		cf_warning(AS_QUERY, "Query threads not initialtized cannot reinitialize");
		return AS_QUERY_ERR;
	}

	if (set_size > AS_QUERY_MAX_WORKER_THREADS) {
		cf_warning(AS_QUERY, "Cannot increase query threads more than %d",
				AS_QUERY_MAX_WORKER_THREADS);
		//unlock
		return AS_QUERY_ERR;
	}

	pthread_rwlock_wrlock(&g_query_lock);
	// Add threads if count is increased
	int i = cf_atomic32_get(g_query_worker_threadcnt);
	g_config.query_worker_threads = set_size;
	if (set_size > g_query_worker_threadcnt) {
		for (; i < set_size; i++) {
			cf_detail(AS_QUERY, "Creating thread %d", i);
			if (0 != pthread_create(&g_query_worker_threads[i], &g_query_worker_th_attr,
					as_query__worker_th, (void*)g_query_request_queue)) {
				break;
			}
		}
		g_config.query_worker_threads = i;
	}
	*actual_size = g_config.query_worker_threads;

	pthread_rwlock_unlock(&g_query_lock);

	return AS_QUERY_OK;
}

/*
 * 	Description -
 * 		It tries to set the query_threads to the given value.
 *
 * 	Synchronization -
 * 		Takes a global query lock to protect the config of
 *
 *	Arguments -
 *		set_size - Value which one want to assign to query_threads.
 *
 * 	Returns -
 * 		AS_QUERY_OK  - On successful resize of query threads.
 * 		AS_QUERY_ERR - Either the set_size exceeds AS_QUERY_MAX_THREADS
 * 					   OR Query threads were not initialized on the first place.
 */
int
as_query_reinit(int set_size, int *actual_size)
{
	if (g_query_init == 0) {
		cf_warning(AS_QUERY, "Query threads not initialized cannot reinitialize");
		return AS_QUERY_ERR;
	}

	if (set_size > AS_QUERY_MAX_THREADS) {
		cf_warning(AS_QUERY, "Cannot increase query threads more than %d",
				AS_QUERY_MAX_THREADS);
		return AS_QUERY_ERR;
	}

	pthread_rwlock_wrlock(&g_query_lock);
	// Add threads if count is increased
	int i = cf_atomic32_get(g_query_threadcnt);

	// make it multiple of 2
	if (set_size % 2 != 0)
		set_size++;

	g_config.query_threads = set_size;
	if (set_size > g_query_threadcnt) {
		for (; i < set_size; i++) {
			cf_detail(AS_QUERY, "Creating thread %d", i);
			if (0 != pthread_create(&g_query_threads[i], &g_query_th_attr,
					as_query__th, (void*)g_query_queue)) {
				break;
			}
			i++;
			if (0 != pthread_create(&g_query_threads[i], &g_query_th_attr,
					as_query__th, (void*)g_query_long_queue)) {
				break;
			}
		}
		g_config.query_threads = i;
	}
	*actual_size = g_config.query_threads;

	pthread_rwlock_unlock(&g_query_lock);

	return AS_QUERY_OK;
}

int
as_query__queue(as_query_transaction *qtr)
{
	uint64_t limit  = 0;
	uint64_t size   = 0;
	cf_queue    * q;
	cf_atomic64 * queue_full_err;
	if (qtr->short_running) {
		limit          = g_config.query_short_q_max_size;
		size           = cf_queue_sz(g_query_queue);
		q              = g_query_queue;
		queue_full_err = &g_config.query_short_queue_full;
	}
	else {
		limit          = g_config.query_long_q_max_size;
		size           = cf_queue_sz(g_query_long_queue);
		q              = g_query_long_queue;
		queue_full_err = &g_config.query_long_queue_full;
	}

	if( size > limit) {
		cf_atomic64_incr(queue_full_err);
		return -1;
	} else if (cf_queue_push(q, &qtr) != 0) {
		cf_warning(AS_QUERY, "Queuing Error !!");
		return -1;
	} else {
		cf_detail(AS_QUERY, "Logged query ");
	}
	if (g_config.query_enable_histogram) {
		qtr->queued_time_ns = cf_getns();
	}
	return 0;
}

#define as_query__udf_call_init udf_call_init

/**
 * Initialize a new query_agg_call.
 * This populates the query_agg_call from information in the current transaction.
 *
 * @param txn the transaction to build a query_udf_call from
 * @param qtr the query transaction to build a query_udf_call from
 * @return a new udf_call
 */

as_stream_status
query_agg_ostream_write(const as_stream *s, as_val *v)
{
	as_query_transaction *qtr = as_stream_source(s);
	if (!v) {
		return AS_STREAM_OK;
	}
	if (as_query__add_val_response((void *)qtr, v, true)) {
		as_val_destroy(v);
		qtr->abort = true;
		return AS_STREAM_ERR;
	}
	as_val_destroy(v);
	return AS_STREAM_OK;
}

const as_stream_hooks query_agg_istream_hooks = {
	.destroy  = NULL,
	.read     = as_aggr_istream_read,
	.write    = NULL
};

const as_stream_hooks query_agg_ostream_hooks = {
	.destroy  = NULL,
	.read     = NULL,
	.write    = query_agg_ostream_write
};

void as_query__set_error (void * caller )
{
	((as_query_transaction *)caller)->err = true;
}

as_aggr_caller_type as_query__get_type ( )
{
	return AS_AGGR_QUERY;
}

bool
as_query__mem_op(mem_tracker *mt, uint32_t num_bytes, memtracker_op op)
{
	bool ret = true;
	if (!mt || !mt->udata) {
		return false;
	}
	uint64_t val = 0;

	as_query_transaction *qtr = (as_query_transaction *)mt->udata;
	if (qtr) return false;

	if (op == MEM_RESERVE) {
		val = cf_atomic_int_add(&g_config.udf_runtime_gmemory_used, num_bytes);
		if (val > g_config.udf_runtime_max_gmemory) {
			cf_atomic_int_sub(&g_config.udf_runtime_gmemory_used, num_bytes);
			ret = false;
			goto END;
		}

		val = cf_atomic_int_add(&qtr->udf_runtime_memory_used, num_bytes);
		if (val > g_config.udf_runtime_max_memory) {
			cf_atomic_int_sub(&qtr->udf_runtime_memory_used, num_bytes);
			cf_atomic_int_sub(&g_config.udf_runtime_gmemory_used, num_bytes);
			ret = false;
			goto END;
		}
	} else if (op == MEM_RELEASE) {
		cf_atomic_int_sub(&qtr->udf_runtime_memory_used, num_bytes);
	} else if (op == MEM_RESET) {
		cf_atomic_int_sub(&g_config.udf_runtime_gmemory_used,
				qtr->udf_runtime_memory_used);
		qtr->udf_runtime_memory_used = 0;
	} else {
		ret = false;
	}
END:
	return ret;
}

const as_aggr_caller_intf as_query_aggr_caller_qintf = {
	.set_error = as_query__set_error,
	.mem_op = as_query__mem_op,
	.get_type = as_query__get_type
};


/*
 *	Arguments -
 *		tr - transaction coming from the client.
 *
 *	Returns -
 *		AS_QUERY_OK  - on success.
 *		AS_QUERY_ERR - on failure. That means the query was not even started.
 *
 * 	Notes -
 * 		Allocates and reserves the qtr if query_in_transaction_thr
 * 		is set to false or data is in not in memory.
 * 		Has the responsibility to free tr->msgp.
 * 		Either call as_query__transaction_done or Cleanup to free the msgp
 */

int
as_query(as_transaction *tr)
{
	if (tr) {
		QUERY_HIST_INSERT_DATA_POINT(query_txn_q_wait_hist, tr->start_time);
	}
	uint64_t start_time     = cf_getns();
	as_sindex *si           = NULL;
	cf_vector *binlist      = 0;
	as_sindex_range *srange = 0;
	char *setname           = NULL;

	as_msg_field *nsfp = as_msg_field_get(&tr->msgp->msg,
			AS_MSG_FIELD_TYPE_NAMESPACE);
	int rv = AS_QUERY_ERR;
	if (!nsfp) {
		cf_debug(AS_QUERY,
				"Query requests must have namespace, client error");
		tr->result_code = AS_PROTO_RESULT_FAIL_PARAMETER;
		rv = AS_QUERY_ERR;
		goto Cleanup;
	}
	as_namespace *ns = as_namespace_get_bymsgfield(nsfp);
	if (!ns) {
		cf_debug(AS_QUERY, "Query with unavailable namespace");
		tr->result_code = AS_PROTO_RESULT_FAIL_PARAMETER;
		rv = AS_QUERY_ERR;
		goto Cleanup;
	}

	bool has_sindex   = as_sindex_ns_has_sindex(ns);
	if (!has_sindex) {
		tr->result_code = AS_PROTO_RESULT_FAIL_INDEX_NOTFOUND;
		cf_debug(AS_QUERY, "No Secondary Index on namespace %s", ns->name);
		rv = AS_QUERY_ERR;
		goto Cleanup;
	}

	if ((si = as_sindex_from_msg(ns, &tr->msgp->msg)) == NULL) {
		cf_debug(AS_QUERY, "No Index Defined in the Query");
	}

	// TODO: Try srange stack allocation in case execution is in transaction thread;
	// as_sindex_range srange;
	int ret = as_sindex_rangep_from_msg(ns, &tr->msgp->msg, &srange);
	if (AS_QUERY_OK != ret) {
		cf_debug(AS_QUERY, "Could not instantiate index range metadata... "
				"Err, %s", as_sindex_err_str(ret));
		tr->result_code = as_sindex_err_to_clienterr(ret, __FILE__, __LINE__);
		rv = AS_QUERY_ERR;
		goto Cleanup;
	}

	// get optional set
	as_msg_field *sfp = as_msg_field_get(&tr->msgp->msg, AS_MSG_FIELD_TYPE_SET);
	if (sfp) {
		setname = cf_strndup((const char *)sfp->data, as_msg_field_get_value_sz(sfp));
	}

	if (si) {
		// Validate index and range specified
		ret = as_sindex_assert_query(si, srange);
		if (AS_QUERY_OK != ret) {
			cf_warning(AS_QUERY, "Query Parameter Mismatch %d", ret);
			tr->result_code = as_sindex_err_to_clienterr(ret, __FILE__, __LINE__);
			rv = AS_QUERY_ERR;
			goto Cleanup;
		}
	} else {
		// Look up sindex by bin in the query in case not
		// specified in query
		si = as_sindex_from_range(ns, setname, srange);
	}

	// Populate binlist to be Projected by the Query
	binlist = as_sindex_binlist_from_msg(ns, &tr->msgp->msg);

	if (!has_sindex || !si) {
		tr->result_code = AS_PROTO_RESULT_FAIL_INDEX_NOTFOUND;
		rv = AS_QUERY_ERR;
		goto Cleanup;
	}

	// quick check if there is any data with the certain set name
	if (setname && as_namespace_get_set_id(ns, setname) == INVALID_SET_ID) {
		tr->result_code = AS_PROTO_RESULT_OK;
		cf_info(AS_QUERY, "Query on non-existent set %s", setname);
		// Send FIN packet to client to ignore this.
		as_msg_send_fin(tr->proto_fd_h->fd, AS_PROTO_RESULT_OK);
		if (tr->msgp) {
			cf_free(tr->msgp);
			tr->msgp = NULL;
		}
		// special case :
		// add this in the query_fail stat
		cf_atomic64_incr(&g_config.query_fail);
		rv = AS_QUERY_OK;
		goto Cleanup;
	}
	cf_detail(AS_QUERY, "Query on index %s ",
			((as_sindex_metadata *)si->imd)->iname);

	as_query_transaction *qtr = cf_rc_alloc(sizeof(as_query_transaction));
	if (!qtr) {
		rv = AS_QUERY_ERR;
		goto Cleanup;
	}
	memset(qtr, 0, sizeof(as_query_transaction));
	qtr->is_malloc           = true;
	qtr->inited              = false;
	qtr->trid                = tr->trid;
	qtr->fd_h                = tr->proto_fd_h;
	qtr->fd_h->fh_info      |= FH_INFO_DONOT_REAP;
	qtr->ns                  = ns;
	qtr->setname             = setname;
	qtr->si                  = si;
	qtr->srange              = srange;
	qtr->job_type            = AS_QUERY_LOOKUP;
	qtr->binlist             = binlist;
	qtr->start_time          = start_time;
	qtr->end_time            = tr->end_time;
	qtr->msgp                = tr->msgp;
	qtr->short_running       = true;
	qtr->err                 = false;
	qtr->abort               = false;
	qtr->track               = false;
	qtr->queued_time_ns      = 0;   
	qtr->querying_ai_time_ns = 0;
	qtr->waiting_time_ns     = 0;

	if (as_aggr_call_init(&qtr->agg_call, tr, qtr, &as_query_aggr_caller_qintf,
			&query_agg_istream_hooks, &query_agg_ostream_hooks, ns, false) == AS_QUERY_OK) {
		// There is no io call back, record is worked on from inside stream
		// interface
		qtr->req_cb    = NULL;
		qtr->job_type  = AS_QUERY_AGG;
		cf_atomic64_incr(&g_config.n_aggregation);
	} else if (!as_query__udf_call_init(&qtr->call, tr)) {
		qtr->job_type  = AS_QUERY_UDF;
	} else {
		qtr->req_cb    = as_query__add_response;
		qtr->job_type  = AS_QUERY_LOOKUP;
		cf_atomic64_incr(&g_config.n_lookup);
	}
	pthread_mutex_init(&qtr->buf_mutex, NULL);

	if (g_config.query_in_transaction_thr) {
		as_query__generator(qtr);
	} else {
		if (as_query__queue(qtr)) {
			// This error will be accounted by thr_tsvc layer. Thus
			// qtr->err            = true; is not needed here. Else it will be accounted twice.
			// reset fd_h and msgp before calling qtr release, let
			// transaction deal with failure
			qtr->fd_h           = NULL;
			qtr->msgp           = NULL;
			as_qtr__release(qtr, __FILE__, __LINE__);
			tr->result_code     = AS_PROTO_RESULT_FAIL_QUERY_QUEUEFULL;
			return AS_QUERY_ERR;
		}
	}
	// Reset msgp to NULL in tr to avoid double free. And it is successful queuing
	// of query to the query engine. It will reply as needed. Reset proto_fd_h.
	tr->msgp       = NULL;
	tr->proto_fd_h = 0;
	return AS_QUERY_OK;

Cleanup:
	// Pre Query Setup Failure
	if (setname)     cf_free(setname);
	if (si)          AS_SINDEX_RELEASE(si);
	if (srange)      as_sindex_range_free(&srange);
	if (binlist)     cf_vector_destroy(binlist);
	if (tr->msgp)    {
		cf_free(tr->msgp);
		tr->msgp = NULL;
	}
	return rv;
}

/*
 * Function - query_kill
 *
 * Arguments -
 * 		trid - transaction id of the query which one want to kill.
 *
 * Returns -
 *		AS_QUERY_OK  - on success.
 *		AS_QUERY_ERR - on failure.
 *
 * Description -
 *		Iterates through the g_query_transaction array and matches the trid.
 *
 *		Synchronization -
 *			Takes a lock on array to avoid deletion of any qtr from this array.
 */
int
as_query_kill(uint64_t trid)
{
	as_query_transaction *qtr;
	int rv = AS_QUERY_ERR;

	rv =  as_query__get_qtr(trid, &qtr);

	if (rv != AS_QUERY_OK) {
		cf_warning(AS_QUERY, "Cannot kill query with trid [ %"PRIu64" ]",  trid);
	} else {
		qtr->abort = true;
		cf_debug(AS_QUERY, "Query %p Aborted at %s:%d", qtr, __FILE__, __LINE__);
		rv = AS_QUERY_OK;
		as_qtr__release(qtr, __FILE__, __LINE__);
	}

	return rv;
}

int
as_query_set_priority(uint64_t trid, uint32_t priority)
{
	as_query_transaction *qtr;
	int rv = AS_QUERY_ERR;

	rv =  as_query__get_qtr(trid, &qtr);

	if (rv != AS_QUERY_OK) {
		cf_warning(AS_QUERY, "Cannot set priority for query with trid [ %"PRIu64" ]",  trid);
	} else {
		uint32_t old_priority = qtr->priority;
		qtr->priority = priority;
		cf_info(AS_QUERY, "Query priority changed from %d to %d", old_priority, priority);
		rv = AS_QUERY_OK;
		as_qtr__release(qtr, __FILE__, __LINE__);
	}
	return rv;
}

/*
 * Function as_query_stat
 *
 * Returns -
 * 		AS_QUERY_OK
 * TODO
 * 		Add more and synch up the existing stats
 */
int
as_query_stat(char *name, cf_dyn_buf *db)
{
	// Store stats to avoid dynamic changes.
	uint64_t agg          = cf_atomic64_get(g_config.n_aggregation);
	uint64_t agg_success  = cf_atomic64_get(g_config.n_agg_success);
	uint64_t agg_err     = cf_atomic64_get(g_config.n_agg_errs);
	uint64_t agg_records  = cf_atomic64_get(g_config.agg_num_records);
	uint64_t agg_abort    = cf_atomic64_get(g_config.n_agg_abort);
	uint64_t lkup         = cf_atomic64_get(g_config.n_lookup);
	uint64_t lkup_success = cf_atomic64_get(g_config.n_lookup_success);
	uint64_t lkup_err     = cf_atomic64_get(g_config.n_lookup_errs);
	uint64_t lkup_records = cf_atomic64_get(g_config.lookup_num_records);
	uint64_t lkup_abort   = cf_atomic64_get(g_config.n_lookup_abort);

	cf_dyn_buf_append_string(db, "query_reqs=");
	cf_dyn_buf_append_uint64(db, cf_atomic64_get(g_config.query_reqs));

	cf_dyn_buf_append_string(db, ";query_success=");
	cf_dyn_buf_append_uint64(db, agg_success + lkup_success);

	cf_dyn_buf_append_string(db, ";query_fail=");
	cf_dyn_buf_append_uint64(db, cf_atomic64_get(g_config.query_fail) + lkup_err + agg_err);

	cf_dyn_buf_append_string(db, ";query_abort=");
	cf_dyn_buf_append_uint64(db, agg_abort + lkup_abort );


	cf_dyn_buf_append_string(db, ";query_avg_rec_count=");
	cf_dyn_buf_append_uint64(db,  (agg + lkup) ? ( agg_records + lkup_records )
									/ (agg + lkup) : 0);

	cf_dyn_buf_append_string(db, ";query_short_queue_full=");
	cf_dyn_buf_append_uint64(db, cf_atomic64_get(g_config.query_short_queue_full));

	cf_dyn_buf_append_string(db, ";query_long_queue_full=");
	cf_dyn_buf_append_uint64(db, cf_atomic64_get(g_config.query_long_queue_full));

	cf_dyn_buf_append_string(db, ";query_short_running=");
	cf_dyn_buf_append_uint64(db, cf_atomic64_get(g_config.query_short_running));

	cf_dyn_buf_append_string(db, ";query_long_running=");
	cf_dyn_buf_append_uint64(db, cf_atomic64_get(g_config.query_long_running));

	cf_dyn_buf_append_string(db, ";query_tracked=");
	cf_dyn_buf_append_uint64(db, cf_atomic64_get(g_config.query_tracked));

	// Aggregation stats
	cf_dyn_buf_append_string(db, ";query_agg=");
	cf_dyn_buf_append_uint64(db, agg);

	cf_dyn_buf_append_string(db, ";query_agg_success=");
	cf_dyn_buf_append_uint64(db, agg_success);

	cf_dyn_buf_append_string(db, ";query_agg_err=");
	cf_dyn_buf_append_uint64(db, agg_err);


	cf_dyn_buf_append_string(db, ";query_agg_abort=");
	cf_dyn_buf_append_uint64(db, agg_abort);

	cf_dyn_buf_append_string(db, ";query_agg_avg_rec_count=");
	cf_dyn_buf_append_uint64(db, agg ? agg_records / agg : 0);

	// Lookup stats
	cf_dyn_buf_append_string(db, ";query_lookups=");
	cf_dyn_buf_append_uint64(db, lkup);

	cf_dyn_buf_append_string(db, ";query_lookup_success=");
	cf_dyn_buf_append_uint64(db, lkup_success);

	cf_dyn_buf_append_string(db, ";query_lookup_err=");
	cf_dyn_buf_append_uint64(db, lkup_err);

	cf_dyn_buf_append_string(db, ";query_lookup_abort=");
	cf_dyn_buf_append_uint64(db, lkup_abort);

	cf_dyn_buf_append_string(db, ";query_lookup_avg_rec_count=");
	cf_dyn_buf_append_uint64(db, lkup ? lkup_records / lkup : 0);

	return AS_QUERY_OK;
}

int
as_query_list_job_reduce_fn (void *key, uint32_t keylen, void *object, void *udata)
{
	as_query_transaction * qtr = (as_query_transaction*)object;
	cf_dyn_buf * db = (cf_dyn_buf*) udata;

	cf_dyn_buf_append_string(db, "trid=");
	cf_dyn_buf_append_uint64(db, qtr->trid);
	cf_dyn_buf_append_string(db, ":job_type=");
	cf_dyn_buf_append_int(db, qtr->job_type);
	cf_dyn_buf_append_string(db, ":num_records=");
	cf_dyn_buf_append_uint64(db, cf_atomic_int_get(qtr->num_records));
	cf_dyn_buf_append_string(db, ":run_time=");
	cf_dyn_buf_append_uint64(db, (cf_getns() - qtr->start_time) / 1000);
	cf_dyn_buf_append_string(db, ":state=");
	if(qtr->abort) {
		cf_dyn_buf_append_string(db, "ABORTED");
	} else {
		cf_dyn_buf_append_string(db, "RUNNING");
	}
	cf_dyn_buf_append_string(db, ";");
	return AS_QUERY_OK;
}
/*
 * Function as_query_list
 *		Lists thr current running queries
 *
 *	Synchronization -
 *		Takes a lock on global array which stores qtr.
 */

int
as_query_list(char *name, cf_dyn_buf *db)
{
	uint32_t size = rchash_get_size(g_query_job_hash);
	// No elements in the query job hash, return failure
	if(!size) {
		cf_dyn_buf_append_string(db, "No running queries");
	}
	// Else go through all the jobs in the hash and list their statistics
	else {
		rchash_reduce(g_query_job_hash, as_query_list_job_reduce_fn, db);
		cf_dyn_buf_chomp(db);
	}
	return AS_QUERY_OK;
}

extern const as_list_hooks udf_arglist_hooks;
void as_query_fakestream(as_stream *istream, as_list *arglist, as_stream *ostream);
void
as_query_fakestream(as_stream *istream, as_list *arglist, as_stream *ostream)
{
	as_val *v = NULL;
	int count = 0;
	int sum = 0;
	while((v = as_stream_read(istream)) != NULL) {
		as_rec *rec = (as_rec *)v;
		// Pick the bin name
		char binname[3];
		as_val *binval = as_list_get(arglist, 0);
		char *bnno = NULL;
		if (binval) {
			bnno = as_val_tostring(binval);
		}
		sprintf(binname, "bn%s", bnno);
		if (bnno) cf_free(bnno);

		as_val *checkval = as_list_get(arglist, 1);
		char *checkno = NULL;
		if (checkval) {
			checkno = as_val_tostring(checkval);
		}

		v = as_rec_get(rec, binname);
		char *val = as_val_tostring(v);
		if (val && checkno) if (!strcmp(val, checkno)) count++;
		cf_info(AS_QUERY, "Instream : binname = %s, val = %s check val = %s count=%d", binname, val, checkno, count);
		sum += atoi(val);
		cf_free(checkval);
		cf_free(val);
		cf_free(v);
	}
	as_integer i;
	as_integer_init(&i, sum);
	as_stream_write(ostream, (as_val *)&i);
}

// TODO: Find proper query and aggregation engine default values
void
as_query_gconfig_default(as_config *c)
{
	// NB: Do not change query_threads default to odd. as_query_reinit code cannot
	// handle it. Code to handle it is unnecessarily complicated code, hence opted
	// to make the default value even.
	c->query_threads             = 6;
	c->query_worker_threads      = 15;
	c->query_priority            = 10;
	c->query_sleep               = 1;
	c->query_bsize               = QUERY_BATCH_SIZE;
	c->query_job_tracking        = false;
	c->query_in_transaction_thr  = 0;
	c->query_req_max_inflight    = AS_QUERY_MAX_QREQ_INFLIGHT;
	c->query_bufpool_size        = AS_QUERY_MAX_BUFS;
	c->query_short_q_max_size    = AS_QUERY_MAX_SHORT_QUEUE_SZ;
	c->query_long_q_max_size     = AS_QUERY_MAX_LONG_QUEUE_SZ;
	c->query_buf_size            = AS_QUERY_BUF_SIZE;
	c->query_threshold           = 10;	// threshold after which the query is considered long running
										// no reason for choosing 10
	c->query_rec_count_bound     = UINT_MAX; // Unlimited
	c->query_req_in_query_thread = 0;
	c->query_untracked_time      = AS_QUERY_UNTRACKED_TIME;

	// Aggregation
	c->udf_runtime_max_memory    = ULONG_MAX;
	c->udf_runtime_max_gmemory   = ULONG_MAX;
	c->udf_runtime_gmemory_used  = 0;
}

// query module to monitor
void
as_query__fill_jobstat(as_query_transaction *qtr, as_mon_jobstat *stat)
{
	stat->trid          = qtr->trid;
	stat->cpu           = 0;                               // not implemented
	stat->mem           = (float)qtr->buf_reserved;
	stat->run_time      = (cf_getns() - qtr->start_time) / 1000000;
	stat->recs_read     = qtr->read_success;
	stat->net_io_bytes  = qtr->net_io_bytes;
	stat->priority      = qtr->priority;

	strcpy(stat->ns, qtr->ns->name);

	if (qtr->setname) {
		strcpy(stat->set, qtr->setname);
	} else {
		strcpy(stat->set, "NULL");
	}

	strcpy(stat->status, "IN_PROGRESS");
	stat->jdata[0]        = '\0';
	char *specific_data   = stat->jdata;
	sprintf(specific_data, "indexname=%s:", qtr->si->imd->iname);
}


/*
 * Populates the as_mon_jobstat and returns to mult-key lookup monitoring infrastructure.
 * Serves as a callback function
 *
 * Returns -
 * 		NULL - In case of failure.
 * 		as_mon_jobstat - On success.
 */
as_mon_jobstat *
as_query_get_jobstat(uint64_t trid)
{
	as_mon_jobstat *stat;
	as_query_transaction *qtr;
	int rv = AS_QUERY_ERR;
	rv     =  as_query__get_qtr(trid, &qtr);

	if (rv != AS_QUERY_OK) {
		cf_warning(AS_MON, "No query was found with trid [ %"PRIu64" ]", trid);
		stat = NULL;
	}
	else {
		stat = cf_malloc(sizeof(as_mon_jobstat));
		as_query__fill_jobstat(qtr, stat);
		as_qtr__release(qtr, __FILE__, __LINE__);
	}
	return stat;
}

typedef struct as_query_jobstat_s {
	int               index;
	as_mon_jobstat ** jobstat;
	int               max_size;
} as_query_jobstat;


int
as_mon_query_jobstat_reduce_fn (void *key, uint32_t keylen, void *object, void *udata)
{
	as_query_transaction * qtr = (as_query_transaction*)object;
	as_query_jobstat *job_pool = (as_query_jobstat*) udata;

	if ( job_pool->index >= job_pool->max_size) return 0;
	as_mon_jobstat * stat = *(job_pool->jobstat);
	stat                  = stat + job_pool->index;
	as_query__fill_jobstat(qtr, stat);
	(job_pool->index)++;
	return 0;
}

as_mon_jobstat *
as_query_get_jobstat_all(int * size)
{
	*size = rchash_get_size(g_query_job_hash);
	if(*size == 0) return AS_QUERY_OK;

	as_mon_jobstat     * job_stats;
	as_query_jobstat   * job_pool;

	job_stats          = (as_mon_jobstat *) cf_malloc(sizeof(as_mon_jobstat) * (*size));
	job_pool           = (as_query_jobstat *) cf_malloc(sizeof(as_query_jobstat));
	job_pool->jobstat  = &job_stats;
	job_pool->index    = 0;
	job_pool->max_size = *size;
	rchash_reduce(g_query_job_hash, as_mon_query_jobstat_reduce_fn, job_pool);
	*size              = job_pool->index;
	cf_free(job_pool);
	return job_stats;
}

udf_call *
as_query_get_udf_call(void *ptr)
{
	as_query_transaction *qtr = (as_query_transaction *)ptr;
	return &qtr->call;
}
