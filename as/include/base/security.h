/*
 * security.h
 *
 * Copyright (C) 2014 Aerospike, Inc.
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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "base/datamodel.h"
#include "base/proto.h"
#include "base/transaction.h"


//==========================================================
// Typedefs & constants.
//

// Security permissions.
typedef enum {
	PERM_NONE			= 0,

	// Data transactions.
	PERM_READ			= 0x0001,
	PERM_SCAN			= 0x0002,
	PERM_QUERY			= 0x0004,
	PERM_WRITE			= 0x0008,
	PERM_DELETE			= 0x0010,
	PERM_UDF_APPLY		= 0x0020,
	PERM_UDF_SCAN		= 0x0040,
	PERM_UDF_QUERY		= 0x0080,
	// ... 8 unused bits ...

	// Data transactions' system metadata management.
	PERM_INDEX_MANAGE	= 0x00010000,
	PERM_UDF_MANAGE		= 0x00020000,
	// ... 6 unused bits ...

	// Deployment operations management.
	PERM_SET_CONFIG		= 0x01000000,
	PERM_LOGGING_CTRL	= 0x02000000,
	PERM_SERVICE_CTRL	= 0x04000000,

	// Database users and roles management.
	PERM_USER_ADMIN		= 0x100000000000
} as_sec_perm;

// Current security message version.
#define AS_SEC_MSG_SCHEME 0

// Security protocol message container.
typedef struct as_sec_msg_s {
	uint8_t		scheme;		// security scheme/version
	uint8_t		result;		// result code (only for responses, except MORE)
	uint8_t		command;	// security command (only for requests)
	uint8_t		n_fields;	// number of fields in this message

	uint8_t		unused[12];	// reserved bytes round as_sec_msg size to 16 bytes

	uint8_t		fields[];	// the fields (name/value pairs)
} __attribute__ ((__packed__)) as_sec_msg;


//==========================================================
// Public API.
//

void as_security_init();
uint8_t as_security_check(const as_file_handle* fd_h, as_sec_perm perm);
bool as_security_check_data_op(as_transaction *tr, as_msg *m, as_namespace *ns,
		as_sec_perm perm);
void* as_security_filter_create();
void as_security_filter_destroy(void* pv_filter);
void as_security_log(const as_file_handle* fd_h, uint8_t result,
		as_sec_perm perm, const char* action, const char* detail);
void as_security_refresh(as_file_handle* fd_h);
void as_security_transact(as_transaction *tr);
