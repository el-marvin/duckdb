#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

using namespace duckdb;

static unique_ptr<SelectStatement> ParseSelect(Connection &con, const string &sql) {
	Parser parser(*con.context, IdentifierCaseMode::PRESERVE_CASE);
	parser.ParseQuery(sql);
	REQUIRE(parser.statements.size() == 1);
	REQUIRE(parser.statements[0]->type == StatementType::SELECT_STATEMENT);
	return unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
}

static unique_ptr<QueryNode> ParseNode(Connection &con, const string &sql) {
	return std::move(ParseSelect(con, sql)->node);
}

template <class CHANGE>
static void RequireChangeBreaksEquality(const string &field, const QueryNode &original, CHANGE &&change) {
	INFO(field);
	auto copy = original.Copy();
	REQUIRE(original.Equals(copy.get()));
	REQUIRE(copy->Equals(&original));
	change(*copy);
	CHECK(!original.Equals(copy.get()));
	CHECK(!copy->Equals(&original));
}

static CommonTableExpressionInfo &CTE(QueryNode &node, const string &name) {
	auto entry = node.cte_map.map.find(Identifier(name));
	REQUIRE(entry != node.cte_map.map.end());
	return *entry->second;
}

TEST_CASE("Select nodes that differ in their aggregate handling are not equal", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto group_by_all = ParseNode(con, "SELECT x FROM t GROUP BY ALL");
	REQUIRE(group_by_all->Cast<SelectNode>().aggregate_handling == AggregateHandling::FORCE_AGGREGATES);
	RequireChangeBreaksEquality("standard handling", *group_by_all, [](QueryNode &copy) {
		copy.Cast<SelectNode>().aggregate_handling = AggregateHandling::STANDARD_HANDLING;
	});
	RequireChangeBreaksEquality("no aggregates allowed", *group_by_all, [](QueryNode &copy) {
		copy.Cast<SelectNode>().aggregate_handling = AggregateHandling::NO_AGGREGATES_ALLOWED;
	});
	CHECK(!group_by_all->Equals(ParseNode(con, "SELECT x FROM t").get()));
}

TEST_CASE("Orders that differ in their null order are not equal", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto null_orders = {OrderByNullType::ORDER_DEFAULT, OrderByNullType::NULLS_FIRST, OrderByNullType::NULLS_LAST};
	for (auto &sql : {"SELECT x FROM t ORDER BY y, x NULLS FIRST", "(SELECT 1 AS x) UNION (SELECT 2) ORDER BY 1, x",
	                  "SELECT first(x ORDER BY y, x NULLS LAST) FROM t"}) {
		auto node = ParseNode(con, sql);
		auto order_of = [](QueryNode &node) -> OrderByNode & {
			auto &modifier = node.modifiers.empty()
			                     ? *node.Cast<SelectNode>().select_list[0]->Cast<FunctionExpression>().OrderByMutable()
			                     : node.modifiers[0]->Cast<OrderModifier>();
			REQUIRE(modifier.orders.size() == 2);
			return modifier.orders[1];
		};
		for (auto null_order : null_orders) {
			if (null_order == order_of(*node).null_order) {
				continue;
			}
			RequireChangeBreaksEquality(string(sql) + " " + EnumUtil::ToString(null_order), *node,
			                            [&](QueryNode &copy) { order_of(copy).null_order = null_order; });
		}
	}
	CHECK(!ParseNode(con, "SELECT first(x ORDER BY x NULLS FIRST) FROM t")
	           ->Equals(ParseNode(con, "SELECT first(x ORDER BY x NULLS LAST) FROM t").get()));
}

