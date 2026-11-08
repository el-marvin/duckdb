#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/thread.hpp"

#include <functional>

#ifdef _WIN32
#include "duckdb/common/windows.hpp"
#else
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace duckdb;

namespace {

// A macOS secondary thread gets this much stack by default; the nests below need several MiB on a release build.
constexpr idx_t SMALL_STACK_SIZE = 512 * 1024;
// A scalar macro costs about 1 KiB of stack per level on a release build, so its nest gets a smaller thread.
constexpr idx_t MACRO_STACK_SIZE = 192 * 1024;
constexpr idx_t DEEP_NEST = 400;
// A nest written out in one statement also makes the parsed statement deep, and destroying it recurses too.
constexpr idx_t DEEP_STATEMENT_NEST = 200;
// Five expression levels per macro keep the chain under max_expression_depth.
constexpr idx_t DEEP_MACRO_NEST = 190;
constexpr idx_t SHALLOW_NEST = 3;
// Enough for a macro cycle to reach a lowered max_expression_depth on any build.
constexpr idx_t LARGE_STACK_SIZE = 16 * 1024 * 1024;

struct SmallStackQuery {
	Connection &con;
	string sql;
	idx_t stack_size;
	bool stack_reported = false;
	bool has_error = false;
	string error;
	string value;
};

void RunQuery(SmallStackQuery &query) {
	idx_t stack_size;
	idx_t remaining;
	query.stack_reported = ThreadUtil::GetStackSpace(stack_size, remaining);
	try {
		auto result = query.con.Query(query.sql);
		if (result->HasError()) {
			query.has_error = true;
			query.error = result->GetError();
		} else {
			query.value = result->Collection().GetValue(0, 0).ToString();
		}
	} catch (std::exception &ex) {
		query.has_error = true;
		query.error = ex.what();
	}
}

#ifdef _WIN32
DWORD WINAPI RunQueryThread(LPVOID argument) {
	RunQuery(*static_cast<SmallStackQuery *>(argument));
	return 0;
}

bool RunOnSmallStack(SmallStackQuery &query) {
	auto thread =
	    CreateThread(nullptr, query.stack_size, RunQueryThread, &query, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
	if (!thread) {
		return false;
	}
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
	return true;
}
#else
void *RunQueryThread(void *argument) {
	RunQuery(*static_cast<SmallStackQuery *>(argument));
	return nullptr;
}

// The thread runs on a stack mapped here, since glibc hands a new thread any cached stack at least as large as the
// one requested.
bool RunOnSmallStack(SmallStackQuery &query) {
	auto page_size = static_cast<idx_t>(sysconf(_SC_PAGESIZE));
	auto mapping_size = query.stack_size + page_size;
	auto mapping = mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (mapping == MAP_FAILED) {
		return false;
	}
	// a guard page below the stack turns an overflow into a crash instead of a write into other memory
	auto stack = static_cast<char *>(mapping) + page_size;
	pthread_attr_t attributes;
	auto created = mprotect(mapping, page_size, PROT_NONE) == 0 && pthread_attr_init(&attributes) == 0;
	if (created) {
		pthread_t thread;
		created = pthread_attr_setstack(&attributes, stack, query.stack_size) == 0 &&
		          pthread_create(&thread, &attributes, RunQueryThread, &query) == 0;
		pthread_attr_destroy(&attributes);
		created = created && pthread_join(thread, nullptr) == 0;
	}
	munmap(mapping, mapping_size);
	return created;
}
#endif

SmallStackQuery QueryOnSmallStack(Connection &con, const string &sql, idx_t stack_size) {
	SmallStackQuery query {con, sql, stack_size};
	REQUIRE(RunOnSmallStack(query));
	return query;
}

void RequireStackError(Connection &con, const string &sql, idx_t stack_size = SMALL_STACK_SIZE) {
	auto query = QueryOnSmallStack(con, sql, stack_size);
	INFO(query.value);
	INFO(query.error);
	REQUIRE(query.has_error);
	REQUIRE(StringUtil::Contains(query.error,
	                             "Binder Error: Max expression depth exceeded: binding the query would overflow the"));
}

void RequireDepthError(Connection &con, const string &sql, const string &expected) {
	auto query = QueryOnSmallStack(con, sql, LARGE_STACK_SIZE);
	INFO(query.error);
	REQUIRE(query.has_error);
	REQUIRE(StringUtil::Contains(query.error, expected));
}

void RequireValue(Connection &con, const string &sql, const string &expected, idx_t stack_size = SMALL_STACK_SIZE) {
	auto query = QueryOnSmallStack(con, sql, stack_size);
	INFO(query.error);
	REQUIRE(!query.has_error);
	REQUIRE(query.value == expected);
}

// Each object first reads a constant and is then replaced from the top down, so no CREATE binds the whole nest.
void CreateChain(Connection &con, const string &object, const string &constant_body, const string &chained_body,
                 idx_t count) {
	for (idx_t i = 0; i <= count; i++) {
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("CREATE " + object + constant_body, i)));
	}
	for (idx_t i = count; i > 0; i -= 1) {
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("CREATE OR REPLACE " + object + chained_body, i, i - 1)));
	}
}

