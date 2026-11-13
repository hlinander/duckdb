#include "catch.hpp"
#include "test_helpers.hpp"

// The test compares the resource totals with CLOCK_PROCESS_CPUTIME_ID and /proc/self/io
#if defined(__linux__)

#include "duckdb.h"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/profiling_node.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/stream_query_result.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sys/stat.h>
#include <thread>
#include <time.h>

using namespace duckdb;
using namespace std;

namespace {

//! The figures of the whole process: the CPU of every thread, and the bytes passed through read and write system calls
struct ProcessFigures {
	uint64_t cpu_ns = 0;
	uint64_t rchar = 0;
	uint64_t wchar = 0;
	std::chrono::steady_clock::time_point wall;
};

ProcessFigures ReadProcessFigures() {
	ProcessFigures result;
	struct timespec now;
	REQUIRE(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) == 0);
	result.cpu_ns = static_cast<uint64_t>(now.tv_sec) * 1000000000ULL + static_cast<uint64_t>(now.tv_nsec);

	std::ifstream io("/proc/self/io");
	REQUIRE(io.good());
	bool found_rchar = false;
	bool found_wchar = false;
	std::string key;
	uint64_t value;
	while (io >> key >> value) {
		if (key == "rchar:") {
			result.rchar = value;
			found_rchar = true;
		} else if (key == "wchar:") {
			result.wchar = value;
			found_wchar = true;
		}
	}
	REQUIRE(found_rchar);
	REQUIRE(found_wchar);
	result.wall = std::chrono::steady_clock::now();
	return result;
}

//! The sums over the profiling tree of one statement
struct TreeSums {
	idx_t nodes = 0;
	idx_t operator_nodes = 0;
	double cpu_seconds = 0;
	//! The largest OPERATOR_CPU_TIME of one node
	double max_node_cpu_seconds = 0;
	uint64_t bytes_read = 0;
	uint64_t bytes_written = 0;
};

void SumTree(ProfilingNode &node, idx_t depth, TreeSums &sums) {
	sums.nodes++;
	if (depth > 0) {
		sums.operator_nodes++;
	}
	auto &metrics = node.GetProfilingInfo().metrics;
	auto cpu = metrics.find(MetricType::OPERATOR_CPU_TIME);
	if (cpu != metrics.end()) {
		auto node_cpu_seconds = cpu->second.GetValue<double>();
		sums.cpu_seconds += node_cpu_seconds;
		if (node_cpu_seconds > sums.max_node_cpu_seconds) {
			sums.max_node_cpu_seconds = node_cpu_seconds;
		}
	}
	auto bytes_read = metrics.find(MetricType::OPERATOR_BYTES_READ);
	if (bytes_read != metrics.end()) {
		sums.bytes_read += bytes_read->second.GetValue<uint64_t>();
	}
	auto bytes_written = metrics.find(MetricType::OPERATOR_BYTES_WRITTEN);
	if (bytes_written != metrics.end()) {
		sums.bytes_written += bytes_written->second.GetValue<uint64_t>();
	}
	for (idx_t i = 0; i < node.GetChildCount(); i++) {
		SumTree(*node.GetChild(i), depth + 1, sums);
	}
}

//! One attempt: the two reads of a bracket, the process figures around the same interval, and the statement's tree
struct Attempt {
	duckdb_resource_totals open;
	duckdb_resource_totals close;
	bool abandoned = false;
	//! Two reads of the totals, taken after the bracket closes and 100 milliseconds apart
	duckdb_resource_totals settled_first;
	duckdb_resource_totals settled_second;
	ProcessFigures before;
	ProcessFigures after;
	TreeSums tree;

	uint64_t ConnectionCpu() const {
		return close.connection.cpu_ns - open.connection.cpu_ns;
	}
	uint64_t DatabaseCpu() const {
		return close.database.cpu_ns - open.database.cpu_ns;
	}
	uint64_t ConnectionBytesRead() const {
		return close.connection.bytes_read - open.connection.bytes_read;
	}
	uint64_t DatabaseBytesRead() const {
		return close.database.bytes_read - open.database.bytes_read;
	}
	uint64_t ConnectionBytesWritten() const {
		return close.connection.bytes_written - open.connection.bytes_written;
	}
	uint64_t DatabaseBytesWritten() const {
		return close.database.bytes_written - open.database.bytes_written;
	}
	uint64_t ProcessCpu() const {
		return after.cpu_ns - before.cpu_ns;
	}
};

