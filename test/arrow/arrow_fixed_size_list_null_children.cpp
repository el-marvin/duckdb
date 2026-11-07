// Arrow scans of a NULL FIXED_SIZE_LIST row: every child slot of the row reads NULL, whatever the child's kind.

#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb.h"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/main/config.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace duckdb;

namespace arrow_fixed_size_list_null_children_test {

void ReleaseSchema(ArrowSchema *s) {
	s->release = nullptr;
}
void ReleaseArray(ArrowArray *a) {
	a->release = nullptr;
}

// A hand built Arrow field and its array, owning its children, its dictionary and its buffers.
struct Field {
	string format;
	ArrowSchema schema {};
	ArrowArray array {};
	std::vector<std::unique_ptr<Field>> children;
	std::unique_ptr<Field> dictionary;
	std::vector<int32_t> data;
	std::vector<uint8_t> bytes;
	std::vector<uint8_t> validity;
	std::vector<ArrowSchema *> schema_children;
	std::vector<ArrowArray *> array_children;
	std::vector<const void *> buffers;
};
using FieldPtr = std::unique_ptr<Field>;

FieldPtr MakeField(const string &format, const char *name, int64_t length, std::vector<FieldPtr> children) {
	auto field = std::unique_ptr<Field>(new Field());
	field->format = format;
	field->schema.format = field->format.c_str();
	field->schema.name = name;
	field->schema.flags = ARROW_FLAG_NULLABLE;
	field->schema.release = ReleaseSchema;
	field->array.length = length;
	field->array.release = ReleaseArray;
	field->children = std::move(children);
	for (auto &child : field->children) {
		field->schema_children.push_back(&child->schema);
		field->array_children.push_back(&child->array);
	}
	field->schema.n_children = int64_t(field->children.size());
	field->schema.children = field->schema_children.empty() ? nullptr : field->schema_children.data();
	field->array.n_children = int64_t(field->children.size());
	field->array.children = field->array_children.empty() ? nullptr : field->array_children.data();
	return field;
}

void SetBuffers(Field &field, std::vector<const void *> buffers) {
	field.buffers = std::move(buffers);
	field.array.n_buffers = int64_t(field.buffers.size());
	field.array.buffers = field.buffers.data();
}

// Clears the validity bits of the given physical slots; a field without null slots keeps no bitmap.
void SetNulls(Field &field, idx_t slots, const std::vector<idx_t> &null_slots) {
	if (null_slots.empty()) {
		return;
	}
	field.validity.assign((slots + 7) / 8, 0xFF);
	for (auto slot : null_slots) {
		field.validity[slot / 8] &= uint8_t(~(1u << (slot % 8)));
	}
	field.buffers[0] = field.validity.data();
	field.array.null_count = int64_t(null_slots.size());
}

FieldPtr Int32(const char *name, std::vector<int32_t> values, const std::vector<idx_t> &null_slots = {}) {
	auto field = MakeField("i", name, int64_t(values.size()), {});
	field->data = std::move(values);
	SetBuffers(*field, {nullptr, field->data.data()});
	SetNulls(*field, field->data.size(), null_slots);
	return field;
}

FieldPtr Utf8(const char *name, const std::vector<string> &values) {
	auto field = MakeField("u", name, int64_t(values.size()), {});
	field->data.push_back(0);
	for (auto &value : values) {
		field->bytes.insert(field->bytes.end(), value.begin(), value.end());
		field->data.push_back(int32_t(field->bytes.size()));
	}
	SetBuffers(*field, {nullptr, field->data.data(), field->bytes.data()});
	return field;
}

// Two byte values, the first byte of each is given.
FieldPtr FixedBinary(const char *name, const std::vector<uint8_t> &first, const std::vector<idx_t> &null_slots = {}) {
	auto field = MakeField("w:2", name, int64_t(first.size()), {});
	for (auto byte : first) {
		field->bytes.push_back(byte);
		field->bytes.push_back('!');
	}
	SetBuffers(*field, {nullptr, field->bytes.data()});
	SetNulls(*field, first.size(), null_slots);
	return field;
}

FieldPtr Struct(const char *name, std::vector<FieldPtr> children, const std::vector<idx_t> &null_slots = {}) {
	auto slots = children[0]->array.length;
	auto field = MakeField("+s", name, slots, std::move(children));
	SetBuffers(*field, {nullptr});
	SetNulls(*field, idx_t(slots), null_slots);
	return field;
}

FieldPtr List(const char *name, std::vector<int32_t> offsets, FieldPtr child,
              const std::vector<idx_t> &null_slots = {}) {
	auto slots = offsets.size() - 1;
	std::vector<FieldPtr> children;
	children.push_back(std::move(child));
	auto field = MakeField("+l", name, int64_t(slots), std::move(children));
	field->data = std::move(offsets);
	SetBuffers(*field, {nullptr, field->data.data()});
	SetNulls(*field, slots, null_slots);
	return field;
}

// A FIXED_SIZE_LIST of the given size over every slot of its child.
FieldPtr FixedSizeList(const char *name, int64_t size, FieldPtr child, const std::vector<idx_t> &null_slots = {}) {
	auto slots = child->array.length / size;
	std::vector<FieldPtr> children;
	children.push_back(std::move(child));
	auto field = MakeField("+w:" + to_string(size), name, slots, std::move(children));
	SetBuffers(*field, {nullptr});
	SetNulls(*field, idx_t(slots), null_slots);
	return field;
}

FieldPtr Dictionary(const char *name, std::vector<int32_t> indices, FieldPtr values) {
	auto field = Int32(name, std::move(indices));
	field->dictionary = std::move(values);
	field->schema.dictionary = &field->dictionary->schema;
	field->array.dictionary = &field->dictionary->array;
	return field;
}

FieldPtr SparseUnion(const char *name, std::vector<int8_t> type_ids, std::vector<FieldPtr> members) {
	auto field = MakeField("+us:0,1", name, int64_t(type_ids.size()), std::move(members));
	for (auto id : type_ids) {
		field->bytes.push_back(uint8_t(id));
	}
	SetBuffers(*field, {field->bytes.data()});
	return field;
}

FieldPtr RunEnd(const char *name, FieldPtr run_ends, FieldPtr values) {
	run_ends->schema.name = "run_ends";
	values->schema.name = "values";
	auto length = int64_t(run_ends->data.back());
	std::vector<FieldPtr> children;
	children.push_back(std::move(run_ends));
	children.push_back(std::move(values));
	auto field = MakeField("+r", name, length, std::move(children));
	SetBuffers(*field, {});
	return field;
}

// Starts the field at the given slot of its buffers.
FieldPtr Sliced(FieldPtr field, int64_t offset) {
	field->array.offset = offset;
	field->array.length -= offset;
	return field;
}

FieldPtr Record(std::vector<FieldPtr> columns) {
	return Struct("", std::move(columns));
}

string RenderValue(const Value &value) {
	if (value.IsNull()) {
		return "NULL";
	}
	auto &type = value.type();
	if (type.id() == LogicalTypeId::UNION) {
		return to_string(UnionValue::GetTag(value)) + ":" + RenderValue(UnionValue::GetValue(value));
	}
	if (StructType::IsStruct(type)) {
		string text;
		for (auto &child : StructValue::GetChildren(value)) {
			text += (text.empty() ? "" : ", ") + RenderValue(child);
		}
		return "(" + text + ")";
	}
	if (type.id() == LogicalTypeId::LIST || type.id() == LogicalTypeId::ARRAY) {
		auto &children =
		    type.id() == LogicalTypeId::LIST ? ListValue::GetChildren(value) : ArrayValue::GetChildren(value);
		string text;
		for (auto &child : children) {
			text += (text.empty() ? "" : ", ") + RenderValue(child);
		}
		return "[" + text + "]";
	}
	return value.ToString();
}

// Requires that every child slot of a NULL array row and every child of a NULL struct row is NULL, at every level.
void RequireNullChildrenNull(Vector &vector, idx_t count) {
	auto &type = vector.GetType();
	vector.Flatten();
	if (type.id() == LogicalTypeId::LIST || type.id() == LogicalTypeId::MAP) {
		Vector child(ListType::GetChildType(type));
		child.Reference(ListVector::GetChild(vector));
		RequireNullChildrenNull(child, ListVector::GetListSize(vector));
		return;
	}
	if (type.id() == LogicalTypeId::ARRAY) {
		auto array_size = ArrayType::GetSize(type);
		Vector child(ArrayType::GetChildType(type));
		child.Reference(ArrayVector::GetChild(vector));
		child.Flatten();
		for (idx_t row = 0; row < count; row++) {
			if (!FlatVector::IsNull(vector, row)) {
				continue;
			}
			for (idx_t slot = row * array_size; slot < (row + 1) * array_size; slot++) {
				INFO("slot " << slot << " of the " << child.GetType().ToString() << " child of NULL array row " << row);
				REQUIRE(FlatVector::IsNull(child, slot));
			}
		}
		RequireNullChildrenNull(child, count * array_size);
		return;
	}
	if (type.InternalType() != PhysicalType::STRUCT) {
		return;
	}
	for (auto &entry : StructVector::GetEntries(vector)) {
		Vector child(entry.GetType());
		child.Reference(entry);
		child.Flatten();
		for (idx_t row = 0; row < count; row++) {
			if (FlatVector::IsNull(vector, row)) {
				INFO("row " << row << " of the " << child.GetType().ToString() << " child of a NULL struct");
				REQUIRE(FlatVector::IsNull(child, row));
			}
		}
		RequireNullChildrenNull(child, count);
	}
}

// The rows are rendered before the checks, which flatten the vectors they walk.
void RenderChunk(DataChunk &chunk, std::vector<string> &rows) {
	chunk.Verify();
	for (idx_t row = 0; row < chunk.size(); row++) {
		string text;
		for (idx_t column = 0; column < chunk.ColumnCount(); column++) {
			text += (column ? "\t" : "") + RenderValue(chunk.GetValue(column, row));
		}
		rows.push_back(text);
	}
	for (auto &column : chunk.data) {
		Vector copy(column.GetType());
		copy.Reference(column);
		RequireNullChildrenNull(copy, chunk.size());
	}
}

// debug_verification_mode is process wide, so it is restored for the tests that run later.
struct VerifyVectors {
	VerifyVectors() {
		DBConfigOptions::global_verification_mode = DebugVerificationMode::VERIFY_VECTORS;
	}
	~VerifyVectors() {
		DBConfigOptions::global_verification_mode = previous;
	}
	DebugVerificationMode previous = DBConfigOptions::global_verification_mode;
};

// Each reader returns the rows it reads, or a single line with its error.
std::vector<string> ScanArrowScan(Field &record) {
	auto schema = record.schema;
	auto array = record.array;
	ArrowArrayStream stream {};
	AdbcError error {};
	REQUIRE(duckdb_adbc::BatchToArrayStream(&array, &schema, &stream, &error) == ADBC_STATUS_OK);
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET memory_limit = '4GB'"));
	auto result = con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(stream))->Execute();
	if (result->HasError()) {
		return {"error: " + result->GetError()};
	}
	std::vector<string> rows;
	for (auto chunk = result->Fetch(); chunk; chunk = result->Fetch()) {
		RenderChunk(*chunk, rows);
	}
	if (result->HasError()) {
		return {"error: " + result->GetError()};
	}
	return rows;
}

