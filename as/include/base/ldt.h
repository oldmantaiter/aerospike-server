/*
 * ldt.h
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
 * Large (Linked) Data Type module
 *
 */

#pragma once

#include "base/feature.h" // turn new AS Features on/off

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "aerospike/as_bytes.h"
#include "citrusleaf/cf_clock.h"
#include "citrusleaf/cf_digest.h"

#include "fault.h"

#include "base/datamodel.h"
#include "base/index.h"
#include "base/ldt_record.h"
#include "base/write_request.h"
#include "storage/storage.h"

#define LDT_SUB_GC_MAX_RATE         100000 // Do not allow more than 100,000 subrecord GC per second

// Use these flags to designate various LDT bin types -- but they are all
// HIDDEN BINS.
#define LDT_FLAG_LDT_BIN 1
#define LDT_FLAG_HIDDEN_BIN 2
#define LDT_FLAG_CONTROL_BIN 4

extern cf_clock cf_clock_getabsoluteus();

#define ERR_TOP_REC_NOT_FOUND   2
#define ERR_NOT_FOUND           125
#define ERR_INTERNAL             1400
#define ERR_UNIQUE_KEY           1402
#define ERR_INSERT               1403
#define ERR_SEARCH               1404
#define ERR_DELETE               1405
#define ERR_VERSION              1406

#define ERR_CAPACITY_EXCEEDED    1408
#define ERR_INPUT_PARM           1409

#define ERR_TYPE_MISMATCH        1410
#define ERR_NULL_BIN_NAME        1411
#define ERR_BIN_NAME_NOT_STRING  1412
#define ERR_BIN_NAME_TOO_LONG    1413
#define ERR_TOO_MANY_OPEN_SUBRECS 1414
#define ERR_SUB_REC_NOT_FOUND    1416
#define ERR_BIN_DOES_NOT_EXIST   1417
#define ERR_BIN_ALREADY_EXISTS   1418
#define ERR_BIN_DAMAGED          1419

#define ERR_SUBREC_POOL_DAMAGED  1420
#define ERR_SUBREC_DAMAGED       1421
#define ERR_SUBREC_OPEN          1422
#define ERR_SUBREC_UPDATE        1423
#define ERR_SUBREC_CREATE        1424
#define ERR_SUBREC_DELETE        1425
#define ERR_SUBREC_CLOSE         1426
#define ERR_TOPREC_UPDATE        1427
#define ERR_TOPREC_CREATE        1428

#define ERR_FILTER_BAD           1430
#define ERR_FILTER_NOT_FOUND     1431
#define ERR_KEY_FUN_BAD          1432
#define ERR_KEY_FUN_NOT_FOUND    1433
#define ERR_TRANS_FUN_BAD        1434
#define ERR_TRANS_FUN_NOT_FOUND  1435
#define ERR_UNTRANS_FUN_BAD      1436
#define ERR_UNTRANS_FUN_NOT_FOUND 1437
#define ERR_USER_MODULE_BAD       1438
#define ERR_USER_MODULE_NOT_FOUND 1439

typedef struct ldt_sub_gc_info_s {
	as_namespace	*ns;
	uint32_t		num_gc;
} ldt_sub_gc_info;


#define LDT_READ_OP		0
#define LDT_WRITE_OP	1

extern int		as_ldt_package_index(const char *package_name);
extern int		as_ldt_op_type(int package_index, const char *op_name);

extern int      as_ldt_flatten_component   (as_partition_reservation *rsv, as_storage_rd *rd, as_index_ref *r_ref, as_record_merge_component *c, bool *);

extern bool     as_ldt_set_flag            (uint16_t flag);
extern bool     as_ldt_flag_has_parent     (uint16_t flag);
extern bool     as_ldt_flag_has_sub        (uint16_t flag);
extern bool     as_ldt_flag_has_subrec     (uint16_t flag);
extern bool     as_ldt_flag_has_esr        (uint16_t flag);

extern void     as_ldt_sub_gc_fn           (as_index_ref *r_ref, void *udata);
extern int      as_ldt_shipop              (write_request *wr, cf_node dest_node);

extern int      as_ldt_parent_storage_set_version (as_storage_rd *rd, uint64_t, uint8_t *, char *fname, int lineno);
extern int      as_ldt_parent_storage_get_version (as_storage_rd *rd, uint64_t *, bool, char *fname, int lineno);
extern int      as_ldt_subrec_storage_get_digests (as_storage_rd *rd, cf_digest *edigest, cf_digest *pdigest);
extern void     as_ldt_subrec_storage_validate    (as_storage_rd *rd, char *op);

extern void     as_ldt_digest_randomizer           (cf_digest *dig);
extern bool     as_ldt_merge_component_is_candidate(as_partition_reservation *rsv, as_record_merge_component *c);

