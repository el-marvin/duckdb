// Arrow scans of string columns: a VARCHAR holds valid UTF-8, so a string value that is not valid UTF-8 is an error.

#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"
#include "duckdb/function/table/arrow.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace duckdb;

namespace arrow_string_utf8_test {

void ReleaseSchema(ArrowSchema *s) {
	s->release = nullptr;
}
void ReleaseArray(ArrowArray *a) {
	a->release = nullptr;
}

using rows = std::vector<string>;

// the lead byte of a three byte sequence with nothing after it
const string TRUNCATED = "ab\xE2";
// the name the scan gives the second of two columns named s
const string DUPLICATE_NAME = "s_1";

std::vector<uint8_t> ValidityBits(idx_t count, const std::vector<idx_t> &null_rows) {
	std::vector<uint8_t> validity((count + 7) / 8, 0xFF);
	for (auto row : null_rows) {
		validity[row / 8] &= uint8_t(~(1u << (row % 8)));
	}
	return validity;
}

// A string or binary column in any of the three layouts: "u" and "z" (32 bit offsets), "U" and "Z" (64 bit
// offsets), "vu" and "vz" (views over one data buffer).
struct StringColumn {
	StringColumn(const char *format, const std::vector<string> &values, const std::vector<idx_t> &null_rows = {},
	             int64_t offset = 0)
	    : validity(ValidityBits(values.size(), null_rows)) {
		for (auto &value : values) {
			data += value;
		}
		buffers.push_back(null_rows.empty() ? nullptr : validity.data());
		if (format[0] == 'v') {
			views.resize(values.size() * 2);
			int32_t data_offset = 0;
			for (idx_t row = 0; row < values.size(); row++) {
				char view[16] = {};
				auto length = int32_t(values[row].size());
				memcpy(view, &length, sizeof(length));
				if (length <= 12) {
					memcpy(view + 4, values[row].data(), values[row].size());
				} else {
					int32_t buffer_index = 0;
					memcpy(view + 4, values[row].data(), 4);
					memcpy(view + 8, &buffer_index, sizeof(buffer_index));
					memcpy(view + 12, &data_offset, sizeof(data_offset));
				}
				memcpy(&views[row * 2], view, sizeof(view));
				data_offset += length;
			}
			variadic_sizes.push_back(int64_t(data.size()));
			buffers.push_back(views.data());
			buffers.push_back(data.data());
			buffers.push_back(variadic_sizes.data());
		} else if (format[0] == 'U' || format[0] == 'Z') {
			offsets64.push_back(0);
			for (auto &value : values) {
				offsets64.push_back(offsets64.back() + int64_t(value.size()));
			}
			buffers.push_back(offsets64.data());
			buffers.push_back(data.data());
		} else {
			offsets32.push_back(0);
			for (auto &value : values) {
				offsets32.push_back(offsets32.back() + int32_t(value.size()));
			}
			buffers.push_back(offsets32.data());
			buffers.push_back(data.data());
		}
		schema.format = format;
		schema.name = "s";
		schema.flags = ARROW_FLAG_NULLABLE;
		schema.release = ReleaseSchema;
		array.length = int64_t(values.size()) - offset;
		array.offset = offset;
		array.null_count = null_rows.empty() ? 0 : -1;
		array.n_buffers = int64_t(buffers.size());
		array.buffers = buffers.data();
		array.release = ReleaseArray;
	}
	StringColumn(const StringColumn &) = delete;

	string data;
	std::vector<uint8_t> validity;
	std::vector<int32_t> offsets32;
	std::vector<int64_t> offsets64;
	std::vector<int64_t> views;
	std::vector<int64_t> variadic_sizes;
	std::vector<const void *> buffers;
	ArrowSchema schema {};
	ArrowArray array {};
};

struct IntColumn {
	explicit IntColumn(const std::vector<int32_t> &values_p) : values(values_p) {
		schema.format = "i";
		schema.name = "i";
		schema.flags = ARROW_FLAG_NULLABLE;
		schema.release = ReleaseSchema;
		buffers[0] = nullptr;
		buffers[1] = values.data();
		array.length = int64_t(values.size());
		array.n_buffers = 2;
		array.buffers = buffers;
		array.release = ReleaseArray;
	}
	IntColumn(const IntColumn &) = delete;