struct CDatabase {
	CDatabase() {
		REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
		REQUIRE(duckdb_connect(db, &con) == DuckDBSuccess);
		REQUIRE(duckdb_query(con, "SET memory_limit = '4GB'", nullptr) == DuckDBSuccess);
	}
	~CDatabase() {
		duckdb_disconnect(&con);
		duckdb_close(&db);
	}
	duckdb_database db = nullptr;
	duckdb_connection con = nullptr;
};

std::vector<string> ScanCApiArrayScan(Field &record) {
	CDatabase c;
	auto schema = record.schema;
	auto array = record.array;
	duckdb_arrow_stream stream = nullptr;
	REQUIRE(duckdb_arrow_array_scan(c.con, "batch", reinterpret_cast<duckdb_arrow_schema>(&schema),
	                                reinterpret_cast<duckdb_arrow_array>(&array), &stream) == DuckDBSuccess);
	duckdb_result result;
	std::vector<string> rows;
	if (duckdb_query(c.con, "SELECT * FROM batch", &result) != DuckDBSuccess) {
		rows.push_back(string("error: ") + duckdb_result_error(&result));
	} else {
		while (auto chunk = duckdb_fetch_chunk(result)) {
			RenderChunk(*reinterpret_cast<DataChunk *>(chunk), rows);
			duckdb_destroy_data_chunk(&chunk);
		}
	}
	duckdb_destroy_result(&result);
	duckdb_destroy_arrow_stream(&stream);
	return rows;
}