TEST_CASE("Common table expressions that differ in a field are not equal", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto node = ParseNode(con, "WITH a AS (SELECT * FROM b), b AS MATERIALIZED (SELECT 2 AS v) SELECT v FROM a");
	RequireChangeBreaksEquality("materialized", *node, [](QueryNode &copy) {
		CTE(copy, "b").materialized = CTEMaterialize::CTE_MATERIALIZE_NEVER;
	});
	RequireChangeBreaksEquality("default materialization", *node, [](QueryNode &copy) {
		CTE(copy, "b").materialized = CTEMaterialize::CTE_MATERIALIZE_DEFAULT;
	});
	RequireChangeBreaksEquality("definition order", *node, [](QueryNode &copy) {
		auto &map = copy.cte_map.map;
		decltype(copy.cte_map.map) reversed;
		for (auto entry = map.rbegin(); entry != map.rend(); ++entry) {
			reversed.insert(entry->first, std::move(entry->second));
		}
		map = std::move(reversed);
	});
	CHECK(!node->Equals(
	    ParseNode(con, "WITH b AS MATERIALIZED (SELECT 2 AS v), a AS (SELECT * FROM b) SELECT v FROM a").get()));

	// the trigger expansion marks the CTEs it generates while binding; the query is the same
	auto copy = node->Copy();
	CTE(*copy, "a").is_trigger_generated = true;
	CHECK(node->Equals(copy.get()));
}

TEST_CASE("Recursive CTE nodes that differ in a field are not equal", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto node = ParseNode(con, "WITH RECURSIVE t(k, a, c) USING KEY (k, sum(a) AS a) AS (SELECT 1, 1, 10 UNION ALL "
	                           "SELECT k, a + 1, c + 1 FROM t WHERE a < 3) SELECT * FROM t");
	RequireChangeBreaksEquality("CTE key target alias", *node,
	                            [](QueryNode &copy) { CTE(copy, "t").key_targets[1]->SetAlias(Identifier("c")); });

	auto &recursive_node = *CTE(*node, "t").query_node;
	REQUIRE(recursive_node.type == QueryNodeType::RECURSIVE_CTE_NODE);
	RequireChangeBreaksEquality("key target alias", recursive_node, [](QueryNode &copy) {
		copy.Cast<RecursiveCTENode>().key_targets[1]->SetAlias(Identifier("c"));
	});
	RequireChangeBreaksEquality("key target", recursive_node,
	                            [](QueryNode &copy) { copy.Cast<RecursiveCTENode>().key_targets.pop_back(); });
	RequireChangeBreaksEquality("ctename", recursive_node,
	                            [](QueryNode &copy) { copy.Cast<RecursiveCTENode>().ctename = Identifier("u"); });
	RequireChangeBreaksEquality("aliases", recursive_node, [](QueryNode &copy) {
		auto &aliases = copy.Cast<RecursiveCTENode>().aliases;
		std::swap(aliases[1], aliases[2]);
	});
	RequireChangeBreaksEquality("union_all", recursive_node,
	                            [](QueryNode &copy) { copy.Cast<RecursiveCTENode>().union_all = false; });
}

TEST_CASE("Query nodes that differ in the spelling of an output name are not equal", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto select = ParseNode(con, "SELECT 1 AS x, 2 AS y");
	RequireChangeBreaksEquality("select alias spelling", *select, [](QueryNode &copy) {
		copy.Cast<SelectNode>().select_list[0]->SetAlias(Identifier("X"));
	});

	auto cte = ParseNode(con, "WITH q(x, y) AS (SELECT 1, 2) SELECT * FROM q");
	RequireChangeBreaksEquality("CTE alias spelling", *cte,
	                            [](QueryNode &copy) { CTE(copy, "q").aliases[0] = Identifier("X"); });

	auto recursive = ParseNode(con, "WITH RECURSIVE r(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM r WHERE x < 3) "
	                                "SELECT * FROM r");
	auto &recursive_node = *CTE(*recursive, "r").query_node;
	REQUIRE(recursive_node.type == QueryNodeType::RECURSIVE_CTE_NODE);
	RequireChangeBreaksEquality("recursive CTE alias spelling", recursive_node,
	                            [](QueryNode &copy) { copy.Cast<RecursiveCTENode>().aliases[0] = Identifier("X"); });

	// a CTE name is resolved case insensitively by its references
	auto renamed = cte->Copy();
	auto entry = std::move(renamed->cte_map.map.begin()->second);
	renamed->cte_map.map.clear();
	renamed->cte_map.map.insert(Identifier("Q"), std::move(entry));
	CHECK(cte->Equals(renamed.get()));
}

