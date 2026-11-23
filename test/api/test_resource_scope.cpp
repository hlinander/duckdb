#include "catch.hpp"
#include "test_helpers.hpp"

// The scopes read CLOCK_THREAD_CPUTIME_ID and address their slot through a pthread key
#if defined(__linux__)

#include "duckdb.h"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/resource_scope.hpp"

#include <atomic>
#include <thread>
#include <time.h>

using namespace duckdb;
using namespace std;

namespace {

uint64_t ThreadCpuNs() {
	struct timespec now;
	// called on threads the test starts, where the test framework's assertions are not usable
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
	return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL + static_cast<uint64_t>(now.tv_nsec);
}

//! Spends at least 20 milliseconds of the calling thread's CPU
void SpendCpu() {
	auto start = ThreadCpuNs();
	volatile uint64_t sink = 0;
	while (ThreadCpuNs() - start < 20000000ULL) {
		for (idx_t i = 0; i < 100000; i++) {
			sink = sink + i;
		}
	}
}

duckdb_resource_totals ReadTotals(Connection &con) {
	duckdb_resource_totals totals;
	REQUIRE(duckdb_resource_totals_read(reinterpret_cast<duckdb_connection>(&con), &totals) == DuckDBSuccess);
	return totals;
}

//! What a thread spent inside a scope it opened from a reference, and around it
struct ThreadFigures {
	//! The thread's CPU between two reads taken inside the guard's lifetime
	uint64_t inner_ns = 0;
	//! The thread's CPU between two reads taken around the guard's lifetime
	uint64_t outer_ns = 0;
};

const idx_t BYTES_READ = 1000;
const idx_t BYTES_WRITTEN = 2000;

//! Opens a scope from the reference on a new thread, spends CPU in it and transfers bytes in it
ThreadFigures RunScopeOnThread(const ResourceScopeReference &reference) {
	ThreadFigures figures;
	std::thread thread([&]() {
		auto outer_start = ThreadCpuNs();
		{
			ResourceScopeGuard guard(reference);
			auto inner_start = ThreadCpuNs();
			SpendCpu();
			ResourceScope::AddTransferredBytes(BYTES_READ, BYTES_WRITTEN);
			figures.inner_ns = ThreadCpuNs() - inner_start;
		}
		figures.outer_ns = ThreadCpuNs() - outer_start;
	});
	thread.join();
	return figures;
}

} // namespace

TEST_CASE("Test a scope opened from a reference on another thread", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	// a second connection reads the database's totals where the first is gone, and must see none of the first's work
	Connection other(db);

	SECTION("a reference to a connection's scope") {
		ResourceScopeReference reference;
		{
			ResourceScopeGuard guard(*con.context);
			reference = ResourceScopeReference::Current();
		}
		auto before = ReadTotals(con);
		auto other_before = ReadTotals(other);
		auto figures = RunScopeOnThread(reference);
		auto after = ReadTotals(con);
		auto other_after = ReadTotals(other);

		// the scope is the only one that closed between the two reads
		auto connection_cpu = after.connection.cpu_ns - before.connection.cpu_ns;
		auto database_cpu = after.database.cpu_ns - before.database.cpu_ns;
		INFO("connection CPU ns " << connection_cpu << ", database CPU ns " << database_cpu << ", inner "
		                          << figures.inner_ns << ", outer " << figures.outer_ns);
		REQUIRE(figures.inner_ns >= 20000000ULL);
		REQUIRE(connection_cpu >= figures.inner_ns);
		REQUIRE(connection_cpu <= figures.outer_ns);
		REQUIRE(database_cpu == connection_cpu);
		REQUIRE(after.connection.bytes_read - before.connection.bytes_read == BYTES_READ);
		REQUIRE(after.connection.bytes_written - before.connection.bytes_written == BYTES_WRITTEN);
		REQUIRE(after.database.bytes_read - before.database.bytes_read == BYTES_READ);
		REQUIRE(after.database.bytes_written - before.database.bytes_written == BYTES_WRITTEN);

		// no other connection's totals move
		REQUIRE(other_after.connection.cpu_ns == other_before.connection.cpu_ns);
		REQUIRE(other_after.connection.bytes_read == other_before.connection.bytes_read);
		REQUIRE(other_after.connection.bytes_written == other_before.connection.bytes_written);
	}

	SECTION("a reference to a scope that names a database alone") {
		ResourceScopeReference reference;
		{
			ResourceScopeGuard guard(nullptr, *db.instance);
			reference = ResourceScopeReference::Current();
		}
		auto before = ReadTotals(con);
		auto figures = RunScopeOnThread(reference);
		auto after = ReadTotals(con);

		auto database_cpu = after.database.cpu_ns - before.database.cpu_ns;
		INFO("database CPU ns " << database_cpu << ", inner " << figures.inner_ns << ", outer " << figures.outer_ns);
		REQUIRE(database_cpu >= figures.inner_ns);
		REQUIRE(database_cpu <= figures.outer_ns);
		REQUIRE(after.database.bytes_read - before.database.bytes_read == BYTES_READ);
		REQUIRE(after.database.bytes_written - before.database.bytes_written == BYTES_WRITTEN);
		REQUIRE(after.connection.cpu_ns == before.connection.cpu_ns);
		REQUIRE(after.connection.bytes_read == before.connection.bytes_read);
		REQUIRE(after.connection.bytes_written == before.connection.bytes_written);
	}

	SECTION("a reference taken on a thread that holds no scope") {
		auto reference = ResourceScopeReference::Current();
		auto before = ReadTotals(con);
		RunScopeOnThread(reference);
		auto after = ReadTotals(con);
		REQUIRE(after.database.cpu_ns == before.database.cpu_ns);
		REQUIRE(after.database.bytes_read == before.database.bytes_read);
		REQUIRE(after.database.bytes_written == before.database.bytes_written);
	}

	SECTION("a scope opened from a reference whose connection is destroyed adds to the database") {
		auto gone = make_uniq<Connection>(db);
		duckdb::weak_ptr<ClientContext> context = gone->context;
		ResourceScopeReference reference;
		{
			ResourceScopeGuard guard(*gone->context);
			reference = ResourceScopeReference::Current();
		}
		// the reference does not extend the life of the connection
		gone.reset();
		REQUIRE(context.expired());

		auto before = ReadTotals(other);
		auto figures = RunScopeOnThread(reference);
		auto after = ReadTotals(other);
		// the scope opened: it added to the database's block, and to the block of the destroyed connection, which
		// nobody reads
		auto database_cpu = after.database.cpu_ns - before.database.cpu_ns;
		INFO("database CPU ns " << database_cpu << ", inner " << figures.inner_ns << ", outer " << figures.outer_ns);
		REQUIRE(database_cpu >= figures.inner_ns);
		REQUIRE(database_cpu <= figures.outer_ns);
		REQUIRE(after.database.bytes_read - before.database.bytes_read == BYTES_READ);
		REQUIRE(after.database.bytes_written - before.database.bytes_written == BYTES_WRITTEN);
		// no other connection's totals move
		REQUIRE(after.connection.cpu_ns == before.connection.cpu_ns);
		REQUIRE(after.connection.bytes_read == before.connection.bytes_read);
		REQUIRE(after.connection.bytes_written == before.connection.bytes_written);
	}
}