std::vector<string> ScanCApiDataChunk(Field &record) {
	CDatabase c;
	duckdb_arrow_converted_schema converted = nullptr;
	auto schema = record.schema;
	REQUIRE(duckdb_schema_from_arrow(c.con, &schema, &converted) == nullptr);
	auto array = record.array;
	duckdb_data_chunk chunk = nullptr;
	std::vector<string> rows;
	auto error = duckdb_data_chunk_from_arrow(c.con, &array, converted, &chunk);
	if (error) {
		rows.push_back(string("error: ") + duckdb_error_data_message(error));
		duckdb_destroy_error_data(&error);
	} else {
		RenderChunk(*reinterpret_cast<DataChunk *>(chunk), rows);
		duckdb_destroy_data_chunk(&chunk);
	}
	duckdb_destroy_arrow_converted_schema(&converted);
	return rows;
}

void RequireRows(Field &record, const std::vector<string> &expected) {
	VerifyVectors verify_vectors;
	SECTION("arrow_scan") {
		REQUIRE(ScanArrowScan(record) == expected);
	}
	SECTION("duckdb_arrow_array_scan") {
		REQUIRE(ScanCApiArrayScan(record) == expected);
	}
	SECTION("duckdb_data_chunk_from_arrow") {
		REQUIRE(ScanCApiDataChunk(record) == expected);
	}
}

