#include "duckdb/main/resource_scope.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/query_profiler.hpp"

#include <mutex>
#ifndef _WIN32
#include <time.h>
#endif

namespace duckdb {

#ifndef _WIN32

//! The key of this engine copy. A copy without a key opens no scope and counts nothing.
static atomic<pthread_key_t> scope_key {};
static atomic<bool> scope_key_set {false};

static inline bool TryGetSlot(ResourceScope *&result) {
	if (!scope_key_set.load(std::memory_order_relaxed)) {
		return false;
	}
	result = static_cast<ResourceScope *>(pthread_getspecific(scope_key.load(std::memory_order_relaxed)));
	return true;
}

static inline void SetSlot(ResourceScope *scope) {
	pthread_setspecific(scope_key.load(std::memory_order_relaxed), scope);
}

static inline uint64_t ThreadCpuNs() {
	struct timespec now;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
	return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL + static_cast<uint64_t>(now.tv_nsec);
}

ResourceScopeKey ResourceScope::EnsureKey() {
	static std::once_flag once;
	std::call_once(once, []() {
		if (scope_key_set.load()) {
			return;
		}
		pthread_key_t key;
		if (pthread_key_create(&key, nullptr) != 0) {
			return;
		}
		scope_key = key;
		scope_key_set = true;
	});
	ResourceScopeKey result;
	result.key = scope_key.load();
	result.valid = scope_key_set.load();
	return result;
}

void ResourceScope::SetKey(ResourceScopeKey key) {
	if (!key.valid || scope_key_set.load()) {
		return;
	}
	scope_key = key.key;
	scope_key_set = true;
}

//! Adds the CPU the scope spent since it last started to the totals it names
static inline void StopAccumulating(uint64_t now, uint64_t start_ns, uint64_t &cpu_ns, QueryProfiler *connection,
                                    DatabaseInstance *database) {
	if (now <= start_ns) {
		return;
	}
	auto elapsed = now - start_ns;
	cpu_ns += elapsed;
	if (connection) {
		connection->cpu_ns.fetch_add(elapsed, std::memory_order_relaxed);
	}
	database->cpu_ns.fetch_add(elapsed, std::memory_order_relaxed);
}

void ResourceScope::Open(optional_ptr<QueryProfiler> connection_p, DatabaseInstance &database_p,
                         optional_ptr<OperatorInformation> operator_info_p) {
	ResourceScope *innermost;
	if (open || !TryGetSlot(innermost)) {
		return;
	}
	auto now = ThreadCpuNs();
	if (innermost) {
		StopAccumulating(now, innermost->start_ns, innermost->cpu_ns, innermost->connection, innermost->database);
	}
	parent = innermost;
	connection = connection_p.get();
	database = &database_p;
	start_ns = now;
	operator_info = operator_info_p.get();
	cpu_ns = 0;
	open = true;
	SetSlot(this);
}

uint64_t ResourceScope::Close() {
	ResourceScope *innermost;
	if (!open || !TryGetSlot(innermost)) {
		return 0;
	}
	// this scope is only closed by the thread that holds it
	bool found = false;
	for (auto entry = innermost; entry; entry = entry->parent) {
		if (entry == this) {
			found = true;
			break;
		}
	}
	if (!found) {
		return 0;
	}
	auto now = ThreadCpuNs();
	// only the innermost scope accumulates: the scopes beneath it stopped when the scope above them opened
	StopAccumulating(now, innermost->start_ns, innermost->cpu_ns, innermost->connection, innermost->database);
	for (auto entry = innermost; entry != this;) {
		auto next = entry->parent;
		entry->open = false;
		entry->parent = nullptr;
		entry->operator_info = nullptr;
		entry = next;
	}
	if (parent) {
		parent->start_ns = now;
	}
	SetSlot(parent);
	open = false;
	parent = nullptr;
	operator_info = nullptr;
	return cpu_ns;
}

void ResourceScope::AddTransferredBytes(idx_t bytes_read, idx_t bytes_written) {
	ResourceScope *innermost;
	if (!TryGetSlot(innermost) || !innermost) {
		return;
	}
	if (bytes_read > 0) {
		if (innermost->operator_info) {
			innermost->operator_info->bytes_read += bytes_read;
		}
		if (innermost->connection) {
			innermost->connection->bytes_read.fetch_add(bytes_read, std::memory_order_relaxed);
		}
		innermost->database->bytes_read.fetch_add(bytes_read, std::memory_order_relaxed);
	}
	if (bytes_written > 0) {
		if (innermost->operator_info) {
			innermost->operator_info->bytes_written += bytes_written;
		}
		if (innermost->connection) {
			innermost->connection->bytes_written.fetch_add(bytes_written, std::memory_order_relaxed);
		}
		innermost->database->bytes_written.fetch_add(bytes_written, std::memory_order_relaxed);
	}
}

void ResourceScopeGuard::Initialize(optional_ptr<ClientContext> context, DatabaseInstance &db) {
	if (!TryGetSlot(outer)) {
		return;
	}
	active = true;
	if (outer) {
		// a plain scope does not open over another scope: the scope already open keeps accumulating
		return;
	}
	scope.Open(context ? &QueryProfiler::Get(*context) : nullptr, db);
}

ResourceScopeGuard::~ResourceScopeGuard() {
	if (!active) {
		return;
	}
	if (scope.IsOpen()) {
		scope.Close();
		return;
	}
	// close every scope that opened above the scope this guard found, and is still open
	ResourceScope *innermost;
	if (!TryGetSlot(innermost)) {
		return;
	}
	ResourceScope *lowest = nullptr;
	for (auto entry = innermost; entry && entry != outer; entry = entry->parent) {
		lowest = entry;
	}
	if (lowest) {
		lowest->Close();
	}
}

#else

ResourceScopeKey ResourceScope::EnsureKey() {
	ResourceScopeKey result;
	result.key = 0;
	result.valid = false;
	return result;
}

void ResourceScope::SetKey(ResourceScopeKey key) {
}

void ResourceScope::Open(optional_ptr<QueryProfiler> connection_p, DatabaseInstance &database_p,
                         optional_ptr<OperatorInformation> operator_info_p) {
}

uint64_t ResourceScope::Close() {
	return 0;
}

void ResourceScope::AddTransferredBytes(idx_t bytes_read, idx_t bytes_written) {
}

void ResourceScopeGuard::Initialize(optional_ptr<ClientContext> context, DatabaseInstance &db) {
}

ResourceScopeGuard::~ResourceScopeGuard() {
}

#endif

ResourceScopeGuard::ResourceScopeGuard(ClientContext &context) {
	Initialize(&context, DatabaseInstance::GetDatabase(context));
}

ResourceScopeGuard::ResourceScopeGuard(optional_ptr<ClientContext> context, DatabaseInstance &db) {
	Initialize(context, db);
}

} // namespace duckdb
