#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/expression/pattern_expression.hpp"
#include "duckdb/parser/parser.hpp"

using namespace duckdb;

static unique_ptr<ParsedExpression> ParseExpression(Connection &con, const string &sql) {
	Parser parser(*con.context, IdentifierCaseMode::PRESERVE_CASE);
	auto expressions = parser.ParseExpressionList(sql);
	REQUIRE(expressions.size() == 1);
	return std::move(expressions[0]);
}

static void RequireEqualAndHashAlike(const ParsedExpression &a, const ParsedExpression &b) {
	CHECK(a.Equals(b));
	CHECK(b.Equals(a));
	CHECK(a.Hash() == b.Hash());
}

static void RequireNotEqual(const ParsedExpression &a, const ParsedExpression &b) {
	CHECK(!a.Equals(b));
	CHECK(!b.Equals(a));
}

// Each pair differs in exactly one field that changes the value, the name or the binding rule.
static const vector<std::pair<string, string>> DIFFERENT_PAIRS {
    // BETWEEN: input, lower, upper
    {"2 BETWEEN 1 AND 3", "3 BETWEEN 1 AND 3"},
    {"2 BETWEEN 1 AND 3", "2 BETWEEN 0 AND 3"},
    {"2 BETWEEN 1 AND 3", "2 BETWEEN 1 AND 4"},
    // CASE: when, then, else
    {"CASE WHEN a THEN 1 ELSE 2 END", "CASE WHEN b THEN 1 ELSE 2 END"},
    {"CASE WHEN a THEN 1 ELSE 2 END", "CASE WHEN a THEN 3 ELSE 2 END"},
    {"CASE WHEN a THEN 1 ELSE 2 END", "CASE WHEN a THEN 1 ELSE 3 END"},
    {"CASE WHEN a THEN 1 ELSE 2 END", "CASE WHEN a THEN 1 WHEN b THEN 1 ELSE 2 END"},
    // CAST: child, target type, try
    {"CAST(a AS INTEGER)", "CAST(b AS INTEGER)"},
    {"CAST(a AS INTEGER)", "CAST(a AS BIGINT)"},
    {"CAST(a AS INTEGER)", "TRY_CAST(a AS INTEGER)"},
    // type expressions: name, parameters, struct field names (order and spelling)
    {"CAST(a AS DECIMAL(10, 2))", "CAST(a AS DECIMAL(10, 3))"},
    {"CAST(a AS STRUCT(x INTEGER, y INTEGER))", "CAST(a AS STRUCT(y INTEGER, x INTEGER))"},
    {"CAST(a AS STRUCT(x INTEGER))", "CAST(a AS STRUCT(X INTEGER))"},
    {"CAST(a AS STRUCT(x INTEGER))", "CAST(a AS STRUCT(x BIGINT))"},
    {"CAST(a AS UNION(x INTEGER, y VARCHAR))", "CAST(a AS UNION(y INTEGER, x VARCHAR))"},
    // COLLATE: child, collation
    {"a COLLATE nocase", "b COLLATE nocase"},
    {"a COLLATE nocase", "a COLLATE noaccent"},
    // column references
    {"a", "b"},
    {"t.a", "u.a"},
    {"a", "t.a"},
    // comparisons: kind, left, right
    {"a = b", "a <> b"},
    {"a = b", "c = b"},
    {"a = b", "a = c"},
    // conjunctions: kind, children
    {"a AND b", "a OR b"},
    {"a AND b", "a AND c"},
    {"a AND b", "a AND b AND c"},
    // constants: the literal text names the column
    {"1", "2"},
    {"1.0", "1.00"},
    {"'a'", "'b'"},
    {"1", "'1'"},
    // functions: name, arguments, argument names, DISTINCT, FILTER, ORDER BY, EXPORT_STATE, operator spelling
    {"f(a)", "g(a)"},
    {"f(a)", "f(b)"},
    {"f(a)", "f(a, b)"},
    {"f(x := a)", "f(y := a)"},
    {"f(x := a)", "f(X := a)"},
    {"f(a)", "s.f(a)"},
    {"count(a)", "count(DISTINCT a)"},
    {"count(a) FILTER (WHERE b)", "count(a) FILTER (WHERE c)"},
    {"count(a) FILTER (WHERE b)", "count(a)"},
    {"string_agg(a ORDER BY b)", "string_agg(a ORDER BY c)"},
    {"string_agg(a ORDER BY b)", "string_agg(a ORDER BY b DESC)"},
    {"string_agg(a ORDER BY b)", "string_agg(a)"},
    {"count(a)", "count(a) EXPORT_STATE"},
    {"1 + 2", "\"+\"(1, 2)"},
    {"j -> 'a' ->> 'b'", "j -> \"->>\"('a', 'b')"},
    {"{'x': 1}", "{'X': 1}"},
    {"{'x': 1, 'y': 2}", "{'y': 1, 'x': 2}"},
    {"struct_pack(x := 1)", "struct_pack(X := 1)"},
    // lambdas: parameters, body, syntax
    {"list_transform(l, lambda x: x + 1)", "list_transform(l, lambda x: x + 2)"},
    {"list_transform(l, lambda x: x + 1)", "list_transform(l, lambda y: x + 1)"},
    {"list_transform(l, lambda x: x + 1)", "list_transform(l, x -> x + 1)"},
    // operators: kind, children
    {"a IS NULL", "a IS NOT NULL"},
    {"a IS NULL", "b IS NULL"},
    {"NOT a", "NOT b"},
    {"a IN (1, 2)", "a IN (1, 3)"},
    {"a IN (1, 2)", "a NOT IN (1, 2)"},
    // parameters and positional references
    {"$1", "$2"},
    {"$a", "$b"},
    {"#1", "#2"},
    // stars: relation, EXCLUDE, REPLACE, RENAME (target spelling), COLUMNS
    {"*", "t.*"},
    {"* EXCLUDE (a)", "* EXCLUDE (b)"},
    {"* REPLACE (a + 1 AS a)", "* REPLACE (a + 2 AS a)"},
    {"* REPLACE (a + 1 AS a)", "* REPLACE (a + 1 AS b)"},
    {"* RENAME (a AS x)", "* RENAME (a AS y)"},
    {"* RENAME (a AS x)", "* RENAME (a AS X)"},
    {"* RENAME (a AS x)", "* RENAME (b AS x)"},
    {"* RENAME (t.a AS x)", "* RENAME (a AS x)"},
    {"* REPLACE (a + 1 AS a)", "* REPLACE (a + 1 AS A)"},
    {"COLUMNS('a')", "COLUMNS('b')"},
    {"COLUMNS(*)", "*"},
    // subqueries: kind, query, child, comparison, select list alias spelling
    {"(SELECT 1)", "(SELECT 2)"},
    {"(SELECT 1)", "EXISTS (SELECT 1)"},
    {"a IN (SELECT 1)", "b IN (SELECT 1)"},
    {"a = ANY (SELECT 1)", "a < ANY (SELECT 1)"},
    {"(SELECT 1 AS x)", "(SELECT 1 AS y)"},
    {"(SELECT 1 AS x)", "(SELECT 1 AS X)"},
    // an unaliased select entry is named by its printed text
    {"(SELECT 1 + 2)", "(SELECT \"+\"(1, 2))"},
    {"(SELECT s + 1)", "(SELECT S + 1)"},
    {"(SELECT 1::tinyint)", "(SELECT 1::TINYINT)"},
    // windows: function, arguments, partitions, orders, frame, exclusion, DISTINCT, FILTER, IGNORE NULLS
    {"sum(a) OVER ()", "max(a) OVER ()"},
    {"sum(a) OVER ()", "sum(b) OVER ()"},
    {"sum(a) OVER ()", "sum(a) OVER (PARTITION BY b)"},
    {"sum(a) OVER (ORDER BY b)", "sum(a) OVER (ORDER BY b DESC)"},
    {"sum(a) OVER (ORDER BY b)", "sum(a) OVER (ORDER BY b NULLS FIRST)"},
    {"sum(a) OVER (ORDER BY b ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)",
     "sum(a) OVER (ORDER BY b ROWS BETWEEN 2 PRECEDING AND CURRENT ROW)"},
    {"sum(a) OVER (ORDER BY b ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)",
     "sum(a) OVER (ORDER BY b RANGE BETWEEN 1 PRECEDING AND CURRENT ROW)"},
    {"sum(a) OVER (ORDER BY b ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)",
     "sum(a) OVER (ORDER BY b ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW EXCLUDE CURRENT ROW)"},
    {"sum(a) OVER ()", "sum(DISTINCT a) OVER ()"},
    {"sum(a) OVER ()", "sum(a) FILTER (WHERE b) OVER ()"},
    {"first(a) OVER ()", "first(a IGNORE NULLS) OVER ()"},
    {"lag(a, 1) OVER ()", "lag(a, 2) OVER ()"},
};