// Element kinds over eight physical slots; slot k holds k + 1, the letter k of the alphabet, or a value made of them.
// The int32 and fixed size binary elements are NULL at slot 7, so their conversions read their own bitmap.
FieldPtr IntElements() {
	return Int32("item", {1, 2, 3, 4, 5, 6, 7, 8}, {7});
}
FieldPtr Utf8Elements() {
	return Utf8("item", {"a", "b", "c", "d", "e", "f", "g", "h"});
}
FieldPtr FixedBinaryElements() {
	return FixedBinary("item", {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'}, {7});
}
FieldPtr DictionaryElements() {
	return Dictionary("item", {0, 1, 0, 1, 0, 1, 0, 1}, Int32("", {100, 200}));
}
FieldPtr StructElements() {
	std::vector<FieldPtr> fields;
	fields.push_back(Int32("x", {10, 20, 30, 40, 50, 60, 70, 80}));
	fields.push_back(Utf8("y", {"a", "b", "c", "d", "e", "f", "g", "h"}));
	fields.push_back(DictionaryElements());
	return Struct("item", std::move(fields));
}
FieldPtr ListElements() {
	return List("item", {0, 1, 2, 3, 4, 5, 6, 7, 8}, Int32("item", {1, 2, 3, 4, 5, 6, 7, 8}));
}
FieldPtr RunEndElements() {
	// a NULL value makes the decoding write the validity of every slot
	return RunEnd("item", Int32("", {2, 4, 6, 8}), Int32("", {7, 8, 9, 10}, {3}));
}
FieldPtr UnionElements() {
	std::vector<FieldPtr> members;
	members.push_back(Int32("i", {1, 2, 3, 4, 5, 6, 7, 8}));
	members.push_back(Int32("j", {10, 20, 30, 40, 50, 60, 70, 80}));
	return SparseUnion("item", {0, 1, 0, 1, 0, 1, 0, 1}, std::move(members));
}
FieldPtr ArrayElements() {
	return FixedSizeList("item", 2, Int32("item", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}));
}

// One FIXED_SIZE_LIST<2> column per element kind, the elements started at child_offset, NULL at the given slots.
FieldPtr AllKinds(int64_t child_offset, const std::vector<idx_t> &null_slots) {
	std::vector<FieldPtr> columns;
	auto add = [&](const char *name, FieldPtr elements) {
		columns.push_back(FixedSizeList(name, 2, Sliced(std::move(elements), child_offset), null_slots));
	};
	add("i", IntElements());
	add("s", Utf8Elements());
	add("w", FixedBinaryElements());
	add("t", StructElements());
	add("l", ListElements());
	add("d", DictionaryElements());
	add("r", RunEndElements());
	add("u", UnionElements());
	add("a", ArrayElements());
	return Record(std::move(columns));
}

// The rows of AllKinds over the element slots 2 * row and 2 * row + 1, when the row is not NULL.
const std::vector<string> ALL_KINDS_ROWS = {
    "[1, 2]\t[a, b]\t[a!, b!]\t[(10, a, 100), (20, b, 200)]\t[[1], [2]]\t[100, 200]\t[7, 7]\t[0:1, 1:20]\t[[1, 2], [3, "
    "4]]",
    "[3, 4]\t[c, d]\t[c!, d!]\t[(30, c, 100), (40, d, 200)]\t[[3], [4]]\t[100, 200]\t[8, 8]\t[0:3, 1:40]\t[[5, 6], [7, "
    "8]]",
    "[5, 6]\t[e, f]\t[e!, f!]\t[(50, e, 100), (60, f, 200)]\t[[5], [6]]\t[100, 200]\t[9, 9]\t[0:5, 1:60]\t[[9, 10], "
    "[11, 12]]",
    "[7, NULL]\t[g, h]\t[g!, NULL]\t[(70, g, 100), (80, h, 200)]\t[[7], [8]]\t[100, 200]\t[NULL, NULL]\t[0:7, "
    "1:80]\t[[13, 14], "
    "[15, 16]]",
};
const string NULL_ROW = "NULL\tNULL\tNULL\tNULL\tNULL\tNULL\tNULL\tNULL\tNULL";

// Builds a record batch of the column, as the view arrow_view, and checks that query returns expected.
bool QueryMatches(FieldPtr column, const string &query, const string &expected) {
	std::vector<FieldPtr> columns;
	columns.push_back(std::move(column));
	auto record = Record(std::move(columns));
	ArrowArrayStream stream {};
	AdbcError err {};
	if (duckdb_adbc::BatchToArrayStream(&record->array, &record->schema, &stream, &err) != ADBC_STATUS_OK) {
		return false;
	}
	DuckDB db(nullptr);
	Connection con(db);
	con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(stream))->CreateView("arrow_view");
	auto result = con.Query(query);
	auto expected_result = con.Query(expected);
	stream.release = nullptr;
	if (result->HasError() || expected_result->HasError()) {
		return false;
	}
	return result->Equals(*expected_result, false);
}

