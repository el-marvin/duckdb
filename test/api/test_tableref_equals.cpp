#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/parser/tableref/delimgetref.hpp"
#include "duckdb/parser/tableref/list.hpp"

using namespace duckdb;

static unique_ptr<TableRef> ParseFrom(Connection &con, const string &from) {
	Parser parser(*con.context);
	parser.ParseQuery("SELECT * FROM " + from);
	REQUIRE(parser.statements.size() == 1);
	auto &select = parser.statements[0]->Cast<SelectStatement>();
	auto result = std::move(select.node->Cast<SelectNode>().from_table);
	REQUIRE(result);
	return result;
}

template <class TARGET>
static TARGET &InnerRef(TableRef &subquery_ref) {
	auto &subquery = subquery_ref.Cast<SubqueryRef>();
	return subquery.subquery->node->Cast<SelectNode>().from_table->Cast<TARGET>();
}

template <class CHANGE>
static void RequireChangeBreaksEquality(const string &field, TableRef &original, CHANGE &&change) {
	INFO(field);
	auto copy = original.Copy();
	REQUIRE(original.Equals(*copy));
	REQUIRE(copy->Equals(original));
	change(*copy);
	CHECK(!original.Equals(*copy));
	CHECK(!copy->Equals(original));
}

TEST_CASE("Table references that differ in their column name aliases are not equal", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<string> sources {"tbl t(x, y)", "(SELECT 1, 2) t(x, y)", "range(1) WITH ORDINALITY t(x, y)",
	                        "src PIVOT (sum(v) FOR k IN ('a', 'b')) AS t(x, y)",
	                        "src MATCH_RECOGNIZE (ORDER BY o MEASURES first(o) AS f PATTERN (A) DEFINE A AS v > 0) AS "
	                        "t(x, y)"};
	for (auto &source : sources) {
		auto ref = ParseFrom(con, source);
		REQUIRE(ref->column_name_alias.size() == 2);
		RequireChangeBreaksEquality(
		    source, *ref, [](TableRef &copy) { std::swap(copy.column_name_alias[0], copy.column_name_alias[1]); });
		RequireChangeBreaksEquality(source, *ref, [](TableRef &copy) { copy.column_name_alias.pop_back(); });
	}

	// kinds that SQL never gives column name aliases still copy and compare the base field
	vector<unique_ptr<TableRef>> refs;
	refs.push_back(ParseFrom(con, "l CROSS JOIN r"));
	refs.push_back(make_uniq<EmptyTableRef>());
	refs.push_back(make_uniq<DelimGetRef>(vector<LogicalType> {LogicalType::INTEGER}));
	for (auto &ref : refs) {
		ref->alias = Identifier("t");
		ref->column_name_alias.emplace_back("x");
		RequireChangeBreaksEquality(EnumUtil::ToString(ref->type), *ref,
		                            [](TableRef &copy) { copy.column_name_alias[0] = Identifier("y"); });
	}
}

TEST_CASE("Join references that differ in their join kind are not equal", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto cross = ParseFrom(con, "l CROSS JOIN r");
	RequireChangeBreaksEquality("ref_type", *cross,
	                            [](TableRef &copy) { copy.Cast<JoinRef>().ref_type = JoinRefType::POSITIONAL; });
	auto asof = ParseFrom(con, "l ASOF JOIN r ON l.a >= r.a");
	RequireChangeBreaksEquality("ref_type", *asof,
	                            [](TableRef &copy) { copy.Cast<JoinRef>().ref_type = JoinRefType::REGULAR; });
	auto regular = ParseFrom(con, "l JOIN r ON l.a = r.a");
	RequireChangeBreaksEquality("delim_flipped", *regular,
	                            [](TableRef &copy) { copy.Cast<JoinRef>().delim_flipped = true; });

	// a comma list and CROSS JOIN bind to the same relation, so is_implicit is left out of Equals
	auto implicit = ParseFrom(con, "l, r");
	REQUIRE(implicit->Cast<JoinRef>().is_implicit);
	CHECK(implicit->Equals(*cross));
	CHECK(cross->Equals(*implicit));
}