	std::vector<int32_t> values;
	const void *buffers[2];
	ArrowSchema schema {};
	ArrowArray array {};
};

struct DictionaryColumn {
	DictionaryColumn(const std::vector<string> &dictionary_values, const std::vector<int8_t> &indices_p,
	                 const std::vector<idx_t> &null_entries = {})
	    : values("u", dictionary_values, null_entries), indices(indices_p) {
		schema.format = "c";
		schema.name = "s";
		schema.flags = ARROW_FLAG_NULLABLE;
		schema.dictionary = &values.schema;
		schema.release = ReleaseSchema;
		buffers[0] = nullptr;
		buffers[1] = indices.data();
		array.length = int64_t(indices.size());
		array.n_buffers = 2;
		array.buffers = buffers;
		array.dictionary = &values.array;
		array.release = ReleaseArray;
	}
	DictionaryColumn(const DictionaryColumn &) = delete;

	StringColumn values;
	std::vector<int8_t> indices;
	const void *buffers[2];
	ArrowSchema schema {};
	ArrowArray array {};
};

struct RunEndColumn {
	RunEndColumn(const std::vector<int32_t> &run_ends_p, const std::vector<string> &run_values,
	             const std::vector<idx_t> &null_runs = {})
	    : run_ends(run_ends_p), values("u", run_values, null_runs) {
		run_ends_schema.format = "i";
		run_ends_schema.name = "run_ends";
		run_ends_schema.release = ReleaseSchema;
		values.schema.name = "values";
		schema_children[0] = &run_ends_schema;
		schema_children[1] = &values.schema;
		schema.format = "+r";
		schema.name = "s";
		schema.flags = ARROW_FLAG_NULLABLE;
		schema.n_children = 2;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		run_ends_buffers[0] = nullptr;
		run_ends_buffers[1] = run_ends.data();
		run_ends_array.length = int64_t(run_ends.size());
		run_ends_array.n_buffers = 2;
		run_ends_array.buffers = run_ends_buffers;
		run_ends_array.release = ReleaseArray;
		array_children[0] = &run_ends_array;
		array_children[1] = &values.array;
		array.length = run_ends.back();
		array.n_children = 2;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	RunEndColumn(const RunEndColumn &) = delete;

	std::vector<int32_t> run_ends;
	StringColumn values;
	const void *run_ends_buffers[2];
	ArrowSchema *schema_children[2];
	ArrowArray *array_children[2];
	ArrowSchema run_ends_schema {};
	ArrowSchema schema {};
	ArrowArray run_ends_array {};
	ArrowArray array {};
};

struct ListColumn {
	ListColumn(const std::vector<string> &child_values, const std::vector<int32_t> &offsets_p,
	           const std::vector<idx_t> &null_rows = {})
	    : child("u", child_values), offsets(offsets_p), validity(ValidityBits(offsets_p.size() - 1, null_rows)) {
		child.schema.name = "item";
		schema_children[0] = &child.schema;
		schema.format = "+l";
		schema.name = "s";
		schema.flags = ARROW_FLAG_NULLABLE;
		schema.n_children = 1;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		buffers[0] = null_rows.empty() ? nullptr : validity.data();
		buffers[1] = offsets.data();
		array_children[0] = &child.array;
		array.length = int64_t(offsets.size()) - 1;
		array.null_count = null_rows.empty() ? 0 : -1;
		array.n_buffers = 2;
		array.buffers = buffers;
		array.n_children = 1;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	ListColumn(const ListColumn &) = delete;

	StringColumn child;
	std::vector<int32_t> offsets;
	std::vector<uint8_t> validity;
	const void *buffers[2];
	ArrowSchema *schema_children[1];
	ArrowArray *array_children[1];
	ArrowSchema schema {};
	ArrowArray array {};
};

struct StructColumn {
	StructColumn(const std::vector<string> &child_values, const std::vector<idx_t> &null_rows)
	    : child("u", child_values), validity(ValidityBits(child_values.size(), null_rows)) {
		child.schema.name = "x";
		schema_children[0] = &child.schema;
		schema.format = "+s";
		schema.name = "s";
		schema.flags = ARROW_FLAG_NULLABLE;
		schema.n_children = 1;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		buffers[0] = validity.data();
		array_children[0] = &child.array;
		array.length = int64_t(child_values.size());
		array.null_count = -1;
		array.n_buffers = 1;
		array.buffers = buffers;
		array.n_children = 1;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	StructColumn(const StructColumn &) = delete;

	StringColumn child;
	std::vector<uint8_t> validity;
	const void *buffers[1];
	ArrowSchema *schema_children[1];
	ArrowArray *array_children[1];
	ArrowSchema schema {};
	ArrowArray array {};
};

struct FixedSizeListColumn {
	FixedSizeListColumn(const std::vector<string> &child_values, const std::vector<idx_t> &null_rows)
	    : child("u", child_values), validity(ValidityBits(child_values.size() / 2, null_rows)) {
		child.schema.name = "item";
		schema_children[0] = &child.schema;
		schema.format = "+w:2";
		schema.name = "s";
		schema.flags = ARROW_FLAG_NULLABLE;
		schema.n_children = 1;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		buffers[0] = validity.data();
		array_children[0] = &child.array;
		array.length = int64_t(child_values.size() / 2);
		array.null_count = -1;
		array.n_buffers = 1;
		array.buffers = buffers;
		array.n_children = 1;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	FixedSizeListColumn(const FixedSizeListColumn &) = delete;

	StringColumn child;
	std::vector<uint8_t> validity;
	const void *buffers[1];
	ArrowSchema *schema_children[1];
	ArrowArray *array_children[1];
	ArrowSchema schema {};
	ArrowArray array {};
};

// Sparse UNION(x VARCHAR, i INTEGER) where slot j selects member type_ids[j].
struct SparseUnionColumn {
	SparseUnionColumn(const std::vector<int8_t> &type_ids_p, const std::vector<string> &string_values,
	                  const std::vector<int32_t> &int_values)
	    : type_ids(type_ids_p), strings("u", string_values), ints(int_values) {
		strings.schema.name = "x";
		schema_children[0] = &strings.schema;
		schema_children[1] = &ints.schema;
		schema.format = "+us:0,1";
		schema.name = "s";
		schema.n_children = 2;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		buffers[0] = type_ids.data();
		array_children[0] = &strings.array;
		array_children[1] = &ints.array;
		array.length = int64_t(type_ids.size());
		array.n_buffers = 1;
		array.buffers = buffers;
		array.n_children = 2;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	SparseUnionColumn(const SparseUnionColumn &) = delete;

	std::vector<int8_t> type_ids;
	StringColumn strings;
	IntColumn ints;
	const void *buffers[1];
	ArrowSchema *schema_children[2];
	ArrowArray *array_children[2];
	ArrowSchema schema {};
	ArrowArray array {};
};

// The C data interface metadata of one key: an int32 count, then the length and the bytes of key and value.
string ExtensionMetadata(const string &extension_name) {
	const string key = "ARROW:extension:name";
	string metadata;
	auto append_int = [&](int32_t value) {
		metadata.append(reinterpret_cast<const char *>(&value), sizeof(value));
	};
	append_int(1);
	append_int(int32_t(key.size()));
	metadata += key;
	append_int(int32_t(extension_name.size()));
	metadata += extension_name;
	return metadata;
}

unique_ptr<QueryResult> ScanColumns(Connection &con, std::vector<ArrowSchema *> schema_children,
                                    std::vector<ArrowArray *> array_children, const string &projection) {
	ArrowSchema record_schema {};
	record_schema.format = "+s";
	record_schema.n_children = int64_t(schema_children.size());
	record_schema.children = schema_children.data();
	record_schema.release = ReleaseSchema;

	const void *buffers[1] = {nullptr};
	ArrowArray record_array {};
	record_array.length = array_children[0]->length;
	record_array.n_buffers = 1;
	record_array.buffers = buffers;
	record_array.n_children = int64_t(array_children.size());
	record_array.children = array_children.data();
	record_array.release = ReleaseArray;

	ArrowArrayStream stream {};
	AdbcError err {};
	REQUIRE(duckdb_adbc::BatchToArrayStream(&record_array, &record_schema, &stream, &err) == ADBC_STATUS_OK);
	auto result = con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(stream))
	                  ->Project(projection)
	                  ->Execute();
	stream.release = nullptr;
	return result;
}

unique_ptr<QueryResult> Scan(Connection &con, ArrowSchema &col_schema, ArrowArray &col_array,
                             const string &projection) {
	return ScanColumns(con, {&col_schema}, {&col_array}, projection);
}

// Hands arrow_scan a batch of only the projected columns, as arrow_scan expects of its producer.
class ProjectingScanFactory : public ArrowScanFactory {
public:
	ProjectingScanFactory(std::vector<ArrowSchema *> schemas_p, std::vector<ArrowArray *> arrays_p)
	    : schemas(std::move(schemas_p)), arrays(std::move(arrays_p)) {
	}