// Row 0 is NULL over the inner rows ['1000', '1000'] and ['5', '5'], row 1 is [['5', '5'], NULL].
const std::vector<string> STRING_VALUES = {"1000", "1000", "5", "5", "5", "5", "5", "5"};

} // namespace arrow_fixed_size_list_null_children_test

TEST_CASE("Arrow scan of a NULL FIXED_SIZE_LIST row over each child kind", "[arrow]") {
	using namespace arrow_fixed_size_list_null_children_test;
	auto record = AllKinds(0, {1});
	auto expected = ALL_KINDS_ROWS;
	expected[1] = NULL_ROW;
	RequireRows(*record, expected);
}

TEST_CASE("Arrow scan of FIXED_SIZE_LIST columns without NULL rows", "[arrow]") {
	using namespace arrow_fixed_size_list_null_children_test;
	auto record = AllKinds(0, {});
	RequireRows(*record, ALL_KINDS_ROWS);
}

TEST_CASE("Arrow scan of a NULL FIXED_SIZE_LIST row at array and child offsets", "[arrow]") {
	using namespace arrow_fixed_size_list_null_children_test;
	// the elements start at slot 2 and the lists at their slot 1, which is NULL over the element slots 4 and 5
	auto record = AllKinds(2, {1});
	for (auto &column : record->children) {
		column = Sliced(std::move(column), 1);
	}
	record->array.length = 2;
	RequireRows(*record, {NULL_ROW, ALL_KINDS_ROWS[3]});
}

