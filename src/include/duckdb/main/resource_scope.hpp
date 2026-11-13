//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/resource_scope.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"

#ifndef _WIN32
#include <pthread.h>
#endif

namespace duckdb {

class ClientContext;
class DatabaseInstance;
class QueryProfiler;
struct OperatorInformation;

//! The thread-specific-data key under which every engine copy of the process addresses the per-thread scope slot
struct ResourceScopeKey {
#ifndef _WIN32
	pthread_key_t key;
#else
	uint32_t key;
#endif
	bool valid;
};

//! A ResourceScope is an interval on one thread during which that thread's CPU and transferred bytes are added to
//! the totals the scope names. Each thread holds one slot that points to its innermost open scope; at most one scope
//! of a thread accumulates at any instant.
class ResourceScope {
	friend class ResourceScopeGuard;

public:
	ResourceScope() {
	}
	ResourceScope(const ResourceScope &) = delete;
	ResourceScope &operator=(const ResourceScope &) = delete;

public:
	//! Returns the key of this engine copy, creating it first if this copy has none
	DUCKDB_API static ResourceScopeKey EnsureKey();
	//! Gives this engine copy the key of the copy that constructed the database (DatabaseInstance::scope_key).
	//! A copy that is not given the key opens no scope and counts nothing.
	DUCKDB_API static void SetKey(ResourceScopeKey key);
	//! Adds the bytes of one transfer to the totals of the calling thread's innermost scope (if any)
	DUCKDB_API static void AddTransferredBytes(idx_t bytes_read, idx_t bytes_written);

	//! Opens this scope above the calling thread's innermost scope, which stops accumulating.
	//! The scope names the totals of "connection" (if any) and of "database". An operator scope passes the pending
	//! information of its operator (if any), which receives the bytes transferred inside the scope.
	DUCKDB_API void Open(optional_ptr<QueryProfiler> connection, DatabaseInstance &database,
	                     optional_ptr<OperatorInformation> operator_info = nullptr);
	//! Closes this scope, and every scope still open above it on the calling thread.
	//! Returns the CPU this scope accumulated while it was open, in nanoseconds; zero if it was not open.
	DUCKDB_API uint64_t Close();

	bool IsOpen() const {
		return open;
	}
	//! Attaches the pending information of the operator this (operator) scope is open for
	void SetOperatorInfo(OperatorInformation &info) {
		if (open) {
			operator_info = &info;
		}
	}
	//! Detaches the pending operator information, which is about to be destroyed
	void ClearOperatorInfo() {
		operator_info = nullptr;
	}

private:
	//! The scope beneath this one on the same thread
	ResourceScope *parent = nullptr;
	//! The totals this scope names; "connection" is null for a scope that names a database alone
	QueryProfiler *connection = nullptr;
	DatabaseInstance *database = nullptr;
	//! The pending information of the operator, for an operator scope whose profiler collects it
	OperatorInformation *operator_info = nullptr;
	//! The thread CPU clock reading at which this scope last started accumulating
	uint64_t start_ns = 0;
	uint64_t cpu_ns = 0;
	bool open = false;
};

//! Opens a scope at a place where a thread starts work for a connection (or for a database alone), unless a scope is
//! already open on the thread. On destruction it closes every scope that was opened above it and is still open.
class ResourceScopeGuard {
public:
	DUCKDB_API explicit ResourceScopeGuard(ClientContext &context);
	//! Names the connection's totals if a context is given, the database's totals alone otherwise
	DUCKDB_API ResourceScopeGuard(optional_ptr<ClientContext> context, DatabaseInstance &db);
	DUCKDB_API ~ResourceScopeGuard();
	ResourceScopeGuard(const ResourceScopeGuard &) = delete;
	ResourceScopeGuard &operator=(const ResourceScopeGuard &) = delete;

private:
	void Initialize(optional_ptr<ClientContext> context, DatabaseInstance &db);

private:
	ResourceScope scope;
	//! The innermost scope of the thread when the guard was constructed
	ResourceScope *outer = nullptr;
	//! Whether the slot was read at construction
	bool active = false;
};

} // namespace duckdb