string NestedCTEs(idx_t count) {
	string query = "SELECT 42 AS a";
	for (idx_t i = 0; i < count; i++) {
		query = StringUtil::Format("WITH c%d AS (%s) SELECT * FROM c%d", i, query, i);
	}
	return query;
}

} // namespace

TEST_CASE("A nest deeper than the thread's stack raises the depth error", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET memory_limit='4GB'"));
	if (!QueryOnSmallStack(con, "SELECT 42", SMALL_STACK_SIZE).stack_reported) {
		// without the stack bounds only max_expression_depth applies, and the nests below would overflow
		WARN("The platform does not report the stack of a thread");
		return;
	}

	SECTION("views") {
		CreateChain(con, "VIEW v%d AS ", "SELECT 42 AS a", "SELECT * FROM v%d", DEEP_NEST);
		RequireStackError(con, StringUtil::Format("SELECT * FROM v%d", DEEP_NEST));
		RequireValue(con, StringUtil::Format("SELECT * FROM v%d", SHALLOW_NEST), "42");
	}
	SECTION("table macros") {
		CreateChain(con, "MACRO t%d() AS TABLE ", "SELECT 42 AS a", "SELECT * FROM t%d()", DEEP_NEST);
		RequireStackError(con, StringUtil::Format("SELECT * FROM t%d()", DEEP_NEST));
		RequireValue(con, StringUtil::Format("SELECT * FROM t%d()", SHALLOW_NEST), "42");
	}
	SECTION("common table expressions") {
		RequireStackError(con, NestedCTEs(DEEP_STATEMENT_NEST));
		RequireValue(con, NestedCTEs(SHALLOW_NEST), "42");
	}
	SECTION("scalar macros") {
		CreateChain(con, "MACRO m%d(x) AS ", "x", "m%d(x) + 1", DEEP_MACRO_NEST);
		RequireStackError(con, StringUtil::Format("SELECT m%d(42)", DEEP_MACRO_NEST), MACRO_STACK_SIZE);
		RequireValue(con, StringUtil::Format("SELECT m%d(42)", SHALLOW_NEST), "45", MACRO_STACK_SIZE);
	}
	// the connection is still usable on any thread
	RequireValue(con, "SELECT 42", "42");
	auto result = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
}

TEST_CASE("A macro cycle through a USING KEY aggregate reaches the depth limit", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET memory_limit='4GB'"));
	// the aggregates bind on a binder without a parent, which continues the depth of the recursive CTE
	const string body =
	    "WITH RECURSIVE r(k, v) USING KEY (k, max(%s()) AS v) AS (VALUES (1, 1) UNION SELECT k, v FROM r "
	    "WHERE false) SELECT max(v) FROM r";
	REQUIRE_NO_FAIL(con.Query("SET max_expression_depth=100"));

	SECTION("closed by dropping a shadow") {
		REQUIRE_NO_FAIL(con.Query("CREATE TEMP MACRO m() AS 1"));
		REQUIRE_NO_FAIL(con.Query("CREATE MACRO main.m() AS (" + StringUtil::Format(body, "m") + ")"));
		REQUIRE_NO_FAIL(con.Query("DROP MACRO temp.m"));
		RequireDepthError(con, "SELECT m()", "Max expression depth limit of 100 exceeded");
	}
	SECTION("closed by the search path") {
		REQUIRE_NO_FAIL(con.Query("CREATE SCHEMA s"));
		REQUIRE_NO_FAIL(con.Query("CREATE MACRO s.n() AS 1"));
		REQUIRE_NO_FAIL(con.Query("SET search_path='s,main'"));
		REQUIRE_NO_FAIL(con.Query("CREATE MACRO main.n() AS (" + StringUtil::Format(body, "n") + ")"));
		REQUIRE_NO_FAIL(con.Query("SET search_path='main'"));
		RequireDepthError(con, "SELECT n()", "Max expression depth limit of 100 exceeded");
	}
}