TEST_CASE("Arrow scan of NULL rows of nested FIXED_SIZE_LIST columns", "[arrow]") {
	using namespace arrow_fixed_size_list_null_children_test;
	// the outer row 0 is NULL over valid inner rows, the inner row 3 is NULL under the valid outer row 1
	std::vector<FieldPtr> columns;
	columns.push_back(FixedSizeList("d", 2, FixedSizeList("item", 2, DictionaryElements(), {3}), {0}));
	columns.push_back(FixedSizeList("w", 2, FixedSizeList("item", 2, FixedBinaryElements(), {3}), {0}));
	columns.push_back(FixedSizeList("u", 2, FixedSizeList("item", 2, UnionElements(), {3}), {0}));
	auto record = Record(std::move(columns));
	RequireRows(*record, {"NULL\tNULL\tNULL", "[[100, 200], NULL]\t[[e!, f!], NULL]\t[[0:5, 1:60], NULL]"});
}

TEST_CASE("Arrow scan of FIXED_SIZE_LIST fields inside a NULL struct row", "[arrow]") {
	using namespace arrow_fixed_size_list_null_children_test;
	std::vector<FieldPtr> fields;
	fields.push_back(FixedSizeList("d", 2, DictionaryElements()));
	fields.push_back(FixedSizeList("w", 2, FixedBinaryElements()));
	fields.push_back(FixedSizeList("u", 2, UnionElements()));
	std::vector<FieldPtr> columns;
	columns.push_back(Struct("s", std::move(fields), {1}));
	auto record = Record(std::move(columns));
	RequireRows(*record, {"([100, 200], [a!, b!], [0:1, 1:20])", "NULL", "([100, 200], [e!, f!], [0:5, 1:60])",
	                      "([100, 200], [g!, NULL], [0:7, 1:80])"});
}

TEST_CASE("Arrow scan of a NULL FIXED_SIZE_LIST row over structs whose fields set their own validity", "[arrow]") {
	using namespace arrow_fixed_size_list_null_children_test;
	std::vector<FieldPtr> fields;
	fields.push_back(FixedBinaryElements());
	fields.push_back(UnionElements());
	fields.push_back(RunEndElements());
	fields.push_back(DictionaryElements());
	std::vector<FieldPtr> columns;
	columns.push_back(FixedSizeList("t", 2, Struct("item", std::move(fields)), {1}));
	auto record = Record(std::move(columns));
	RequireRows(*record, {"[(a!, 0:1, 7, 100), (b!, 1:20, 7, 200)]", "NULL", "[(e!, 0:5, 9, 100), (f!, 1:60, 9, 200)]",
	                      "[(g!, 0:7, NULL, 100), (NULL, 1:80, NULL, 200)]"});
}

TEST_CASE("Arrow scan of a NULL struct row over fields that set their own validity", "[arrow]") {
	using namespace arrow_fixed_size_list_null_children_test;
	std::vector<FieldPtr> fields;
	fields.push_back(FixedBinaryElements());
	fields.push_back(UnionElements());
	fields.push_back(RunEndElements());
	std::vector<FieldPtr> columns;
	columns.push_back(Struct("s", std::move(fields), {2}));
	auto record = Record(std::move(columns));
	RequireRows(*record, {"(a!, 0:1, 7)", "(b!, 1:20, 7)", "NULL", "(d!, 1:40, 8)", "(e!, 0:5, 9)", "(f!, 1:60, 9)",
	                      "(g!, 0:7, NULL)", "(NULL, 1:80, NULL)"});
}

