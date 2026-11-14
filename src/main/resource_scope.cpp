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
	// the acquire load pairs with the release store of the flag, which follows the store of the key: a thread that
	// reads the flag as set reads the key that was stored before it
	if (!scope_key_set.load(std::memory_order_acquire)) {
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
		scope_key.store(key, std::memory_order_relaxed);
		scope_key_set.store(true, std::memory_order_release);
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
	scope_key.store(key.key, std::memory_order_relaxed);
	scope_key_set.store(true, std::memory_order_release);
}

//! Adds the CPU the scope spent since it last started to the totals it names
static inline void StopAccumulating(uint64_t now, uint64_t start_ns, uint64_t &cpu_ns, QueryProfiler *connection,
                                    DatabaseInstance *database) {
	if (now <= start_ns) {
		return;
	}
	auto elapsed = now - start_ns;
	cpu_ns += elapsed;
	// the database's total first: a reader loads the connection's first, and never sees it exceed the database's
	database->cpu_ns.fetch_add(elapsed, std::memory_order_relaxed);
	if (connection) {
		connection->cpu_ns.fetch_add(elapsed, std::memory_order_relaxed);
	}
}

//! The scope that accumulates while "innermost" is the innermost scope: the nearest one that read the clock
ResourceScope *ResourceScope::Accumulating(ResourceScope *innermost) {
	auto entry = innermost;
	while (entry && !entry->read_clock) {
		entry = entry->parent;
	}
	return entry;
}

void ResourceScope::Open(optional_ptr<QueryProfiler> connection_p, DatabaseInstance &database_p,
                         ResourceScopeKind kind_p, bool read_clock_p) {
	ResourceScope *innermost;
	if (open || !TryGetSlot(innermost)) {
		return;
	}
	if (read_clock_p) {
		auto now = ThreadCpuNs();
		auto accumulating = Accumulating(innermost);
		if (accumulating) {
			StopAccumulating(now, accumulating->start_ns, accumulating->cpu_ns, accumulating->connection,
			                 accumulating->database);
		}
		start_ns = now;
	}
	parent = innermost;
	connection = connection_p.get();
	database = &database_p;
	operator_info = nullptr;
	cpu_ns = 0;
	kind = kind_p;
	read_clock = read_clock_p;
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
	// one scope accumulates: the nearest to the innermost that read the clock. The thread reads the clock only if
	// that scope closes here; a scope beneath this one keeps accumulating.
	auto accumulating = Accumulating(innermost);
	bool accumulating_closes = false;
	for (auto entry = innermost; entry; entry = entry->parent) {
		if (entry == accumulating) {
			accumulating_closes = true;
		}
		if (entry == this) {
			break;
		}
	}
	uint64_t now = 0;
	if (accumulating_closes) {
		now = ThreadCpuNs();
		StopAccumulating(now, accumulating->start_ns, accumulating->cpu_ns, accumulating->connection,
		                 accumulating->database);
	}
	for (auto entry = innermost; entry != this;) {
		auto next = entry->parent;
		entry->open = false;
		entry->parent = nullptr;
		entry->operator_info = nullptr;
		entry = next;
	}
	if (accumulating_closes) {
		auto next = Accumulating(parent);
		if (next) {
			next->start_ns = now;
		}
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
		innermost->database->bytes_read.fetch_add(bytes_read, std::memory_order_relaxed);
		if (innermost->connection) {
			innermost->connection->bytes_read.fetch_add(bytes_read, std::memory_order_relaxed);
		}
	}
	if (bytes_written > 0) {
		if (innermost->operator_info) {
			innermost->operator_info->bytes_written += bytes_written;
		}
		innermost->database->bytes_written.fetch_add(bytes_written, std::memory_order_relaxed);
		if (innermost->connection) {
			innermost->connection->bytes_written.fetch_add(bytes_written, std::memory_order_relaxed);
		}
	}
}

void ResourceScopeGuard::Initialize(optional_ptr<ClientContext> context, DatabaseInstance &db) {
	if (!TryGetSlot(outer)) {
		return;
	}
	active = true;
	if (outer && outer->kind != ResourceScopeKind::WORKER) {
		// a plain scope does not open over an operator scope or a plain scope: that scope keeps accumulating
		return;
	}
	scope.Open(context ? &QueryProfiler::Get(*context) : nullptr, db, ResourceScopeKind::PLAIN);
}

ResourceScopeReference ResourceScopeReference::Current() {
	ResourceScopeReference result;
	ResourceScope *innermost;
	if (!TryGetSlot(innermost) || !innermost) {
		return result;
	}
	// an owner that is being destroyed hands out no pointer: the reference stays empty
	try {
		weak_ptr<DatabaseInstance> database = innermost->database->shared_from_this();
		weak_ptr<ClientContext> connection;
		if (innermost->connection) {
			connection = innermost->connection->context.shared_from_this();
		}
		result.database = std::move(database);
		result.connection = std::move(connection);
		result.has_connection = innermost->connection != nullptr;
		result.has_database = true;
	} catch (std::exception &) {
		return ResourceScopeReference();
	}
	return result;
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
                         ResourceScopeKind kind_p, bool read_clock_p) {
}

uint64_t ResourceScope::Close() {
	return 0;
}

void ResourceScope::AddTransferredBytes(idx_t bytes_read, idx_t bytes_written) {
}

void ResourceScopeGuard::Initialize(optional_ptr<ClientContext> context, DatabaseInstance &db) {
}

ResourceScopeReference ResourceScopeReference::Current() {
	return ResourceScopeReference();
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

ResourceScopeGuard::ResourceScopeGuard(const ResourceScopeReference &reference) {
	if (!reference.has_database) {
		return;
	}
	database_owner = reference.database.lock();
	if (!database_owner) {
		return;
	}
	if (reference.has_connection) {
		connection_owner = reference.connection.lock();
		if (!connection_owner) {
			return;
		}
	}
	Initialize(connection_owner.get(), *database_owner);
}

} // namespace duckdb