Attempt Measure(Connection &con, const string &name, const std::function<void()> &body) {
	auto connection = reinterpret_cast<duckdb_connection>(&con);
	Attempt attempt;
	attempt.before = ReadProcessFigures();
	REQUIRE(duckdb_begin_measured(connection, &attempt.open) == DuckDBSuccess);
	body();
	REQUIRE(duckdb_end_measured(connection, &attempt.close, &attempt.abandoned) == DuckDBSuccess);
	attempt.after = ReadProcessFigures();
	REQUIRE(duckdb_resource_totals_read(connection, &attempt.settled_first) == DuckDBSuccess);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	REQUIRE(duckdb_resource_totals_read(connection, &attempt.settled_second) == DuckDBSuccess);
	QueryProfiler::Get(*con.context).GetRootUnderLock([&](optional_ptr<ProfilingNode> root) {
		if (root) {
			SumTree(*root, 0, attempt.tree);
		}
	});

	if (std::getenv("DUCKDB_RESOURCE_TOTALS_PRINT")) {
		fprintf(stderr,
		        "%-28s process %7.3fs database %7.3fs (%5.1f%%) connection %7.3fs tree %7.3fs nodes %2llu | read db "
		        "%llu conn %llu tree %llu rchar %llu | written db %llu conn %llu tree %llu wchar %llu | memory open "
		        "%llu peak %llu byte_ms %llu\n",
		        name.c_str(), double(attempt.ProcessCpu()) / 1e9, double(attempt.DatabaseCpu()) / 1e9,
		        100.0 * double(attempt.DatabaseCpu()) / double(attempt.ProcessCpu()),
		        double(attempt.ConnectionCpu()) / 1e9, attempt.tree.cpu_seconds,
		        (unsigned long long)attempt.tree.operator_nodes, (unsigned long long)attempt.DatabaseBytesRead(),
		        (unsigned long long)attempt.ConnectionBytesRead(), (unsigned long long)attempt.tree.bytes_read,
		        (unsigned long long)(attempt.after.rchar - attempt.before.rchar),
		        (unsigned long long)attempt.DatabaseBytesWritten(),
		        (unsigned long long)attempt.ConnectionBytesWritten(), (unsigned long long)attempt.tree.bytes_written,
		        (unsigned long long)(attempt.after.wchar - attempt.before.wchar),
		        (unsigned long long)attempt.open.memory_bytes, (unsigned long long)attempt.close.memory_peak_bytes,
		        (unsigned long long)(attempt.close.memory_byte_ms - attempt.open.memory_byte_ms));
	}
	return attempt;
}

//! What the assertions of every bracket read
struct RunState {
	//! The process clock, read once before the database opens
	uint64_t process_cpu_before_database_ns = 0;
	//! The value of task_boundary_cpu_clock the statement runs under
	bool task_boundary_cpu_clock = false;
};