TEST_CASE("Arrow scan of sparse unions whose null count is not zero", "[arrow]") {
	using namespace arrow_fixed_size_list_null_children_test;
	// a sparse union has no validity bitmap, its first buffer holds the type ids, which read as a bitmap are NULLs
	auto four_slots = [](int64_t null_count) {
		std::vector<FieldPtr> members;
		members.push_back(Int32("i", {1, 2, 3, 4}));
		members.push_back(Int32("j", {10, 20, 30, 40}));
		auto field = SparseUnion("u", {0, 1, 0, 1}, std::move(members));
		field->array.null_count = null_count;
		return field;
	};
	auto elements = UnionElements();
	elements->array.null_count = -1;
	std::vector<FieldPtr> fields;
	fields.push_back(four_slots(-1));
	std::vector<FieldPtr> columns;
	columns.push_back(FixedSizeList("a", 2, std::move(elements), {1}));
	columns.push_back(Struct("s", std::move(fields), {2}));
	columns.push_back(four_slots(1));
	// a list member merges the parent mask it is given into its rows
	std::vector<FieldPtr> members;
	members.push_back(List("l", {0, 1, 2, 3, 4}, Int32("item", {7, 8, 9, 10})));
	members.push_back(Int32("i", {1, 2, 3, 4}));
	columns.push_back(SparseUnion("m", {0, 1, 0, 1}, std::move(members)));
	columns.back()->array.null_count = -1;
	auto record = Record(std::move(columns));
	RequireRows(*record, {"[0:1, 1:20]\t(0:1)\t0:1\t0:[7]", "NULL\t(1:20)\t1:20\t1:2", "[0:5, 1:60]\tNULL\t0:3\t0:[9]",
	                      "[0:7, 1:80]\t(1:40)\t1:40\t1:4"});
}

TEST_CASE("Arrow scan of NULL FIXED_SIZE_LIST rows past the first vector", "[arrow]") {
	using namespace arrow_fixed_size_list_null_children_test;
	constexpr idx_t ROWS = 3000;
	const std::vector<idx_t> null_rows {5, 2048, 2051, 2999};
	std::vector<int32_t> indices;
	std::vector<uint8_t> letters;
	for (idx_t slot = 0; slot < 2 * ROWS; slot++) {
		indices.push_back(int32_t(slot % 2));
		letters.push_back(uint8_t('a' + slot % 26));
	}
	std::vector<FieldPtr> columns;
	columns.push_back(FixedSizeList("d", 2, Dictionary("item", indices, Int32("", {100, 200})), null_rows));
	columns.push_back(FixedSizeList("w", 2, FixedBinary("item", letters, {4101}), null_rows));
	auto record = Record(std::move(columns));
	std::vector<string> expected;
	for (idx_t row = 0; row < ROWS; row++) {
		if (std::find(null_rows.begin(), null_rows.end(), row) != null_rows.end()) {
			expected.push_back("NULL\tNULL");
			continue;
		}
		auto first = string(1, char('a' + (2 * row) % 26)) + "!";
		auto second = row == 2050 ? string("NULL") : string(1, char('a' + (2 * row + 1) % 26)) + "!";
		expected.push_back("[100, 200]\t[" + first + ", " + second + "]");
	}
	RequireRows(*record, expected);
}

TEST_CASE("The vector verifier requires the child slots of a NULL ARRAY row to be NULL", "[arrow]") {
	using namespace arrow_fixed_size_list_null_children_test;
	VerifyVectors verify_vectors;
	Vector vector(LogicalType::ARRAY(LogicalType::INTEGER, 2), 2);
	auto values = FlatVector::GetDataMutable<int32_t>(ArrayVector::GetChildMutable(vector));
	for (int32_t slot = 0; slot < 4; slot++) {
		values[slot] = slot;
	}
	FlatVector::SetSize(vector, 2);
	FlatVector::ValidityMutable(vector).SetInvalid(1);
#ifndef DUCKDB_CRASH_ON_ASSERT
	// an InternalException aborts the process when DUCKDB_CRASH_ON_ASSERT is set
	REQUIRE_THROWS_WITH(vector.Verify(), Catch::Contains("Array NULL mismatch"));
#endif
	FlatVector::SetNull(vector, 1, true);
	REQUIRE_NOTHROW(vector.Verify());

	Vector constant(LogicalType::ARRAY(LogicalType::INTEGER, 2), 1);
	auto constant_values = FlatVector::GetDataMutable<int32_t>(ArrayVector::GetChildMutable(constant));
	constant_values[0] = 1;
	constant_values[1] = 2;
	FlatVector::SetSize(constant, 1);
	constant.SetVectorType(VectorType::CONSTANT_VECTOR);
	constant.BufferMutable().GetValidityMask().SetInvalid(0);
#ifndef DUCKDB_CRASH_ON_ASSERT
	REQUIRE_THROWS_WITH(constant.Verify(), Catch::Contains("Array NULL mismatch"));
#endif
	ConstantVector::SetNull(constant, true);
	REQUIRE_NOTHROW(constant.Verify());
}

