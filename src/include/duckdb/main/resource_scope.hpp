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
#include "duckdb/common/shared_ptr.hpp"

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

//! The kind of a scope decides whether a plain scope opens above it
enum class ResourceScopeKind : uint8_t {
	//! Opened by OperatorProfiler::StartOperator
	OPERATOR,
	//! Opened by a ResourceScopeGuard, where a thread starts work for a connection or for a database alone
	PLAIN,
	//! Opened by TaskScheduler::ExecuteForever around the loop of a worker thread
	WORKER
};

//! A ResourceScope is an interval on one thread during which that thread's CPU and transferred bytes are added to
//! the totals the scope names. Each thread holds one slot that points to its innermost open scope; at most one scope
//! of a thread accumulates at any instant.
class ResourceScope {
	friend class ResourceScopeGuard;
	friend class ResourceScopeReference;

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

	//! Opens this scope above the calling thread's innermost scope.
	//! The scope names the totals of "connection" (if any) and of "database".
	//! If "read_clock" is set the thread reads its CPU clock, the scope that was accumulating stops, and this scope
	//! starts. Otherwise the thread reads no clock and the scope that was accumulating keeps accumulating.
	DUCKDB_API void Open(optional_ptr<QueryProfiler> connection, DatabaseInstance &database, ResourceScopeKind kind,
	                     bool read_clock = true);
	//! Closes this scope, and every scope still open above it on the calling thread.
	//! Returns the CPU this scope accumulated while it was open, in nanoseconds; zero if it was not open or was
	//! opened without a clock read.
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
	//! The scope that accumulates while "innermost" is the innermost scope: the nearest one that read the clock
	static ResourceScope *Accumulating(ResourceScope *innermost);

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
	ResourceScopeKind kind = ResourceScopeKind::PLAIN;
	//! Whether the scope read the clock when it opened; a scope that did not never accumulates
	bool read_clock = true;
	bool open = false;
};

//! A copyable reference to the totals a scope names. It does not extend the life of the connection or the database:
//! a scope opened from a reference whose owner is gone does not open.
class ResourceScopeReference {
	friend class ResourceScopeGuard;

public:
	ResourceScopeReference() {
	}

public:
	//! The totals the innermost scope of the calling thread names; an empty reference if the thread holds no scope
	DUCKDB_API static ResourceScopeReference Current();

private:
	weak_ptr<DatabaseInstance> database;
	weak_ptr<ClientContext> connection;
	//! Whether the referenced scope names a connection
	bool has_connection = false;
	bool has_database = false;
};

//! Opens a plain scope at a place where a thread starts work for a connection (or for a database alone). The scope
//! opens if the thread holds no scope or its innermost scope is a worker scope; above an operator scope or a plain
//! scope it does not open. On destruction the guard closes every scope that was opened above it and is still open.
class ResourceScopeGuard {
public:
	DUCKDB_API explicit ResourceScopeGuard(ClientContext &context);
	//! Names the connection's totals if a context is given, the database's totals alone otherwise
	DUCKDB_API ResourceScopeGuard(optional_ptr<ClientContext> context, DatabaseInstance &db);
	//! Names the totals "reference" names. The guard keeps their owners alive until it is destroyed; it opens no
	//! scope if an owner is gone.
	DUCKDB_API explicit ResourceScopeGuard(const ResourceScopeReference &reference);
	DUCKDB_API ~ResourceScopeGuard();
	ResourceScopeGuard(const ResourceScopeGuard &) = delete;
	ResourceScopeGuard &operator=(const ResourceScopeGuard &) = delete;

private:
	void Initialize(optional_ptr<ClientContext> context, DatabaseInstance &db);

private:
	//! The owners of the totals, held by a guard that was constructed from a reference
	shared_ptr<DatabaseInstance> database_owner;
	shared_ptr<ClientContext> connection_owner;
	ResourceScope scope;
	//! The innermost scope of the thread when the guard was constructed
	ResourceScope *outer = nullptr;
	//! Whether the slot was read at construction
	bool active = false;
};

} // namespace duckdb