//! The assertions that hold for every statement
void CheckAttempt(const Attempt &attempt, const string &name, RunState &state) {
	// the database's total starts at zero when the database opens
	auto database_cpu_total = attempt.close.database.cpu_ns;
	auto process_cpu_since_database = attempt.after.cpu_ns - state.process_cpu_before_database_ns;

	if (std::getenv("DUCKDB_RESOURCE_TOTALS_PRINT")) {
		fprintf(stderr,
		        "%-28s database total %8.3fs process since open %8.3fs slack %.6fs | largest node %7.3fs | "
		        "connection after close %llu, 100 ms later %llu\n",
		        "", double(database_cpu_total) / 1e9, double(process_cpu_since_database) / 1e9,
		        (double(process_cpu_since_database) - double(database_cpu_total)) / 1e9,
		        attempt.tree.max_node_cpu_seconds, (unsigned long long)attempt.settled_first.connection.cpu_ns,
		        (unsigned long long)attempt.settled_second.connection.cpu_ns);
	}

	INFO(name);
	INFO("task_boundary_cpu_clock: " << state.task_boundary_cpu_clock);
	INFO("database CPU ns at the closing read: " << database_cpu_total);
	INFO("process CPU ns since the read that precedes the database: " << process_cpu_since_database);
	INFO("largest OPERATOR_CPU_TIME of a node, seconds: " << attempt.tree.max_node_cpu_seconds << ", operator nodes "
	                                                      << attempt.tree.operator_nodes);
	INFO("connection CPU ns of the two reads after the bracket: " << attempt.settled_first.connection.cpu_ns << ", "
	                                                              << attempt.settled_second.connection.cpu_ns);
	INFO("process CPU ns: " << attempt.ProcessCpu());
	INFO("database CPU ns: " << attempt.DatabaseCpu());
	INFO("connection CPU ns: " << attempt.ConnectionCpu());
	INFO("tree CPU seconds: " << attempt.tree.cpu_seconds << " over " << attempt.tree.nodes << " nodes");

	// no CPU-second is reported that the process did not spend: a connection's figure takes no carry, and the
	// database's total since it opened counts each carried CPU-second once
	REQUIRE(attempt.ConnectionCpu() <= attempt.ProcessCpu());
	REQUIRE(database_cpu_total <= process_cpu_since_database);
	// the scopes cover the CPU the process spent
	REQUIRE(attempt.DatabaseCpu() * 10 >= attempt.ProcessCpu() * 9);

	// a connection's totals are a part of its database's
	REQUIRE(attempt.ConnectionCpu() <= attempt.DatabaseCpu());
	REQUIRE(attempt.ConnectionBytesRead() <= attempt.DatabaseBytesRead());
	REQUIRE(attempt.ConnectionBytesWritten() <= attempt.DatabaseBytesWritten());

	// the tree divides the connection's totals: the tree holds seconds in a double, one nanosecond for each node
	REQUIRE(attempt.tree.cpu_seconds * 1e9 <= double(attempt.ConnectionCpu()) + double(attempt.tree.nodes));
	if (state.task_boundary_cpu_clock) {
		// an operator scope reads no clock
		REQUIRE(attempt.tree.max_node_cpu_seconds == 0);
	} else if (attempt.tree.operator_nodes > 0) {
		REQUIRE(attempt.tree.cpu_seconds > 0);
	}
	// the connection's work ends inside its bracket
	REQUIRE(attempt.settled_first.connection.cpu_ns == attempt.settled_second.connection.cpu_ns);
	REQUIRE(attempt.tree.bytes_read <= attempt.ConnectionBytesRead());
	REQUIRE(attempt.tree.bytes_written <= attempt.ConnectionBytesWritten());

	// no byte is reported that the process did not pass through a system call
	REQUIRE(attempt.DatabaseBytesRead() <= attempt.after.rchar - attempt.before.rchar);
	REQUIRE(attempt.DatabaseBytesWritten() <= attempt.after.wchar - attempt.before.wchar);

	// the memory marks
	REQUIRE(attempt.close.memory_peak_bytes >= attempt.open.memory_bytes);
	auto wall_ms = std::chrono::duration<double, std::milli>(attempt.after.wall - attempt.before.wall).count();
	auto byte_ms = attempt.close.memory_byte_ms - attempt.open.memory_byte_ms;
	// one byte-millisecond for the truncation of each of the two reads
	REQUIRE(double(byte_ms) <= double(attempt.close.memory_peak_bytes) * wall_ms + 2);
}

uint64_t FileSize(const string &path) {
	struct stat st;
	REQUIRE(stat(path.c_str(), &st) == 0);
	return static_cast<uint64_t>(st.st_size);
}

//! A table function that runs a query on a second connection of the same database
Connection *nested_connection = nullptr;

struct NestedQueryState : public GlobalTableFunctionState {
	bool done = false;
};

duckdb::unique_ptr<FunctionData> NestedQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                                 duckdb::vector<LogicalType> &return_types,
                                                 duckdb::vector<string> &names) {
	return_types.push_back(LogicalType::BIGINT);
	names.push_back("result");
	return nullptr;
}

duckdb::unique_ptr<GlobalTableFunctionState> NestedQueryInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<NestedQueryState>();
}

void NestedQueryFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<NestedQueryState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto result = nested_connection->Query("SELECT count(DISTINCT i % 5000011) FROM range(40000000) t(i)");
	if (result->HasError()) {
		result->ThrowError();
	}
	output.SetValue(0, 0, result->GetValue(0, 0));
	output.SetCardinality(1);
}

} // namespace

TEST_CASE("Test that the resource totals cover what the process spent", "[api]") {
	auto db_path = TestCreatePath("resource_totals.db");
	auto copy_path = TestCreatePath("resource_totals_copy.parquet");
	auto gzip_path = TestCreatePath("resource_totals.csv.gz");
	auto csv_path = TestCreatePath("resource_totals.csv");
	auto union_path = TestCreatePath("resource_totals_union");
	TestDeleteFile(db_path);
	TestDeleteFile(db_path + ".wal");
	TestDeleteFile(copy_path);
	TestDeleteFile(gzip_path);
	TestDeleteFile(csv_path);
	TestDeleteDirectory(union_path);

	RunState state;
	state.process_cpu_before_database_ns = ReadProcessFigures().cpu_ns;

	DuckDB db(db_path);
	Connection con(db);
	// the second connection of the statement that runs a nested query
	Connection nested(db);
	nested_connection = &nested;

	// the tree is built for every statement, and holds the figures the assertions sum
	REQUIRE_NO_FAIL(con.Query("SET threads = 8"));
	REQUIRE_NO_FAIL(con.Query("SET enable_profiling = 'no_output'"));
	REQUIRE_NO_FAIL(con.Query("SET profiling_coverage = 'all'"));
	REQUIRE_NO_FAIL(con.Query("SET custom_profiling_settings = '{\"OPERATOR_CPU_TIME\": \"true\", "
	                          "\"OPERATOR_TIMING\": \"true\", \"OPERATOR_CARDINALITY\": \"true\", "
	                          "\"OPERATOR_BYTES_READ\": \"true\", \"OPERATOR_BYTES_WRITTEN\": \"true\"}'"));

	// the data the statements read; none of this is measured
	REQUIRE_NO_FAIL(con.Query("SET wal_autocheckpoint = '1TB'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE src AS SELECT i, i * 3 AS j, hash(i) AS h FROM range(60000000) t(i)"));
	// the table whose deletes the checkpointing commit writes. It is wide and not long: the commit's work grows with
	// the bytes it rewrites, and the memory the deletes hold afterwards grows with the rows
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE checkpointed AS SELECT i, i * 3 AS j, hash(i) AS h, hash(i + 1) AS h1, "
	                          "hash(i + 2) AS h2, hash(i + 3) AS h3, hash(i + 4) AS h4, hash(i + 5) AS h5 FROM "
	                          "range(100000000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	const string csv_query =
	    "SELECT i % 1000 AS a, 'some repeated text value' AS b, i % 7 AS c FROM range(24000000) t(i)";
	REQUIRE_NO_FAIL(con.Query("COPY (" + csv_query + ") TO '" + gzip_path + "' (FORMAT csv, COMPRESSION gzip)"));
	REQUIRE_NO_FAIL(con.Query("COPY (" + csv_query + ") TO '" + csv_path + "' (FORMAT csv)"));
	REQUIRE_NO_FAIL(con.Query("COPY (SELECT i, j, h, i % 48 AS part FROM src WHERE i < 30000000) TO '" + union_path +
	                          "' (FORMAT parquet, PARTITION_BY (part))"));
	{
		TableFunction nested_query("nested_query", {}, NestedQueryFunction, NestedQueryBind, NestedQueryInit);
		CreateTableFunctionInfo info(nested_query);
		con.BeginTransaction();
		Catalog::GetSystemCatalog(*con.context).CreateTableFunction(*con.context, info);
		con.Commit();
	}

	// the whole list runs under both values of task_boundary_cpu_clock
	for (idx_t pass = 0; pass < 2; pass++) {
		state.task_boundary_cpu_clock = pass == 1;
		REQUIRE_NO_FAIL(con.Query(string("SET GLOBAL task_boundary_cpu_clock = ") + (pass == 1 ? "true" : "false")));
		if (std::getenv("DUCKDB_RESOURCE_TOTALS_PRINT")) {
			fprintf(stderr, "task_boundary_cpu_clock = %s\n", pass == 1 ? "true" : "false");
		}

		auto run = [&](const string &name, const string &query) {
			auto attempt = Measure(con, name, [&]() { REQUIRE_NO_FAIL(con.Query(query)); });
			CheckAttempt(attempt, name, state);
			REQUIRE(!attempt.abandoned);
			return attempt;
		};

		// a finalize that runs inline on the finish task of the pipeline
		run("grouped aggregation", "SELECT count(*), sum(s) FROM (SELECT i % 3000017 AS g, sum(i) AS s FROM "
		                           "range(40000000) t(i) GROUP BY g)");

		// HashAggregateDistinctFinalizeTask, HashAggregateFinalizeTask and UngroupedDistinctAggregateFinalizeTask
		run("grouped distinct aggregation", "SELECT count(*), sum(c) FROM (SELECT i % 1009 AS g, count(DISTINCT i % "
		                                    "2000003) AS c FROM range(20000000) t(i) GROUP BY g)");
		run("ungrouped distinct aggregation",
		    "SELECT count(DISTINCT i % 5000011), sum(DISTINCT i % 3000017) FROM range(30000000) t(i)");

		// the tasks that already hold a ThreadContext, and pipeline tasks
		run("hash join, sort and window",
		    "SELECT sum(rn), max(j) FROM (SELECT j, row_number() OVER (PARTITION BY i % 1000 ORDER BY j) AS rn FROM "
		    "(SELECT a.i, b.j FROM range(5000000) a(i) JOIN range(5000000) b(j) ON a.i = b.j ORDER BY hash(a.i) OFFSET "
		    "10))");

		// ProcessRemainingBatchesTask, and bytes_written
		{
			auto attempt = run("batched copy to a file", "COPY src TO '" + copy_path + "' (FORMAT parquet)");
			REQUIRE(attempt.DatabaseBytesWritten() >= FileSize(copy_path));
		}

		// bytes_read, through a handle that wraps another
		{
			auto compressed_size = FileSize(gzip_path);
			auto decompressed_size = FileSize(csv_path);
			REQUIRE(compressed_size * 4 < decompressed_size);
			auto attempt =
			    run("read_csv of a gzip file", "SELECT count(*), sum(a), sum(c) FROM read_csv('" + gzip_path + "')");
			REQUIRE(attempt.DatabaseBytesRead() >= compressed_size);
			REQUIRE(attempt.DatabaseBytesRead() < decompressed_size);
			REQUIRE(attempt.tree.bytes_read > 0);
		}

		// probe_bytes: the pool refuses the reservation of the join's pointer table, which holds
		// NextPowerOfTwo(2 * 134217729) pointers
		{
			const uint64_t refused_bytes = 1ULL << 32;
			// RESET memory_limit leaves the pool's limit in place and RESET temp_directory leaves it empty: both are
			// set back
			auto settings = con.Query("SELECT current_setting('memory_limit'), current_setting('temp_directory')");
			REQUIRE_NO_FAIL(*settings);
			auto memory_limit = settings->GetValue(0, 0).ToString();
			auto temp_directory = settings->GetValue(1, 0).ToString();
			// the limit is below the refused amount, so no level the pool grants reaches that amount
			REQUIRE_NO_FAIL(con.Query("SET memory_limit = '4000MiB'"));
			REQUIRE_NO_FAIL(con.Query("SET temp_directory = ''"));
			REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers = 'join_order,build_side_probe_side'"));
			string error;
			auto attempt = Measure(con, "refused reservation", [&]() {
				auto result = con.Query("SELECT count(*) FROM range(1000) a(i) JOIN (SELECT j::INTEGER AS j FROM "
				                        "range(134217729) b(j)) b ON a.i::INTEGER = b.j");
				REQUIRE(result->HasError());
				error = result->GetError();
			});
			INFO(error);
			REQUIRE(error.find("failed to allocate data of size 4.0 GiB") != string::npos);
			CheckAttempt(attempt, "refused reservation", state);
			REQUIRE(attempt.close.memory_peak_bytes < refused_bytes);
			REQUIRE_NO_FAIL(con.Query("RESET disabled_optimizers"));
			REQUIRE_NO_FAIL(con.Query("SET temp_directory = '" + temp_directory + "'"));
			REQUIRE_NO_FAIL(con.Query("SET memory_limit = '" + memory_limit + "'"));
		}

		// operator scopes nested on one thread
		run("recursive CTE",
		    "WITH RECURSIVE t(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM t WHERE i < 20000) SELECT count(*) FROM t");

		// TaskExecutor tasks during binding
		run("multi-file read with union_by_name", "SELECT count(*), count(DISTINCT h) FROM read_parquet('" +
		                                              union_path + "/*/*.parquet', union_by_name = true)");

		// BaseCheckpointTask inside EndQueryInternal
		{
			REQUIRE_NO_FAIL(con.Query("BEGIN"));
			// each pass deletes rows the other leaves
			REQUIRE_NO_FAIL(con.Query("DELETE FROM checkpointed WHERE i % 3 = " + to_string(pass)));
			REQUIRE_NO_FAIL(con.Query("SET wal_autocheckpoint = '1KB'"));
			run("commit that checkpoints", "COMMIT");
			// the commit checkpointed: it left no write-ahead log
			struct stat st;
			REQUIRE((stat((db_path + ".wal").c_str(), &st) != 0 || st.st_size == 0));
			REQUIRE_NO_FAIL(con.Query("SET wal_autocheckpoint = '1TB'"));
		}

		// the rollback, and the tree that is kept across it
		{
			auto attempt = Measure(con, "failing statement", [&]() {
				REQUIRE_FAIL(
				    con.Query("SELECT CASE WHEN c > 0 THEN error('deliberate failure') ELSE 0 END FROM (SELECT "
				              "count(DISTINCT i % 5000011) AS c FROM range(40000000) t(i))"));
			});
			CheckAttempt(attempt, "failing statement", state);
			REQUIRE(!attempt.abandoned);
			REQUIRE(attempt.tree.operator_nodes > 0);
		}

		// the drain inside duckdb_end_measured
		{
			duckdb::unique_ptr<QueryResult> result;
			auto attempt = Measure(con, "abandoned stream", [&]() {
				result = con.SendQuery("SELECT g, c FROM (SELECT i % 5000011 AS g, count(*) AS c FROM range(40000000) "
				                       "t(i) GROUP BY g)");
				INFO(result->GetError());
				REQUIRE(!result->HasError());
				REQUIRE(result->type == QueryResultType::STREAM_RESULT);
				auto chunk = result->Fetch();
				REQUIRE(chunk);
				REQUIRE(chunk->size() > 0);
			});
			CheckAttempt(attempt, "abandoned stream", state);
			REQUIRE(attempt.abandoned);
			REQUIRE(attempt.ConnectionCpu() > 0);
			// the connection is usable afterwards
			result.reset();
			REQUIRE_NO_FAIL(con.Query("SELECT 42"));
		}

		// CPU that reaches the database's totals and not the first connection's
		{
			auto nested_handle = reinterpret_cast<duckdb_connection>(&nested);
			duckdb_resource_totals nested_open;
			duckdb_resource_totals nested_close;
			// nothing but the bracket's statement runs on the second connection, so the two reads around the bracket
			// give its CPU over the bracket
			REQUIRE(duckdb_resource_totals_read(nested_handle, &nested_open) == DuckDBSuccess);
			auto attempt = run("query on a second connection", "SELECT * FROM nested_query()");
			REQUIRE(duckdb_resource_totals_read(nested_handle, &nested_close) == DuckDBSuccess);
			auto nested_cpu = nested_close.connection.cpu_ns - nested_open.connection.cpu_ns;
			INFO("second connection CPU ns: " << nested_cpu);
			if (std::getenv("DUCKDB_RESOURCE_TOTALS_PRINT")) {
				fprintf(stderr, "%-28s second connection %7.3fs\n", "", double(nested_cpu) / 1e9);
			}
			REQUIRE(nested_cpu > 0);
			REQUIRE(attempt.ConnectionCpu() + nested_cpu <= attempt.DatabaseCpu());
		}
	}
	nested_connection = nullptr;
}

#endif