// Each pair binds to the same value: references resolve case insensitively and a top level alias names nothing inside.
static const vector<std::pair<string, string>> EQUAL_PAIRS {
    {"(SELECT s)", "(SELECT S)"},
    {"(SELECT t.*)", "(SELECT T.*)"},
    {"(SELECT * EXCLUDE (a))", "(SELECT * EXCLUDE (A))"},
    {"a", "A"},
    {"t.a", "T.a"},
    {"abs(a)", "ABS(a)"},
    {"CAST(a AS INTEGER)", "CAST(a AS integer)"},
    {"* EXCLUDE (a)", "* EXCLUDE (A)"},
    {"t.*", "T.*"},
    {"a AS x", "a AS y"},
};

TEST_CASE("Parsed expressions that differ in a field are not equal", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	for (auto &pair : DIFFERENT_PAIRS) {
		INFO(pair.first + "  against  " + pair.second);
		auto first = ParseExpression(con, pair.first);
		auto second = ParseExpression(con, pair.second);
		RequireEqualAndHashAlike(*first, *first->Copy());
		RequireEqualAndHashAlike(*second, *second->Copy());
		RequireNotEqual(*first, *second);
	}
	for (auto &pair : EQUAL_PAIRS) {
		INFO(pair.first + "  against  " + pair.second);
		RequireEqualAndHashAlike(*ParseExpression(con, pair.first), *ParseExpression(con, pair.second));
	}
}