static void RequireCopyEquals(const SelectStatement &statement) {
	CHECK(statement.node->Equals(statement.node->Copy().get()));
}

static void RequireReparseEquals(Connection &con, const SelectStatement &statement) {
	auto reparsed = ParseSelect(con, statement.ToString());
	CHECK(statement.node->Equals(reparsed->node.get()));
}

static void RequireDeserializedEquals(const SelectStatement &statement) {
	Allocator allocator;
	MemoryStream stream(allocator);
	SerializationOptions options;
	options.storage_compatibility = StorageCompatibility::FromString("latest");
	BinarySerializer::Serialize(statement, stream, options);
	stream.Rewind();
	auto deserialized = BinaryDeserializer::Deserialize<SelectStatement>(stream);
	CHECK(statement.node->Equals(deserialized->node.get()));
}

TEST_CASE("Query nodes equal their copy and their reparsed and deserialized forms", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<string> queries {
	    "SELECT x FROM t GROUP BY ALL",
	    "SELECT x, count(*) FROM t GROUP BY x HAVING count(*) > 1 QUALIFY row_number() OVER () = 1",
	    "SELECT x, y FROM t GROUP BY GROUPING SETS ((x), (x, y), ())",
	    "SELECT x, y FROM t GROUP BY ROLLUP (x, y)",
	    "SELECT x FROM t WHERE x > 1 ORDER BY x DESC NULLS FIRST, y NULLS LAST, z ASC LIMIT 2 OFFSET 1",
	    "SELECT x FROM t ORDER BY x COLLATE nocase LIMIT 10 %",
	    "SELECT * FROM t ORDER BY ALL DESC NULLS FIRST",
	    "SELECT DISTINCT ON (x) x, y FROM t ORDER BY x, y NULLS FIRST",
	    "SELECT DISTINCT x FROM t",
	    "SELECT first(x ORDER BY x DESC NULLS FIRST) FILTER (WHERE x > 1) FROM t",
	    "SELECT sum(x) OVER w, first(x IGNORE NULLS) OVER (w ROWS BETWEEN 1 PRECEDING AND CURRENT ROW EXCLUDE TIES) "
	    "FROM t WINDOW w AS (PARTITION BY y ORDER BY x NULLS FIRST)",
	    "(SELECT 1 AS x) UNION ALL BY NAME (SELECT 2 AS x) ORDER BY x NULLS FIRST",
	    "(SELECT 1) UNION (SELECT 2) UNION (SELECT 3)",
	    "(SELECT 1) EXCEPT ALL (SELECT 2)",
	    "(SELECT 1) INTERSECT (SELECT 2) LIMIT 1",
	    "WITH a AS (SELECT * FROM b), b AS MATERIALIZED (SELECT 2 AS v), c AS NOT MATERIALIZED (SELECT 3) SELECT v "
	    "FROM a",
	    "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM t WHERE n < 3) SELECT * FROM t",
	    "WITH RECURSIVE t(k, a, c) USING KEY (k, sum(a) AS a) AS (SELECT 1, 1, 10 UNION ALL SELECT k, a + 1, c + 1 "
	    "FROM t WHERE a < 3) SELECT * FROM t",
	    "SELECT (SELECT x FROM t GROUP BY ALL ORDER BY x NULLS FIRST LIMIT 1)",
	};
	for (auto &sql : queries) {
		INFO(sql);
		auto statement = ParseSelect(con, sql);
		RequireCopyEquals(*statement);
		RequireReparseEquals(con, *statement);
		RequireDeserializedEquals(*statement);
	}
}
