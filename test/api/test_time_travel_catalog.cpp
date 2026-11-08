#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

using namespace duckdb;

namespace {

//! The AT clause of every lookup that carried one, as "<unit> <value>"
vector<string> looked_up_at;

//! A catalog that accepts every AT clause and reads the current state, as no built-in catalog supports time travel
class TimeTravelCatalog : public DuckCatalog {
public:
	explicit TimeTravelCatalog(AttachedDatabase &db) : DuckCatalog(db) {
	}

	bool SupportsTimeTravel() const override {
		return true;
	}

	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override {
		auto at_clause = schema_lookup.GetAtClause();
		if (at_clause) {
			looked_up_at.push_back(at_clause->Unit().GetIdentifierName() + " " + at_clause->GetValue().ToString());
		}
		return DuckCatalog::LookupSchema(transaction, schema_lookup, if_not_found);
	}
};

struct TimeTravelStorageExtension : StorageExtension {
	TimeTravelStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &info, AttachOptions &) -> unique_ptr<Catalog> {
			info.path = IN_MEMORY_PATH;
			return make_uniq_base<Catalog, TimeTravelCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

void RequireTimeTravelError(Connection &con, const string &query, const string &message) {
	auto result = con.Query(query);
	INFO(query);
	REQUIRE_FAIL(result);
	REQUIRE(result->GetErrorType() == ExceptionType::BINDER);
	REQUIRE(StringUtil::Contains(result->GetError(), message));
}

} // namespace

TEST_CASE("An AT clause applies only to a reference the catalog resolves", "[api][time_travel]") {
	DBConfig config;
	StorageExtension::Register(config, "time_travel_test", make_shared_ptr<TimeTravelStorageExtension>());
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("ATTACH 'versioned' AS versioned (TYPE time_travel_test)"));
	REQUIRE_NO_FAIL(con.Query("USE versioned"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE tbl AS SELECT 42 AS i"));
	REQUIRE_NO_FAIL(con.Query("CREATE VIEW v AS SELECT i + 1 AS j FROM tbl"));

	// a table and a view of the catalog take the clause to the lookup, and a view passes it on to its body
	looked_up_at.clear();
	auto result = con.Query("SELECT * FROM tbl AT (VERSION => 1)");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	REQUIRE(looked_up_at == vector<string> {"VERSION 1"});
	looked_up_at.clear();
	result = con.Query("SELECT * FROM v AT (TIMESTAMP => TIMESTAMP '2020-01-01')");
	REQUIRE(CHECK_COLUMN(result, 0, {43}));
	REQUIRE(looked_up_at == vector<string> {"TIMESTAMP 2020-01-01 00:00:00", "TIMESTAMP 2020-01-01 00:00:00"});
	// a name the catalog does not have keeps the catalog's error, which names the version
	result = con.Query("SELECT * FROM nonexistent AT (VERSION => 1)");
	REQUIRE_FAIL(result);
	REQUIRE(result->GetErrorType() == ExceptionType::CATALOG);
	REQUIRE(StringUtil::Contains(result->GetError(), "does not exist at version 1"));

	// a CTE takes the name before the catalog, so its AT clause is refused rather than dropped
	RequireTimeTravelError(con, "WITH tbl AS (SELECT 1 AS a) SELECT * FROM tbl AT (VERSION => 1)",
	                       "CTE \"tbl\" does not support time travel");
	result = con.Query("WITH tbl AS (SELECT 1 AS a) SELECT * FROM tbl");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	// a qualified name is not a CTE reference, so it reaches the table the CTE shadows
	looked_up_at.clear();
	result = con.Query("WITH tbl AS (SELECT 1 AS a) SELECT * FROM main.tbl AT (VERSION => 2)");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	REQUIRE(looked_up_at == vector<string> {"VERSION 2"});
	auto names = con.GetTableNames("SELECT * FROM tbl AT (VERSION => 1)");
	REQUIRE(names == unordered_set<string> {"tbl"});
	REQUIRE_THROWS_AS(con.GetTableNames("WITH tbl AS (SELECT 1 AS a) SELECT * FROM tbl AT (VERSION => 1)"),
	                  BinderException);
	// the AT clause of a view reaches its body, where a CTE is read as it is
	REQUIRE_NO_FAIL(con.Query("CREATE VIEW cte_view AS WITH c AS (SELECT 5 AS x) SELECT * FROM c"));
	result = con.Query("SELECT * FROM cte_view AT (VERSION => 1)");
	REQUIRE(CHECK_COLUMN(result, 0, {5}));
	// a CTE that cannot be referenced yet leaves the name to the catalog
	result = con.Query("WITH tbl AS (SELECT * FROM tbl AT (VERSION => 1)) SELECT * FROM tbl");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));

	// a name the catalog does not know at the version falls to a replacement scan, which has no versions
	auto path = TestCreatePath("time_travel_replacement.csv");
	REQUIRE_NO_FAIL(con.Query("COPY (SELECT 7 AS k) TO '" + path + "'"));
	RequireTimeTravelError(con, "SELECT * FROM '" + path + "' AT (VERSION => 1)",
	                       "Replacement scan \"" + path + "\" does not support time travel");
	// the replacement is refused before it is bound, so a missing file gives no IO error
	auto missing_path = TestCreatePath("time_travel_missing.csv");
	RequireTimeTravelError(con, "SELECT * FROM '" + missing_path + "' AT (VERSION => 1)",
	                       "Replacement scan \"" + missing_path + "\" does not support time travel");
	result = con.Query("SELECT * FROM '" + path + "'");
	REQUIRE(CHECK_COLUMN(result, 0, {7}));
	// the AT clause of a view reaches its body, where a replacement scan reads the file as it is
	REQUIRE_NO_FAIL(con.Query("CREATE VIEW file_view AS SELECT * FROM '" + path + "'"));
	result = con.Query("SELECT * FROM file_view AT (VERSION => 1)");
	REQUIRE(CHECK_COLUMN(result, 0, {7}));
}