TEST_CASE("Parsed expressions that differ in a field SQL cannot set alone are not equal", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	// an arrow lambda read from storage before v2.0.0 binds even when the arrow syntax is disabled
	auto lambda = ParseExpression(con, "list_transform(l, x -> x + 1)");
	auto stored = lambda->Copy();
	auto &stored_lambda = stored->Cast<FunctionExpression>().GetArgumentsMutable()[1].GetExpressionMutable();
	stored_lambda->Cast<LambdaExpression>().GetLambdaSyntaxTypeMutable() = LambdaSyntaxType::SINGLE_ARROW_STORAGE;
	RequireNotEqual(*lambda, *stored);

	auto quantified = [](bool excluded, bool reluctant, optional_idx min_count, optional_idx max_count) {
		return make_uniq<QuantifiedExpression>(ConstantExpression::Integer(1), min_count, max_count, excluded,
		                                       reluctant);
	};
	auto base = quantified(false, false, optional_idx(1), optional_idx());
	RequireEqualAndHashAlike(*base, *base->Copy());
	RequireNotEqual(*base, *quantified(true, false, optional_idx(1), optional_idx()));
	RequireNotEqual(*base, *quantified(false, true, optional_idx(1), optional_idx()));
	RequireNotEqual(*base, *quantified(false, false, optional_idx(0), optional_idx()));
	RequireNotEqual(*base, *quantified(false, false, optional_idx(1), optional_idx(2)));

	AnchorExpression start(false);
	RequireEqualAndHashAlike(start, *start.Copy());
	RequireNotEqual(start, AnchorExpression(true));

	auto concatenation = [](int64_t second) {
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(ConstantExpression::Integer(1));
		children.push_back(ConstantExpression::Integer(second));
		return make_uniq<ConcatenationExpression>(std::move(children));
	};
	RequireEqualAndHashAlike(*concatenation(2), *concatenation(2)->Copy());
	RequireNotEqual(*concatenation(2), *concatenation(3));

	auto alternation = [](int64_t left, int64_t right) {
		return make_uniq<AlternationExpression>(ConstantExpression::Integer(left), ConstantExpression::Integer(right));
	};
	RequireEqualAndHashAlike(*alternation(1, 2), *alternation(1, 2)->Copy());
	RequireNotEqual(*alternation(1, 2), *alternation(2, 2));
	RequireNotEqual(*alternation(1, 2), *alternation(1, 3));
}

static unique_ptr<ParsedExpression> Deserialized(const ParsedExpression &expression) {
	Allocator allocator;
	MemoryStream stream(allocator);
	SerializationOptions options;
	options.storage_compatibility = StorageCompatibility::FromString("latest");
	BinarySerializer::Serialize(expression, stream, options);
	stream.Rewind();
	return BinaryDeserializer::Deserialize<ParsedExpression>(stream);
}

TEST_CASE("Parsed expressions equal and hash alike their copy, reparsed text and deserialized form", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	// a star keeps its REPLACE, RENAME and EXCLUDE lists in hash containers, which a copy may reorder
	vector<string> expressions {"(SELECT row(*COLUMNS(* REPLACE (s AS a, 2 AS b, 3 AS c))))",
	                            "(SELECT (SELECT row(*COLUMNS(* REPLACE (s AS a, 2 AS b, 3 AS c)))))",
	                            "(SELECT (SELECT * EXCLUDE (a, b, c) RENAME (d AS x, e AS y, f AS z)))"};
	for (auto &pair : DIFFERENT_PAIRS) {
		expressions.push_back(pair.first);
		expressions.push_back(pair.second);
	}
	for (auto &sql : expressions) {
		INFO(sql);
		auto expression = ParseExpression(con, sql);
		RequireEqualAndHashAlike(*expression, *expression->Copy());
		RequireEqualAndHashAlike(*expression, *ParseExpression(con, expression->ToString()));
		RequireEqualAndHashAlike(*expression, *Deserialized(*expression));
	}
}