TEST_CASE("Table references that differ in a sample option are not equal", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	for (auto &source :
	     {"tbl TABLESAMPLE reservoir(10 ROWS)", "tbl TABLESAMPLE bernoulli(10 PERCENT) REPEATABLE (1)"}) {
		auto ref = ParseFrom(con, source);
		REQUIRE(ref->sample);
		RequireChangeBreaksEquality(string(source) + " sample_size", *ref,
		                            [](TableRef &copy) { copy.sample->sample_size = Value::BIGINT(20); });
		RequireChangeBreaksEquality(string(source) + " is_percentage", *ref,
		                            [](TableRef &copy) { copy.sample->is_percentage = !copy.sample->is_percentage; });
		RequireChangeBreaksEquality(string(source) + " method", *ref,
		                            [](TableRef &copy) { copy.sample->method = SampleMethod::SYSTEM_SAMPLE; });
		RequireChangeBreaksEquality(string(source) + " seed", *ref,
		                            [](TableRef &copy) { copy.sample->seed = optional_idx(2); });
		RequireChangeBreaksEquality(string(source) + " repeatable", *ref,
		                            [](TableRef &copy) { copy.sample->repeatable = !copy.sample->repeatable; });
		RequireChangeBreaksEquality(string(source) + " sample_rate", *ref,
		                            [](TableRef &copy) { copy.sample->sample_rate = 0.5; });
		RequireChangeBreaksEquality(string(source) + " no sample", *ref, [](TableRef &copy) { copy.sample.reset(); });
	}

	auto select_sample = [&](const string &sql) {
		Parser parser(*con.context);
		parser.ParseQuery(sql);
		REQUIRE(parser.statements.size() == 1);
		return parser.statements[0]->Cast<SelectStatement>().node->Copy();
	};
	auto ten = select_sample("SELECT * FROM tbl USING SAMPLE 10 ROWS");
	auto twenty = select_sample("SELECT * FROM tbl USING SAMPLE 20 ROWS");
	CHECK(ten->Equals(ten->Copy().get()));
	CHECK(!ten->Equals(twenty.get()));
}

TEST_CASE("Table references that differ in a field of their kind are not equal", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto ordinality = ParseFrom(con, "range(3) WITH ORDINALITY");
	RequireChangeBreaksEquality("with_ordinality", *ordinality, [](TableRef &copy) {
		copy.Cast<TableFunctionRef>().with_ordinality = OrdinalityType::WITHOUT_ORDINALITY;
	});

	auto values = ParseFrom(con, "(VALUES (1, 2))");
	auto &list = InnerRef<ExpressionListRef>(*values);
	list.expected_names = {Identifier("x"), Identifier("y")};
	list.expected_types = {LogicalType::INTEGER, LogicalType::INTEGER};
	RequireChangeBreaksEquality("expected_names", list, [](TableRef &copy) {
		std::swap(copy.Cast<ExpressionListRef>().expected_names[0], copy.Cast<ExpressionListRef>().expected_names[1]);
	});
	RequireChangeBreaksEquality("expected_types", list, [](TableRef &copy) {
		copy.Cast<ExpressionListRef>().expected_types[1] = LogicalType::BIGINT;
	});

	DelimGetRef delim_get(vector<LogicalType> {LogicalType::INTEGER});
	REQUIRE(!delim_get.Equals(DelimGetRef(vector<LogicalType> {LogicalType::BIGINT})));
	REQUIRE(!delim_get.Equals(DelimGetRef(vector<LogicalType> {LogicalType::INTEGER, LogicalType::INTEGER})));
	REQUIRE(delim_get.Equals(*delim_get.Copy()));

	auto pivot = ParseFrom(con, "src PIVOT (sum(v) AS s, max(v) AS m FOR k IN ('a'))");
	RequireChangeBreaksEquality("aggregate alias", *pivot, [](TableRef &copy) {
		copy.Cast<PivotRef>().aggregates[0]->SetAlias(Identifier("m"));
		copy.Cast<PivotRef>().aggregates[1]->SetAlias(Identifier("s"));
	});

	auto match = ParseFrom(con, "src MATCH_RECOGNIZE (ORDER BY o MEASURES first(o) AS f, last(o) AS l PATTERN (A B) "
	                            "DEFINE A AS v < 3, B AS v >= 3)");
	RequireChangeBreaksEquality("measure alias", *match, [](TableRef &copy) {
		auto &measures = copy.Cast<MatchRecognizeRef>().config->measures_expression_list;
		measures[0]->SetAlias(Identifier("l"));
		measures[1]->SetAlias(Identifier("f"));
	});
	RequireChangeBreaksEquality("define alias", *match, [](TableRef &copy) {
		auto &defines = copy.Cast<MatchRecognizeRef>().config->defines_expression_list;
		defines[0]->SetAlias(Identifier("B"));
		defines[1]->SetAlias(Identifier("A"));
	});
}

