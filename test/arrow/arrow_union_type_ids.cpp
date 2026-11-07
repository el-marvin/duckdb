// Arrow scans of sparse unions whose format string lists type ids other than 0, 1, 2 and so on in child order.

#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb.h"
#include "test_helpers.hpp"

using namespace duckdb;

namespace arrow_union_type_ids_test {

void ReleaseSchema(ArrowSchema *s) {
	s->release = nullptr;
}
void ReleaseArray(ArrowArray *a) {
	a->release = nullptr;
}

// A hand built Arrow field and its array, owning its children, its dictionary and its buffers.
struct Field {
	ArrowSchema schema {};
	ArrowArray array {};
	vector<unique_ptr<Field>> children;
	unique_ptr<Field> dictionary;
	vector<int32_t> data;
	string chars;
	vector<uint8_t> validity;
	vector<int8_t> type_ids;
	vector<ArrowSchema *> schema_children;
	vector<ArrowArray *> array_children;
	vector<const void *> buffers;
};
using FieldPtr = unique_ptr<Field>;

FieldPtr MakeField(const char *format, const char *name, int64_t length, vector<FieldPtr> children) {
	auto field = make_uniq<Field>();
	field->schema.format = format;
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

void SetBuffers(Field &field, vector<const void *> buffers) {
	field.buffers = std::move(buffers);
	field.array.n_buffers = int64_t(field.buffers.size());
	field.array.buffers = field.buffers.data();
}

const void *Validity(Field &field, const vector<idx_t> &null_rows) {
	if (null_rows.empty()) {
		return nullptr;
	}
	field.validity.assign((idx_t(field.array.length) + 7) / 8, 0xFF);
	for (auto row : null_rows) {
		field.validity[row / 8] &= uint8_t(~(1 << (row % 8)));
	}
	field.array.null_count = int64_t(null_rows.size());
	return field.validity.data();
}

FieldPtr Int32(const char *name, vector<int32_t> values, const vector<idx_t> &null_rows = {}) {
	auto field = MakeField("i", name, int64_t(values.size()), {});
	field->data = std::move(values);
	SetBuffers(*field, {Validity(*field, null_rows), field->data.data()});
	return field;
}

FieldPtr Utf8(const char *name, const vector<string> &values, const vector<idx_t> &null_rows = {}) {
	auto field = MakeField("u", name, int64_t(values.size()), {});
	field->data.push_back(0);
	for (auto &value : values) {
		field->chars += value;
		field->data.push_back(int32_t(field->chars.size()));
	}
	SetBuffers(*field, {Validity(*field, null_rows), field->data.data(), field->chars.data()});
	return field;
}

FieldPtr Struct(const char *name, vector<FieldPtr> children) {
	auto length = children[0]->array.length;
	auto field = MakeField("+s", name, length, std::move(children));
	SetBuffers(*field, {nullptr});
	return field;
}

FieldPtr List(const char *name, vector<int32_t> offsets, FieldPtr child) {
	vector<FieldPtr> children;
	children.push_back(std::move(child));
	auto field = MakeField("+l", name, int64_t(offsets.size()) - 1, std::move(children));
	field->data = std::move(offsets);
	SetBuffers(*field, {nullptr, field->data.data()});
	return field;
}

FieldPtr Dictionary(const char *name, vector<int32_t> indices, FieldPtr values) {
	auto field = Int32(name, std::move(indices));
	field->dictionary = std::move(values);
	field->schema.dictionary = &field->dictionary->schema;
	field->array.dictionary = &field->dictionary->array;
	return field;
}

FieldPtr SparseUnion(const char *name, const char *format, vector<int8_t> type_ids, vector<FieldPtr> members) {
	auto field = MakeField(format, name, int64_t(type_ids.size()), std::move(members));
	field->type_ids = std::move(type_ids);
	SetBuffers(*field, {field->type_ids.data()});
	return field;
}

// The union u of the reversed type ids 1 and 0: i is the child of id 1, s the child of id 0.
FieldPtr ReversedUnion(const char *name, vector<int8_t> type_ids) {
	vector<int32_t> ints;
	vector<string> strings;
	for (idx_t row = 0; row < type_ids.size(); row++) {
		ints.push_back(int32_t(row + 1));
		strings.push_back(string(1, char('a' + row)));
	}
	vector<FieldPtr> members;
	members.push_back(Int32("i", std::move(ints)));
	members.push_back(Utf8("s", strings));
	return SparseUnion(name, "+us:1,0", std::move(type_ids), std::move(members));
}

// The union u of the type ids 2, 0 and 1: i is the child of id 2, s of id 0 and j of id 1.
FieldPtr RotatedUnion(const char *name, vector<int8_t> type_ids) {
	vector<int32_t> ints;
	vector<string> strings;
	vector<int32_t> tens;
	for (idx_t row = 0; row < type_ids.size(); row++) {
		ints.push_back(int32_t(row + 1));
		strings.push_back(string(1, char('a' + row)));
		tens.push_back(int32_t(10 * (row + 1)));
	}
	vector<FieldPtr> members;
	members.push_back(Int32("i", std::move(ints)));
	members.push_back(Utf8("s", strings));
	members.push_back(Int32("j", std::move(tens)));
	return SparseUnion(name, "+us:2,0,1", std::move(type_ids), std::move(members));
}

FieldPtr Record(vector<FieldPtr> columns) {
	return Struct("", std::move(columns));
}

FieldPtr Record(FieldPtr column) {
	vector<FieldPtr> columns;
	columns.push_back(std::move(column));
	return Record(std::move(columns));
}

struct BatchStream {
	vector<reference<Field>> batches;
	idx_t next = 0;
};

int GetSchema(ArrowArrayStream *stream, ArrowSchema *out) {
	*out = static_cast<BatchStream *>(stream->private_data)->batches[0].get().schema;
	return 0;
}

int GetNext(ArrowArrayStream *stream, ArrowArray *out) {
	auto &state = *static_cast<BatchStream *>(stream->private_data);
	if (state.next == state.batches.size()) {
		out->release = nullptr;
		return 0;
	}
	*out = state.batches[state.next++].get().array;
	return 0;
}

const char *GetLastError(ArrowArrayStream *) {
	return nullptr;
}

void ReleaseStream(ArrowArrayStream *stream) {
	unique_ptr<BatchStream> state(static_cast<BatchStream *>(stream->private_data));
	stream->release = nullptr;
}

// Serves the record batches, which share the schema of the first, as a new stream for every scan.
class BatchScanFactory : public ArrowScanFactory {
public:
	explicit BatchScanFactory(vector<reference<Field>> batches_p) : batches(std::move(batches_p)) {
	}
	void GetSchema(ArrowSchema &out) override {
		out = batches[0].get().schema;
	}
	unique_ptr<ArrowArrayStreamWrapper> ProduceStream(ArrowStreamParameters &) override {
		auto result = make_uniq<ArrowArrayStreamWrapper>();
		auto state = make_uniq<BatchStream>();
		state->batches = batches;
		result->arrow_array_stream.private_data = state.release();
		result->arrow_array_stream.get_schema = arrow_union_type_ids_test::GetSchema;
		result->arrow_array_stream.get_next = GetNext;
		result->arrow_array_stream.get_last_error = GetLastError;
		result->arrow_array_stream.release = ReleaseStream;
		return result;
	}

	vector<reference<Field>> batches;
};

// Runs the query over the batches scanned as src, one row per line with tab separated values, or the error.
string Scan(vector<reference<Field>> batches, const string &query) {
	DuckDB db(nullptr);
	Connection con(db);
	// one thread keeps the rows of several record batches in order
	REQUIRE_NO_FAIL(con.Query("SET threads = 1"));
	auto factory = make_shared_ptr<BatchScanFactory>(std::move(batches));
	try {
		auto result = con.TableFunction("arrow_scan", {}, {}, factory)->Query("src", query);
		if (result->HasError()) {
			return "error: " + result->GetError();
		}
		string text;
		for (auto chunk = result->Fetch(); chunk; chunk = result->Fetch()) {
			for (idx_t row = 0; row < chunk->size(); row++) {
				for (idx_t column = 0; column < chunk->ColumnCount(); column++) {
					text += (column ? "\t" : "") + chunk->GetValue(column, row).ToString();
				}
				text += "\n";
			}
		}
		return text;
	} catch (std::exception &ex) {
		ErrorData error(ex);
		return "error: " + error.Message();
	}
}

string Scan(Field &record, const string &query) {
	return Scan(vector<reference<Field>> {record}, query);
}

TEST_CASE("Arrow sparse union with identity type ids", "[arrow]") {
	vector<FieldPtr> members;
	members.push_back(Int32("i", {1, 2, 3}));
	members.push_back(Utf8("s", {"a", "b", "c"}));
	auto record = Record(SparseUnion("u", "+us:0,1", {0, 1, 0}, std::move(members)));
	REQUIRE(Scan(*record, "SELECT typeof(u), union_tag(u), u FROM src") == "UNION(i INTEGER, s VARCHAR)\ti\t1\n"
	                                                                       "UNION(i INTEGER, s VARCHAR)\ts\tb\n"
	                                                                       "UNION(i INTEGER, s VARCHAR)\ti\t3\n");
}

TEST_CASE("Arrow sparse union with reversed type ids", "[arrow]") {
	// i is the child of type id 1 and s the child of type id 0; NULLs in the child a row does not select do not matter
	vector<FieldPtr> members;
	members.push_back(Int32("i", {1, 2, 3, 4, 5}, {1, 4}));
	members.push_back(Utf8("s", {"a", "b", "c", "d", "e"}, {0, 3}));
	auto record = Record(SparseUnion("u", "+us:1,0", {1, 0, 1, 0, 1}, std::move(members)));
	REQUIRE(Scan(*record, "SELECT typeof(u), union_tag(u), u, union_extract(u, 'i'), union_extract(u, 's') FROM src") ==
	        "UNION(i INTEGER, s VARCHAR)\ti\t1\t1\tNULL\n"
	        "UNION(i INTEGER, s VARCHAR)\ts\tb\tNULL\tb\n"
	        "UNION(i INTEGER, s VARCHAR)\ti\t3\t3\tNULL\n"
	        "UNION(i INTEGER, s VARCHAR)\tNULL\tNULL\tNULL\tNULL\n"
	        "UNION(i INTEGER, s VARCHAR)\tNULL\tNULL\tNULL\tNULL\n");
}

TEST_CASE("Arrow sparse union with type ids that do not start at 0", "[arrow]") {
	SECTION("type ids 0, 5 and 7") {
		vector<FieldPtr> members;
		members.push_back(Int32("i", {1, 2, 3, 4}));
		members.push_back(Utf8("s", {"a", "b", "c", "d"}));
		members.push_back(Int32("j", {10, 20, 30, 40}));
		auto record = Record(SparseUnion("u", "+us:0,5,7", {7, 5, 0, 7}, std::move(members)));
		REQUIRE(Scan(*record, "SELECT union_tag(u), u FROM src") == "j\t10\ns\tb\ni\t3\nj\t40\n");
	}
	SECTION("the largest type id, 127, also with leading zeros") {
		for (auto format : {"+us:127,0", "+us:0127,00"}) {
			INFO(format);
			vector<FieldPtr> members;
			members.push_back(Int32("i", {1, 2}));
			members.push_back(Utf8("s", {"a", "b"}));
			auto record = Record(SparseUnion("u", format, {0, 127}, std::move(members)));
			REQUIRE(Scan(*record, "SELECT union_tag(u), u FROM src") == "s\ta\ni\t2\n");
		}
	}
}

TEST_CASE("Arrow sparse union formats with a malformed type id list are refused at bind", "[arrow]") {
	const char *formats[] = {"+us",      "+us:",      "+us:0",   "+us:0,1,2", "+us:0,,1",  "+us:0,1,",
	                         "+us:,0,1", "+us:0,0",   "+us:1,1", "+us:-1,0",  "+us:+1,0",  "+us: 1,0",
	                         "+us:1 ,0", "+us:128,0", "+us:a,0", "+us:1;0",   "+us:0x1,0", "+us:99999999999999999999,0",
	                         "+usx0,1"};
	for (auto format : formats) {
		INFO(format);
		// the union has no rows, so only the bind can raise the error
		vector<FieldPtr> members;
		members.push_back(Int32("i", {}));
		members.push_back(Utf8("s", {}));
		auto record = Record(SparseUnion("u", format, {}, std::move(members)));
		auto result = Scan(*record, "SELECT u FROM src");
		CHECK(StringUtil::StartsWith(result, "error: Invalid Input Error: "));
		CHECK(StringUtil::Contains(result, string("\"") + format + "\""));
	}
	vector<FieldPtr> members;
	members.push_back(Int32("i", {}));
	members.push_back(Utf8("s", {}));
	auto record = Record(SparseUnion("u", "+ud:0,1", {}, std::move(members)));
	REQUIRE(StringUtil::StartsWith(Scan(*record, "SELECT u FROM src"), "error: Not implemented Error: "));
}

TEST_CASE("Arrow sparse union rows with a type id the format does not list are a scan error", "[arrow]") {
	// rows 0 and 1 are valid, so the error is raised for the third row
	for (int8_t type_id : {int8_t(2), int8_t(-1), int8_t(127), int8_t(-128)}) {
		INFO(int(type_id));
		auto record = Record(ReversedUnion("u", {0, 1, type_id}));
		REQUIRE(Scan(*record, "SELECT u FROM src") == "error: Invalid Input Error: Arrow union row has type id " +
		                                                  to_string(int(type_id)) +
		                                                  ", which is not one of the type ids of the union");
	}
	vector<FieldPtr> members;
	members.push_back(Int32("i", {1, 2}));
	members.push_back(Utf8("s", {"a", "b"}));
	members.push_back(Int32("j", {10, 20}));
	auto record = Record(SparseUnion("u", "+us:0,5,7", {5, 1}, std::move(members)));
	REQUIRE(Scan(*record, "SELECT u FROM src") ==
	        "error: Invalid Input Error: Arrow union row has type id 1, which is not one of the type ids of the union");
}

TEST_CASE("Arrow sparse unions with custom type ids inside and around other types", "[arrow]") {
	SECTION("inside a struct") {
		vector<FieldPtr> fields;
		fields.push_back(RotatedUnion("u", {0, 1, 2}));
		fields.push_back(Int32("k", {7, 8, 9}));
		auto record = Record(Struct("t", std::move(fields)));
		REQUIRE(Scan(*record, "SELECT union_tag(t.u), t FROM src") ==
		        "s\t{'u': a, 'k': 7}\nj\t{'u': 20, 'k': 8}\ni\t{'u': 3, 'k': 9}\n");
	}
	SECTION("inside a list") {
		auto record = Record(List("l", {0, 2, 2, 4}, RotatedUnion("u", {2, 0, 1, 2})));
		REQUIRE(Scan(*record, "SELECT l, list_transform(l, lambda x: union_tag(x)) FROM src") ==
		        "[1, b]\t[i, s]\n[]\t[]\n[30, 4]\t[j, i]\n");
	}
	SECTION("a struct member") {
		vector<FieldPtr> struct_fields;
		struct_fields.push_back(Int32("a", {1, 2, 3}));
		vector<FieldPtr> members;
		members.push_back(Utf8("s", {"x", "y", "z"}));
		members.push_back(Struct("t", std::move(struct_fields)));
		members.push_back(Int32("k", {7, 8, 9}));
		auto record = Record(SparseUnion("u", "+us:2,0,1", {0, 2, 1}, std::move(members)));
		REQUIRE(Scan(*record, "SELECT union_tag(u), u FROM src") == "t\t{'a': 1}\ns\ty\nk\t9\n");
	}
	SECTION("a union member with other type ids") {
		vector<FieldPtr> members;
		members.push_back(RotatedUnion("inner", {0, 1, 2}));
		members.push_back(Int32("k", {7, 8, 9}));
		auto record = Record(SparseUnion("u", "+us:1,0", {1, 0, 1}, std::move(members)));
		REQUIRE(Scan(*record, "SELECT union_tag(u), union_tag(union_extract(u, 'inner')), u FROM src") ==
		        "inner\ts\ta\nk\tNULL\t8\ninner\ti\t3\n");
	}
	SECTION("the values of a dictionary") {
		auto record = Record(Dictionary("d", {2, 0, 1, 1}, RotatedUnion("u", {0, 1, 2})));
		REQUIRE(Scan(*record, "SELECT union_tag(d), d FROM src") == "i\t3\ns\ta\nj\t20\nj\t20\n");
	}
	SECTION("a dictionary encoded member") {
		vector<FieldPtr> members;
		members.push_back(Dictionary("i", {1, 0, 1}, Int32("", {10, 20})));
		members.push_back(Utf8("s", {"a", "b", "c"}));
		auto record = Record(SparseUnion("u", "+us:1,0", {1, 0, 1}, std::move(members)));
		REQUIRE(Scan(*record, "SELECT union_tag(u), u FROM src") == "i\t20\ns\tb\ni\t20\n");
	}
	SECTION("with an offset") {
		auto u = RotatedUnion("u", {1, 1, 0, 2});
		u->array.offset = 1;
		u->array.length = 3;
		auto record = Record(std::move(u));
		REQUIRE(Scan(*record, "SELECT union_tag(u), u FROM src") == "j\t20\ns\tc\ni\t4\n");
	}
}

TEST_CASE("Arrow sparse union with reversed type ids over several record batches", "[arrow]") {
	auto first = Record(ReversedUnion("u", {1, 0}));
	auto second = Record(ReversedUnion("u", {0, 0, 1}));
	REQUIRE(Scan({*first, *second}, "SELECT union_tag(u), u FROM src") == "i\t1\ns\tb\ns\ta\ns\tb\ni\t3\n");
}

TEST_CASE("Filter above an Arrow scan of a sparse union with reversed type ids", "[arrow]") {
	auto record = Record(ReversedUnion("u", {1, 0, 1, 0}));
	REQUIRE(Scan(*record, "SELECT u FROM src WHERE union_tag(u) = 'i'") == "1\n3\n");
	REQUIRE(Scan(*record, "SELECT u FROM src WHERE union_extract(u, 's') IS NOT NULL") == "b\nd\n");
	REQUIRE(Scan(*record, "SELECT count(*) FROM src WHERE union_extract(u, 'i') > 1") == "1\n");
}

TEST_CASE("duckdb_schema_from_arrow of a sparse union with reversed type ids", "[arrow]") {
	duckdb_database db = nullptr;
	duckdb_connection con = nullptr;
	REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(db, &con) == DuckDBSuccess);

	auto record = Record(ReversedUnion("u", {1, 0, 0}));
	duckdb_arrow_converted_schema converted = nullptr;
	auto error = duckdb_schema_from_arrow(con, &record->schema, &converted);
	REQUIRE(error == nullptr);
	duckdb_data_chunk chunk = nullptr;
	error = duckdb_data_chunk_from_arrow(con, &record->array, converted, &chunk);
	REQUIRE(error == nullptr);
	REQUIRE(duckdb_data_chunk_get_size(chunk) == 3);
	auto union_vector = duckdb_data_chunk_get_vector(chunk, 0);
	auto type = duckdb_vector_get_column_type(union_vector);
	REQUIRE(duckdb_get_type_id(type) == DUCKDB_TYPE_UNION);
	REQUIRE(duckdb_union_type_member_count(type) == 2);
	auto first_name = duckdb_union_type_member_name(type, 0);
	auto second_name = duckdb_union_type_member_name(type, 1);
	REQUIRE(string(first_name) == "i");
	REQUIRE(string(second_name) == "s");
	duckdb_free(first_name);
	duckdb_free(second_name);
	duckdb_destroy_logical_type(&type);
	// the tag of a union vector is its first struct child, the member position in the union
	auto tags = static_cast<uint8_t *>(duckdb_vector_get_data(duckdb_struct_vector_get_child(union_vector, 0)));
	REQUIRE(tags[0] == 0);
	REQUIRE(tags[1] == 1);
	REQUIRE(tags[2] == 1);
	auto ints = static_cast<int32_t *>(duckdb_vector_get_data(duckdb_struct_vector_get_child(union_vector, 1)));
	REQUIRE(ints[0] == 1);
	duckdb_destroy_data_chunk(&chunk);
	duckdb_destroy_arrow_converted_schema(&converted);

	auto repeated = Record(SparseUnion("u", "+us:1,1", {1}, [] {
		vector<FieldPtr> members;
		members.push_back(Int32("i", {1}));
		members.push_back(Utf8("s", {"a"}));
		return members;
	}()));
	error = duckdb_schema_from_arrow(con, &repeated->schema, &converted);
	REQUIRE(error != nullptr);
	REQUIRE(duckdb_error_data_error_type(error) == DUCKDB_ERROR_INVALID_INPUT);
	REQUIRE(StringUtil::Contains(duckdb_error_data_message(error), "type id 1 is listed twice"));
	duckdb_destroy_error_data(&error);
	REQUIRE(converted == nullptr);

	duckdb_disconnect(&con);
	duckdb_close(&db);
}

} // namespace arrow_union_type_ids_test