TEST_CASE("Test that a scope opened from a reference holds neither the connection nor the database", "[api]") {
	auto db = make_uniq<DuckDB>(nullptr);
	auto con = make_uniq<Connection>(*db);
	duckdb::weak_ptr<DatabaseInstance> instance = db->instance;
	duckdb::weak_ptr<ClientContext> context = con->context;
	ResourceScopeReference reference;
	{
		ResourceScopeGuard guard(*con->context);
		reference = ResourceScopeReference::Current();
	}

	std::atomic<bool> scope_open {false};
	std::atomic<bool> owners_destroyed {false};
	ThreadFigures figures;
	std::thread thread([&]() {
		auto outer_start = ThreadCpuNs();
		{
			ResourceScopeGuard guard(reference);
			scope_open = true;
			while (!owners_destroyed) {
				std::this_thread::yield();
			}
			// the connection and the database are destroyed: the scope adds to the two blocks, which nobody reads
			auto inner_start = ThreadCpuNs();
			SpendCpu();
			ResourceScope::AddTransferredBytes(BYTES_READ, BYTES_WRITTEN);
			// a reference taken inside the scope names the same blocks
			auto inner_reference = ResourceScopeReference::Current();
			{
				ResourceScopeGuard inner_guard(inner_reference);
				ResourceScope::AddTransferredBytes(BYTES_READ, BYTES_WRITTEN);
			}
			figures.inner_ns = ThreadCpuNs() - inner_start;
		}
		figures.outer_ns = ThreadCpuNs() - outer_start;
	});
	while (!scope_open) {
		std::this_thread::yield();
	}
	// the scope is open on the other thread, and this thread releases the last owner of the connection and then of
	// the database: both are destroyed here, while the scope is open
	con.reset();
	REQUIRE(context.expired());
	db.reset();
	REQUIRE(instance.expired());
	owners_destroyed = true;
	thread.join();
	REQUIRE(figures.inner_ns >= 20000000ULL);
	REQUIRE(figures.outer_ns >= figures.inner_ns);
}

TEST_CASE("Test a scope opened from a reference whose database is destroyed", "[api]") {
	ResourceScopeReference reference;
	duckdb::weak_ptr<DatabaseInstance> instance;
	{
		DuckDB db(nullptr);
		Connection con(db);
		instance = db.instance;
		ResourceScopeGuard guard(*con.context);
		reference = ResourceScopeReference::Current();
	}
	// the reference does not extend the life of the database
	REQUIRE(instance.expired());
	// the scope opens and adds to the two blocks the reference holds, which nobody reads
	auto figures = RunScopeOnThread(reference);
	REQUIRE(figures.inner_ns >= 20000000ULL);
}

TEST_CASE("Test that a read adds to the total of its source", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	const idx_t file_read = 300;
	const idx_t http_read = 50;
	const idx_t other_read = 7;

	auto before = ReadTotals(con);
	{
		ResourceScopeGuard guard(*con.context);
		ResourceScope::AddTransferredBytes(file_read, 11, TransferSource::FILE);
		ResourceScope::AddTransferredBytes(http_read, 13, TransferSource::HTTP);
		ResourceScope::AddTransferredBytes(other_read, 17);
	}
	auto after = ReadTotals(con);

	// every read adds to the total of all reads, and a file or an HTTP read also to the total of its source
	REQUIRE(after.connection.bytes_read - before.connection.bytes_read == file_read + http_read + other_read);
	REQUIRE(after.connection.file_bytes_read - before.connection.file_bytes_read == file_read);
	REQUIRE(after.connection.http_bytes_read - before.connection.http_bytes_read == http_read);
	REQUIRE(after.database.bytes_read - before.database.bytes_read == file_read + http_read + other_read);
	REQUIRE(after.database.file_bytes_read - before.database.file_bytes_read == file_read);
	REQUIRE(after.database.http_bytes_read - before.database.http_bytes_read == http_read);
	// written bytes have one total
	REQUIRE(after.connection.bytes_written - before.connection.bytes_written == 11 + 13 + 17);
	REQUIRE(after.database.bytes_written - before.database.bytes_written == 11 + 13 + 17);
}

#endif