extern void     as_ldt_record_set_rectype_bits    (as_record *r, const as_rec_props *props);
extern int      as_ldt_record_pickle              (ldt_record *lrecord, uint8_t **pickled_buf, size_t *pickled_sz, uint32_t *pickled_void_time);

// Version related functions
extern uint64_t as_ldt_generate_version();
extern void     as_ldt_subdigest_setversion   (cf_digest *dig, uint64_t version);
extern uint64_t as_ldt_subdigest_getversion   (cf_digest *dig);
extern void     as_ldt_subdigest_resetversion (cf_digest *dig);

/*
 * Returns true if passed in record an LDT Parent (top record).
 * NOTE: Record is expected to be properly initialized and locked and
 * partition reserved
 */
static inline bool
as_ldt_record_is_parent(as_record *r)
{
	return ((!r)
			? false
			: as_index_is_flag_set( r, AS_INDEX_FLAG_SPECIAL_BINS ));
}

static inline bool
as_ldt_record_is_sub(as_record *r)
{
	return ((!r)
			? false
			: (as_index_is_flag_set( r, AS_INDEX_FLAG_CHILD_ESR ) ||
			   as_index_is_flag_set( r, AS_INDEX_FLAG_CHILD_REC )));
}

// Return true if this record is ANY type of LDT record (parent, child, esr)
static inline bool
as_ldt_record_is_ldt(as_index *r)
{
	return ((!r)
			? false
			: (as_index_is_flag_set( r, AS_INDEX_FLAG_CHILD_ESR ) ||
			   as_index_is_flag_set( r, AS_INDEX_FLAG_CHILD_REC ) ||
			   as_index_is_flag_set( r, AS_INDEX_FLAG_SPECIAL_BINS )));
}

// Return true if this record is an LDT subrecord; Child or ESR subrec.
static inline bool
as_ldt_record_is_subrec(as_record *r)
{
	return ((!r)
			? false
			: (as_index_is_flag_set( r, AS_INDEX_FLAG_CHILD_REC )));
}

// Return true if this LDT subrecord is of type ESR (Existence Sub Record).
static inline bool
as_ldt_record_is_esr(as_record *r)
{
	return ((!r)
			? false
			: as_index_is_flag_set( r, AS_INDEX_FLAG_CHILD_ESR ));
}

/*
 * Create 16 bit property field for storing on disk
 * Notes:
 * (1) All this property is some way or the other also in index so that is
 * pattern used.
 * (2) Must not add anything which is not in index as record
 * property at this point!
 */
static inline uint16_t
as_ldt_record_get_rectype_bits(as_record *r)
{
	return (as_index_get_flags(r) & 
		(AS_INDEX_FLAG_CHILD_ESR | AS_INDEX_FLAG_CHILD_REC | AS_INDEX_FLAG_SPECIAL_BINS));
}

static inline int
as_ldt_bytes_todigest(as_bytes *bytes, cf_digest *keyd)
{
	if (as_bytes_size(bytes) < CF_DIGEST_KEY_SZ) {
		cf_warning(AS_LDT, "ldt_string_todigest Invalid digest size %d", as_bytes_size(bytes));
		return -1;
	}

	int i = 0;
	for(;;) {
		uint8_t val;
		as_bytes_get_byte(bytes, i, &val);
		//cf_detail(AS_UDF, "dig %d", val);
		keyd->digest[i++] = val;
		if (i == CF_DIGEST_KEY_SZ) break;
	}
	return 0;
}

// Note: If the string form bdig ever changes this function will break.
// TODO: change the whole thing into as_bytes.
static inline int
as_ldt_string_todigest(const char *bdig, cf_digest *keyd)
{
	if (strlen(bdig) < ((CF_DIGEST_KEY_SZ * 3) - 1)) {
		cf_warning(AS_LDT, "ldt_string_todigest Invalid digest %s:%d", bdig, strlen(bdig));
		return -1;
	}
	// bdig looks like
	// "06 89 8C 53 4A 51 AC F7 70 29 8D 71 FE FF 00 00 00 00 00 00"
	//
	// start at 1 and at every byte shift three positions
	int j = 0;
	for (int i = 0; ; i++) {
		char val[3];
		val[0] = bdig[i++];
		val[1] = bdig[i++];
		val[2] = '\0';
		int intval = strtol(val, NULL, 16);
		cf_detail(AS_UDF, "dig %s->%d", val, intval);
		keyd->digest[j++] = intval;
		if (j == CF_DIGEST_KEY_SZ) break;
	}
	cf_detail(AS_LDT, "Convert %s to %"PRIx64"", bdig, keyd);
	return 0;
}
