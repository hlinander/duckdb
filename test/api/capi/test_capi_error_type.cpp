#include "capi_tester.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("Test the error type of a failed prepare in the C API", "[capi]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));

	// a null statement holds no failure
	REQUIRE(duckdb_prepare_error_type(nullptr) == DUCKDB_ERROR_INVALID);

	// a successful prepare holds no failure
	duckdb_prepared_statement stmt = nullptr;
	REQUIRE(duckdb_prepare(tester.connection, "SELECT 42", &stmt) == DuckDBSuccess);
	REQUIRE(duckdb_prepare_error(stmt) == nullptr);
	REQUIRE(duckdb_prepare_error_type(stmt) == DUCKDB_ERROR_INVALID);

	// a failed bind reports the type of the failure whose text duckdb_prepare_error returns
	REQUIRE(duckdb_bind_int32(stmt, 1, 1) == DuckDBError);
	REQUIRE(duckdb_prepare_error(stmt) != nullptr);
	REQUIRE(duckdb_prepare_error_type(stmt) == DUCKDB_ERROR_INVALID_INPUT);
	duckdb_destroy_prepare(&stmt);

	// a missing table is a catalog error
	REQUIRE(duckdb_prepare(tester.connection, "SELECT * FROM table_that_does_not_exist", &stmt) == DuckDBError);
	REQUIRE(stmt != nullptr);
	REQUIRE(duckdb_prepare_error(stmt) != nullptr);
	REQUIRE(duckdb_prepare_error_type(stmt) == DUCKDB_ERROR_CATALOG);
	duckdb_destroy_prepare(&stmt);

	// a syntax error is a parser error
	REQUIRE(duckdb_prepare(tester.connection, "SELEC 42", &stmt) == DuckDBError);
	REQUIRE(stmt != nullptr);
	REQUIRE(duckdb_prepare_error(stmt) != nullptr);
	REQUIRE(duckdb_prepare_error_type(stmt) == DUCKDB_ERROR_PARSER);
	duckdb_destroy_prepare(&stmt);

	// a missing column is a binder error
	REQUIRE(duckdb_prepare(tester.connection, "SELECT column_that_does_not_exist FROM (SELECT 1 AS a)", &stmt) == DuckDBError);
	REQUIRE(duckdb_prepare_error(stmt) != nullptr);
	REQUIRE(duckdb_prepare_error_type(stmt) == DUCKDB_ERROR_BINDER);
	duckdb_destroy_prepare(&stmt);

	// an extracted statement that fails to prepare reports its type through the same function
	duckdb_extracted_statements stmts = nullptr;
	REQUIRE(duckdb_extract_statements(tester.connection, "SELECT 1; SELECT * FROM table_that_does_not_exist", &stmts) ==
	        2);
	REQUIRE(duckdb_prepare_extracted_statement(tester.connection, stmts, 0, &stmt) == DuckDBSuccess);
	REQUIRE(duckdb_prepare_error_type(stmt) == DUCKDB_ERROR_INVALID);
	duckdb_destroy_prepare(&stmt);
	REQUIRE(duckdb_prepare_extracted_statement(tester.connection, stmts, 1, &stmt) == DuckDBError);
	REQUIRE(duckdb_prepare_error(stmt) != nullptr);
	REQUIRE(duckdb_prepare_error_type(stmt) == DUCKDB_ERROR_CATALOG);
	duckdb_destroy_prepare(&stmt);
	duckdb_destroy_extracted(&stmts);
}

TEST_CASE("Test the error type of a failed extraction in the C API", "[capi]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));

	// a null set holds no failure
	REQUIRE(duckdb_extract_statements_error_type(nullptr) == DUCKDB_ERROR_INVALID);

	// a successful extraction holds no failure
	duckdb_extracted_statements stmts = nullptr;
	REQUIRE(duckdb_extract_statements(tester.connection, "SELECT 1; SELECT 2", &stmts) == 2);
	REQUIRE(duckdb_extract_statements_error(stmts) == nullptr);
	REQUIRE(duckdb_extract_statements_error_type(stmts) == DUCKDB_ERROR_INVALID);
	duckdb_destroy_extracted(&stmts);

	// an extraction of zero statements holds no failure
	REQUIRE(duckdb_extract_statements(tester.connection, "", &stmts) == 0);
	REQUIRE(duckdb_extract_statements_error(stmts) == nullptr);
	REQUIRE(duckdb_extract_statements_error_type(stmts) == DUCKDB_ERROR_INVALID);
	duckdb_destroy_extracted(&stmts);

	// a syntax error is a parser error
	REQUIRE(duckdb_extract_statements(tester.connection, "SELECT 1; SELEC 2", &stmts) == 0);
	REQUIRE(stmts != nullptr);
	REQUIRE(duckdb_extract_statements_error(stmts) != nullptr);
	REQUIRE(duckdb_extract_statements_error_type(stmts) == DUCKDB_ERROR_PARSER);
	duckdb_destroy_extracted(&stmts);
}