TEST_CASE("Arrow scan of a NULL FIXED_SIZE_LIST row over FIXED_SIZE_LIST children with NULLs", "[arrow]") {
	using namespace arrow_fixed_size_list_null_children_test;
	auto inner = FixedSizeList("item", 2, Utf8("item", STRING_VALUES), {3});
	REQUIRE(QueryMatches(FixedSizeList("a", 2, std::move(inner), {0}),
	                     "SELECT a::TINYINT[2][2]::VARCHAR FROM arrow_view",
	                     "SELECT * FROM (VALUES (NULL), ('[[5, 5], NULL]')) t(a)"));
	inner = FixedSizeList("item", 2, Utf8("item", STRING_VALUES), {3});
	REQUIRE(QueryMatches(FixedSizeList("a", 2, std::move(inner), {0}),
	                     "SELECT count(*) FROM arrow_view WHERE a IS NULL", "SELECT 1::BIGINT"));
}

TEST_CASE("Arrow scan of a NULL FIXED_SIZE_LIST row over LIST children with NULLs", "[arrow]") {
	using namespace arrow_fixed_size_list_null_children_test;
	auto inner = List("item", {0, 2, 4, 6, 8}, Utf8("item", STRING_VALUES), {3});
	REQUIRE(QueryMatches(FixedSizeList("a", 2, std::move(inner), {0}),
	                     "SELECT a::TINYINT[][2]::VARCHAR FROM arrow_view",
	                     "SELECT * FROM (VALUES (NULL), ('[[5, 5], NULL]')) t(a)"));
}

TEST_CASE("Arrow scan of a NULL FIXED_SIZE_LIST row three levels above the values", "[arrow]") {
	using namespace arrow_fixed_size_list_null_children_test;
	// the middle level has its own NULL, so the NULL of row 0 has to pass through it to the inner level
	auto innermost = FixedSizeList("item", 1, Utf8("item", {"1000", "1000", "1000", "1000", "5", "5", "5", "5"}));
	auto middle = FixedSizeList("item", 2, std::move(innermost), {3});
	REQUIRE(QueryMatches(FixedSizeList("a", 2, std::move(middle), {0}),
	                     "SELECT a::TINYINT[1][2][2]::VARCHAR FROM arrow_view",
	                     "SELECT * FROM (VALUES (NULL), ('[[[5], [5]], NULL]')) t(a)"));
}

TEST_CASE("Arrow scan of a NULL FIXED_SIZE_LIST row at an array offset", "[arrow]") {
	using namespace arrow_fixed_size_list_null_children_test;
	// the scan starts at row 1, which is NULL over ['1000', '1000'] and ['5', '5']
	auto strings = Utf8("item", {"5", "5", "5", "5", "1000", "1000", "5", "5", "5", "5", "5", "5"});
	auto inner = FixedSizeList("item", 2, std::move(strings), {5});
	REQUIRE(QueryMatches(Sliced(FixedSizeList("a", 2, std::move(inner), {1}), 1),
	                     "SELECT a::TINYINT[2][2]::VARCHAR FROM arrow_view",
	                     "SELECT * FROM (VALUES (NULL), ('[[5, 5], NULL]')) t(a)"));
}
