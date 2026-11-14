#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"

using duckdb::ClientContext;
using duckdb::Connection;
using duckdb::DatabaseInstance;
using duckdb::DuckDB;
using duckdb::EnumUtil;
using duckdb::MetricType;
using duckdb::optional_ptr;
using duckdb::ProfilingNode;
using duckdb::QueryProfiler;

duckdb_profiling_info duckdb_get_profiling_info(duckdb_connection connection) {
	if (!connection) {
		return nullptr;
	}
	Connection *conn = reinterpret_cast<Connection *>(connection);
	optional_ptr<ProfilingNode> profiling_node;
	try {
		profiling_node = conn->GetProfilingTree();
	} catch (std::exception &ex) {
		return nullptr;
	}

	if (!profiling_node) {
		return nullptr;
	}
	return reinterpret_cast<duckdb_profiling_info>(profiling_node.get());
}

duckdb_value duckdb_profiling_info_get_value(duckdb_profiling_info info, const char *key) {
	if (!info) {
		return nullptr;
	}
	auto &node = *reinterpret_cast<duckdb::ProfilingNode *>(info);
	auto &profiling_info = node.GetProfilingInfo();
	auto key_enum = EnumUtil::FromString<MetricType>(duckdb::StringUtil::Upper(key));
	if (!profiling_info.Enabled(profiling_info.settings, key_enum)) {
		return nullptr;
	}

	auto str = profiling_info.GetMetricAsString(key_enum);
	return duckdb_create_varchar_length(str.c_str(), strlen(str.c_str()));
}

duckdb_value duckdb_profiling_info_get_metrics(duckdb_profiling_info info) {
	if (!info) {
		return nullptr;
	}

	auto &node = *reinterpret_cast<duckdb::ProfilingNode *>(info);
	auto &profiling_info = node.GetProfilingInfo();

	duckdb::InsertionOrderPreservingMap<duckdb::string> metrics_map;
	for (const auto &metric : profiling_info.metrics) {
		auto key = EnumUtil::ToString(metric.first);
		if (!profiling_info.Enabled(profiling_info.settings, metric.first)) {
			continue;
		}

		if (key == EnumUtil::ToString(MetricType::OPERATOR_TYPE)) {
			auto type = duckdb::PhysicalOperatorType(metric.second.GetValue<uint8_t>());
			metrics_map[key] = EnumUtil::ToString(type);
		} else {
			metrics_map[key] = metric.second.ToString();
		}
	}

	auto map = duckdb::Value::MAP(metrics_map);
	return reinterpret_cast<duckdb_value>(new duckdb::Value(map));
}

idx_t duckdb_profiling_info_get_child_count(duckdb_profiling_info info) {
	if (!info) {
		return 0;
	}
	auto &node = *reinterpret_cast<duckdb::ProfilingNode *>(info);
	return node.GetChildCount();
}

duckdb_profiling_info duckdb_profiling_info_get_child(duckdb_profiling_info info, idx_t index) {
	if (!info) {
		return nullptr;
	}
	auto &node = *reinterpret_cast<duckdb::ProfilingNode *>(info);
	if (index >= node.GetChildCount()) {
		return nullptr;
	}

	ProfilingNode *profiling_info_ptr = node.GetChild(index).get();
	return reinterpret_cast<duckdb_profiling_info>(profiling_info_ptr);
}

//! Writes every field of the totals; zeroes them first so a failure leaves no field unwritten
static void ReadResourceTotals(Connection &conn, duckdb_resource_totals &out) {
	auto &context = *conn.context;
	auto &profiler = QueryProfiler::Get(context);
	auto &db = DatabaseInstance::GetDatabase(context);

	auto &connection_totals = *profiler.resource_totals;
	auto &database_totals = *db.resource_totals;

	out.connection.cpu_ns = connection_totals.cpu_ns.load(std::memory_order_relaxed);
	out.connection.bytes_read = connection_totals.bytes_read.load(std::memory_order_relaxed);
	out.connection.bytes_written = connection_totals.bytes_written.load(std::memory_order_relaxed);
	out.database.cpu_ns = database_totals.cpu_ns.load(std::memory_order_relaxed);
	out.database.bytes_read = database_totals.bytes_read.load(std::memory_order_relaxed);
	out.database.bytes_written = database_totals.bytes_written.load(std::memory_order_relaxed);

	auto marks = db.GetBufferPool().ReadMemoryMarks();
	out.memory_bytes = marks.floor_bytes;
	out.memory_peak_bytes = marks.peak_bytes;
	out.memory_byte_ms = marks.byte_ms;
}

duckdb_state duckdb_resource_totals_read(duckdb_connection connection, duckdb_resource_totals *out) {
	if (!out) {
		return DuckDBError;
	}
	*out = duckdb_resource_totals();
	if (!connection) {
		return DuckDBError;
	}
	Connection *conn = reinterpret_cast<Connection *>(connection);
	try {
		ReadResourceTotals(*conn, *out);
	} catch (...) {
		return DuckDBError;
	}
	return DuckDBSuccess;
}

duckdb_state duckdb_begin_measured(duckdb_connection connection, duckdb_resource_totals *out_open) {
	if (!out_open) {
		return DuckDBError;
	}
	*out_open = duckdb_resource_totals();
	if (!connection) {
		return DuckDBError;
	}
	Connection *conn = reinterpret_cast<Connection *>(connection);
	optional_ptr<QueryProfiler> profiler;
	try {
		// an attempt that starts no query would otherwise read the tree of the previous statement
		profiler = QueryProfiler::Get(*conn->context);
		profiler->measured_bracket = true;
		profiler->Reset();
		DatabaseInstance::GetDatabase(*conn->context).GetBufferPool().SetMemoryPeak();
		ReadResourceTotals(*conn, *out_open);
	} catch (...) {
		// no bracket is open after an error: a caller that receives one does not call duckdb_end_measured
		if (profiler) {
			profiler->measured_bracket = false;
		}
		return DuckDBError;
	}
	return DuckDBSuccess;
}

duckdb_state duckdb_end_measured(duckdb_connection connection, duckdb_resource_totals *out_close, bool *out_abandoned) {
	if (out_abandoned) {
		*out_abandoned = false;
	}
	if (!out_close) {
		return DuckDBError;
	}
	*out_close = duckdb_resource_totals();
	if (!connection) {
		return DuckDBError;
	}
	Connection *conn = reinterpret_cast<Connection *>(connection);
	auto state = DuckDBSuccess;
	try {
		// end an execution that nobody finished, so its CPU stays with the interval that incurred it
		auto abandoned = conn->context->EndAbandonedQuery();
		if (out_abandoned) {
			*out_abandoned = abandoned;
		}
	} catch (...) {
		state = DuckDBError;
	}
	try {
		QueryProfiler::Get(*conn->context).measured_bracket = false;
		ReadResourceTotals(*conn, *out_close);
	} catch (...) {
		state = DuckDBError;
	}
	return state;
}