static unique_ptr<SelectStatement> ParseSelect(Connection &con, const string &sql) {
	Parser parser(*con.context, IdentifierCaseMode::PRESERVE_CASE);
	parser.ParseQuery(sql);
	REQUIRE(parser.statements.size() == 1);
	REQUIRE(parser.statements[0]->type == StatementType::SELECT_STATEMENT);
	return unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
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

TEST_CASE("Table references equal their copy, their reparsed text and their deserialized form", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<string> queries {"SELECT 1",
	                        "SELECT * FROM tbl t(x, y) TABLESAMPLE reservoir(10 ROWS)",
	                        "SELECT * FROM tbl AT (VERSION => 1)",
	                        "SELECT * FROM tbl TABLESAMPLE bernoulli(10 PERCENT) REPEATABLE (1)",
	                        "SELECT * FROM tbl USING SAMPLE 10 PERCENT (system, 2)",
	                        "SELECT * FROM l, r",
	                        "SELECT * FROM l CROSS JOIN r",
	                        "SELECT * FROM l POSITIONAL JOIN r",
	                        "SELECT * FROM l NATURAL JOIN r",
	                        "SELECT * FROM l ASOF LEFT JOIN r ON l.a >= r.a",
	                        "SELECT * FROM l JOIN r USING (a)",
	                        "SELECT * FROM l SEMI JOIN r ON l.a = r.a",
	                        "SELECT * FROM l, LATERAL (SELECT l.a) t(x)",
	                        "SELECT * FROM (SELECT 1, 2) t(x, y)",
	                        "SELECT * FROM range(3) WITH ORDINALITY t(x, y)",
	                        "SELECT * FROM src PIVOT (sum(v) AS s, max(v) AS m FOR k IN ('a', 'b')) AS p(x, y)",
	                        "SELECT * FROM (PIVOT src ON k IN ('a') USING sum(v) AS s GROUP BY g)",
	                        "SELECT * FROM src UNPIVOT INCLUDE NULLS (val FOR n IN (a, b)) AS u(x, y)",
	                        "SELECT * FROM range(3) WITH ORDINALITY AS t"};
	for (auto &sql : queries) {
		INFO(sql);
		auto statement = ParseSelect(con, sql);
		RequireCopyEquals(*statement);
		RequireReparseEquals(con, *statement);
		RequireDeserializedEquals(*statement);
	}

	// VALUES and MATCH_RECOGNIZE gain a nesting level when printed and reparsed
	vector<string> not_reparsed {
	    "SELECT * FROM (VALUES (1, 2), (3, 4)) t(x, y)",
	    "SELECT * FROM src MATCH_RECOGNIZE (PARTITION BY p ORDER BY o MEASURES first(o) AS f, last(o) AS l ONE ROW "
	    "PER MATCH AFTER MATCH SKIP PAST LAST ROW PATTERN (A B+) DEFINE A AS v < 3, B AS v >= 3) AS m(x, y)",
	};
	for (auto &sql : not_reparsed) {
		INFO(sql);
		auto statement = ParseSelect(con, sql);
		RequireCopyEquals(*statement);
		RequireDeserializedEquals(*statement);
	}

	// a SHOW reference gains an empty catalog and schema when deserialized
	for (auto &sql : {"SELECT * FROM (DESCRIBE tbl)", "SELECT * FROM (SUMMARIZE SELECT 1)"}) {
		INFO(sql);
		auto statement = ParseSelect(con, sql);
		RequireCopyEquals(*statement);
		RequireReparseEquals(con, *statement);
	}
}