	void GetSchema(ArrowSchema &schema) override {
		schema = ArrowSchema {};
		schema.format = "+s";
		schema.n_children = int64_t(schemas.size());
		schema.children = schemas.data();
		schema.release = ReleaseSchema;
	}

	unique_ptr<ArrowArrayStreamWrapper> ProduceStream(ArrowStreamParameters &parameters) override {
		projected_schemas.clear();
		projected_arrays.clear();
		for (idx_t idx = 0; idx < parameters.projected_columns.columns.size(); idx++) {
			auto col_idx = parameters.projected_columns.filter_to_col.at(idx);
			projected_schemas.push_back(schemas[col_idx]);
			projected_arrays.push_back(arrays[col_idx]);
		}
		ArrowSchema record_schema {};
		record_schema.format = "+s";
		record_schema.n_children = int64_t(projected_schemas.size());
		record_schema.children = projected_schemas.data();
		record_schema.release = ReleaseSchema;
		ArrowArray record_array {};
		record_array.length = arrays[0]->length;
		record_array.n_buffers = 1;
		record_array.buffers = record_buffers;
		record_array.n_children = int64_t(projected_arrays.size());
		record_array.children = projected_arrays.data();
		record_array.release = ReleaseArray;
		auto result = make_uniq<ArrowArrayStreamWrapper>();
		AdbcError err {};
		REQUIRE(duckdb_adbc::BatchToArrayStream(&record_array, &record_schema, &result->arrow_array_stream, &err) ==
		        ADBC_STATUS_OK);
		return result;
	}

private:
	std::vector<ArrowSchema *> schemas;
	std::vector<ArrowArray *> arrays;
	std::vector<ArrowSchema *> projected_schemas;
	std::vector<ArrowArray *> projected_arrays;
	const void *record_buffers[1] = {nullptr};
};

unique_ptr<QueryResult> ScanProjected(Connection &con, std::vector<ArrowSchema *> schemas,
                                      std::vector<ArrowArray *> arrays, const string &projection) {
	auto factory = make_shared_ptr<ProjectingScanFactory>(std::move(schemas), std::move(arrays));
	return con.TableFunction("arrow_scan", {}, {}, factory)->Project(projection)->Execute();
}

rows Text(QueryResult &result) {
	rows text;
	for (idx_t row = 0; row < result.RowCount(); row++) {
		text.push_back(result.Collection().GetValue(0, row).ToString());
	}
	return text;
}

void RequireInvalidResult(QueryResult &result, const string &value, const string &column = "s") {
	REQUIRE(result.HasError());
	REQUIRE(result.GetErrorType() == ExceptionType::CONVERSION);
	REQUIRE_THAT(result.GetError(),
	             Catch::Matchers::Contains("Invalid string encoding found in Arrow column \"" + column +
	                                       "\": value \"" + value + "\" is not valid UTF8"));
}

template <class COLUMN>
rows ScanText(COLUMN &col, const string &projection = "s") {
	DuckDB db(nullptr);
	Connection con(db);
	auto result = Scan(con, col.schema, col.array, projection);
	REQUIRE_NO_FAIL(*result);
	return Text(*result);
}

template <class COLUMN>
void RequireInvalid(COLUMN &col, const string &value, const string &projection = "s") {
	DuckDB db(nullptr);
	Connection con(db);
	auto result = Scan(con, col.schema, col.array, projection);
	RequireInvalidResult(*result, value);
}

// upper() decodes code points, so it must never see the value
TEST_CASE("Arrow scan of a string ending in a truncated sequence", "[arrow]") {
	StringColumn col("u", {TRUNCATED});
	RequireInvalid(col, "ab\\xE2");
	RequireInvalid(col, "ab\\xE2", "upper(s)");
	RequireInvalid(col, "ab\\xE2", "length(s)");
}

TEST_CASE("Arrow scan of valid UTF-8 strings", "[arrow]") {
	const rows values = {"",
	                     "abc",
	                     "\xC3\xA9t\xC3\xA9",
	                     "\xE2\x82\xAC",
	                     "\xF0\x9F\xA6\x86",
	                     "a string view value longer than twelve bytes \xC3\xA9",
	                     string(70, 'x') + "\xE2\x82\xAC" + string(70, 'y')};
	for (auto format : {"u", "U", "vu"}) {
		INFO(format);
		StringColumn col(format, values);
		REQUIRE(ScanText(col) == values);
	}
}

TEST_CASE("Arrow scan of invalid UTF-8 in every string layout", "[arrow]") {
	struct InvalidValue {
		string bytes;
		string rendered;
	};
	const std::vector<InvalidValue> invalid = {
	    {"\x80", "\\x80"},                                     // continuation byte without a lead byte
	    {"\xC3", "\\xC3"},                                     // sequence cut short at the end of the value
	    {"\xC3(", "\\xC3("},                                   // lead byte followed by ASCII
	    {"\xC0\x80", "\\xC0\\x80"},                            // overlong form of U+0000
	    {"\xED\xA0\x80", "\\xED\\xA0\\x80"},                   // UTF-16 surrogate U+D800
	    {"\xF4\x90\x80\x80", "\\xF4\\x90\\x80\\x80"},          // above U+10FFFF
	    {"\xFF", "\\xFF"},                                     // never a UTF-8 byte
	    {"abcdefgh\x80", "abcdefgh\\x80"},                     // inlined, the bad byte in the last four bytes
	    {"abcdefghij\xE2", "abcdefghij\\xE2"},                 // inlined, eleven bytes
	    {"abcdefghijk\xE2", "abcdefghijk\\xE2"},               // inlined, twelve bytes
	    {string(64, 'a') + "\xE2", string(64, 'a') + "\\xE2"}, // past the 32 byte ASCII fast path
	    {"thirteen byte\xE2", "thirteen byte\\xE2"}};          // a string view value that is not inlined
	for (auto format : {"u", "U", "vu"}) {
		for (auto &value : invalid) {
			INFO(format << " " << value.rendered);
			StringColumn col(format, {"ok", value.bytes, "ok"});
			RequireInvalid(col, value.rendered);
		}
	}
}

TEST_CASE("Arrow scan of invalid UTF-8 behind a NULL, an offset or past the first chunk", "[arrow]") {
	for (auto format : {"u", "U", "vu"}) {
		INFO(format);
		StringColumn null_slot(format, {"a", TRUNCATED, "c"}, {1});
		REQUIRE(ScanText(null_slot) == rows({"a", "NULL", "c"}));
		StringColumn offset_skips(format, {TRUNCATED, "b", "c"}, {}, 1);
		REQUIRE(ScanText(offset_skips) == rows({"b", "c"}));
		StringColumn offset_reads(format, {"a", "b", TRUNCATED}, {}, 1);
		RequireInvalid(offset_reads, "ab\\xE2");
	}
	std::vector<string> many(3000, "v");
	many[2500] = TRUNCATED;
	StringColumn late("u", many);
	RequireInvalid(late, "ab\\xE2");
	StringColumn late_null("u", many, {2500});
	auto text = ScanText(late_null);
	REQUIRE(text.size() == 3000);
	REQUIRE(text[2499] == "v");
	REQUIRE(text[2500] == "NULL");
}

// A dictionary is converted as a whole, so an entry no index points at is checked too.
TEST_CASE("Arrow scan of dictionary encoded invalid UTF-8", "[arrow]") {
	DictionaryColumn valid({"a", "\xC3\xA9"}, {1, 0, 1});
	REQUIRE(ScanText(valid) == rows({"\xC3\xA9", "a", "\xC3\xA9"}));
	DictionaryColumn referenced({"a", TRUNCATED}, {0, 1});
	RequireInvalid(referenced, "ab\\xE2");
	DictionaryColumn unreferenced({"a", TRUNCATED}, {0, 0});
	RequireInvalid(unreferenced, "ab\\xE2");
	DictionaryColumn null_entry({"a", TRUNCATED}, {0, 1}, {1});
	REQUIRE(ScanText(null_entry) == rows({"a", "NULL"}));
}

TEST_CASE("Arrow scan of run-end encoded invalid UTF-8", "[arrow]") {
	RunEndColumn valid({2, 3}, {"\xC3\xA9", "b"});
	REQUIRE(ScanText(valid) == rows({"\xC3\xA9", "\xC3\xA9", "b"}));
	RunEndColumn invalid({2, 3}, {"a", TRUNCATED});
	RequireInvalid(invalid, "ab\\xE2");
	RunEndColumn null_run({2, 3}, {"a", TRUNCATED}, {1});
	REQUIRE(ScanText(null_run) == rows({"a", "a", "NULL"}));
}

// Every child slot is converted, as for the Arrow TIME range check, so the segment under a NULL list entry and a
// union slot no row selects are checked too; the error names the top level column.
TEST_CASE("Arrow scan of invalid UTF-8 nested in lists, structs and unions", "[arrow]") {
	ListColumn list_valid({"a", "\xC3\xA9", "c"}, {0, 2, 3});
	REQUIRE(ScanText(list_valid) == rows({"[a, \xC3\xA9]", "[c]"}));
	ListColumn list_invalid({"a", TRUNCATED, "c"}, {0, 2, 3});
	RequireInvalid(list_invalid, "ab\\xE2");
	ListColumn list_null_entry({"a", TRUNCATED, "c"}, {0, 2, 3}, {0});
	RequireInvalid(list_null_entry, "ab\\xE2");

	StructColumn struct_invalid({"a", TRUNCATED}, {});
	RequireInvalid(struct_invalid, "ab\\xE2");
	StructColumn struct_null_row({"a", TRUNCATED}, {1});
	REQUIRE(ScanText(struct_null_row) == rows({"{'x': a}", "NULL"}));

	SparseUnionColumn union_selected({0, 0}, {"a", TRUNCATED}, {1, 2});
	RequireInvalid(union_selected, "ab\\xE2");
	SparseUnionColumn union_unselected({0, 1}, {"a", TRUNCATED}, {1, 2});
	RequireInvalid(union_unselected, "ab\\xE2");
	SparseUnionColumn union_valid({0, 1}, {"\xC3\xA9", "b"}, {1, 2});
	REQUIRE(ScanText(union_valid, "s::VARCHAR") == rows({"\xC3\xA9", "2"}));
}

// A conversion that marks the child slots under a NULL parent row NULL first (struct, fixed size list) never reads
// them; a list's child range is addressed by offsets, so its segment under a NULL entry is converted and checked.
TEST_CASE("Arrow scan of invalid UTF-8 in a fixed size list", "[arrow]") {
	FixedSizeListColumn valid({"a", "b", "\xC3\xA9", "d"}, {});
	REQUIRE(ScanText(valid) == rows({"[a, b]", "[\xC3\xA9, d]"}));
	FixedSizeListColumn invalid({"a", "b", TRUNCATED, "d"}, {});
	RequireInvalid(invalid, "ab\\xE2");
	FixedSizeListColumn null_row({"a", "b", TRUNCATED, "d"}, {1});
	REQUIRE(ScanText(null_row) == rows({"[a, b]", "NULL"}));
}

// The error names the column by its name in the scan, whatever its position in the projection.
TEST_CASE("Arrow scan of invalid UTF-8 names the column", "[arrow]") {
	IntColumn ints({1, 2});
	StringColumn strings("u", {"ok", TRUNCATED});
	for (auto projection : {"s", "s, i", "i, upper(s)"}) {
		INFO(projection);
		DuckDB db(nullptr);
		Connection con(db);
		auto result = ScanProjected(con, {&ints.schema, &strings.schema}, {&ints.array, &strings.array}, projection);
		RequireInvalidResult(*result, "ab\\xE2");
	}
	{
		DuckDB db(nullptr);
		Connection con(db);
		auto result = ScanProjected(con, {&ints.schema, &strings.schema}, {&ints.array, &strings.array}, "i");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(Text(*result) == rows({"1", "2"}));
	}
	StringColumn valid("u", {"ok", "ok"});
	{
		DuckDB db(nullptr);
		Connection con(db);
		auto result = ScanProjected(con, {&valid.schema, &strings.schema}, {&valid.array, &strings.array}, "*");
		RequireInvalidResult(*result, "ab\\xE2", DUPLICATE_NAME);
	}
}

// A reader that builds its own scan state names no column, so the message leaves the column out.
TEST_CASE("Arrow conversion of invalid UTF-8 without a column name", "[arrow]") {
	DuckDB db(nullptr);
	Connection con(db);
	StringColumn col("u", {"ok", TRUNCATED});
	auto arrow_type = ArrowType::GetArrowLogicalType(*con.context, col.schema);
	ArrowArrayScanState state(*con.context);
	Vector result(LogicalType::VARCHAR, 2);
	ArrowToDuckDBConversion::SetValidityMask(result, col.array, 0, 2, 0, -1);
	string message;
	try {
		ArrowToDuckDBConversion::ColumnArrowToDuckDB(result, col.array, 0, state, 2, *arrow_type);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		REQUIRE(error.Type() == ExceptionType::CONVERSION);
		message = error.RawMessage();
	}
	REQUIRE(message == "Invalid string encoding found in Arrow array: value \"ab\\xE2\" is not valid UTF8");
}

TEST_CASE("Arrow scan of invalid bytes in binary and JSON columns", "[arrow]") {
	for (auto format : {"z", "Z", "vz"}) {
		INFO(format);
		StringColumn binary(format, {TRUNCATED, "thirteen byte\xE2"});
		REQUIRE(ScanText(binary) == rows({"ab\\xE2", "thirteen byte\\xE2"}));
	}
	auto metadata = ExtensionMetadata("arrow.json");
	StringColumn json_valid("u", {"{\"a\": \"\xC3\xA9\"}"});
	json_valid.schema.metadata = metadata.c_str();
	REQUIRE(ScanText(json_valid) == rows({"{\"a\": \"\xC3\xA9\"}"}));
	StringColumn json_invalid("u", {"\"" + TRUNCATED + "\""});
	json_invalid.schema.metadata = metadata.c_str();
	// the rendering escapes a quote as \x22
	RequireInvalid(json_invalid, "\\x22ab\\xE2\\x22");
}

} // namespace arrow_string_utf8_test
