#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/row/integer_partitioned_tuple_data.hpp"
#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/common/types/row/tuple_data_iterator.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

TEST_CASE("Integer-partitioned row append respects the source selection", "[tuple_data_allocation]") {
	if (STANDARD_VECTOR_SIZE < 6) {
		return;
	}
	DuckDB db(nullptr);
	Connection con(db);
	auto &buffer_manager = BufferManager::GetBufferManager(*con.context);
	vector<LogicalType> types {LogicalType::BIGINT};
	auto layout = make_shared_ptr<TupleDataLayout>();
	layout->Initialize(types, TupleDataValidityType::CAN_HAVE_NULL_VALUES);
	TupleDataCollection source(buffer_manager, layout, MemoryTag::ORDER_BY);
	TupleDataAppendState source_state;
	source.InitializeAppend(source_state, TupleDataPinProperties::KEEP_EVERYTHING_PINNED);
	DataChunk chunk;
	chunk.Initialize(Allocator::DefaultAllocator(), types);
	{
		auto keys = FlatVector::Writer<int64_t>(chunk.data[0], 6);
		keys.WriteValue(0);
		keys.WriteValue(1);
		keys.WriteValue(2);
		keys.WriteNull();
		keys.WriteValue(99);
		keys.WriteValue(-1);
	}
	source.Append(source_state, chunk);

	IntegerPartitionedTupleData target(buffer_manager, layout, MemoryTag::ORDER_BY, 0, Value::BIGINT(0), 3);
	PartitionedTupleDataAppendState target_state;
	target.InitializeAppendState(target_state);
	SelectionVector selection(5);
	const idx_t selected_rows[] = {4, 2, 3, 5, 2};
	for (idx_t i = 0; i < 5; i++) {
		selection.set_index(i, selected_rows[i]);
	}
	target.Append(target_state, source_state.chunk_state, 5, selection);
	target.FlushAppendState(target_state);
	REQUIRE(target.Count() == 5);

	vector<vector<Value>> expected {{},
	                                {},
	                                {Value::BIGINT(2), Value::BIGINT(2)},
	                                {Value(LogicalType::BIGINT)},
	                                {Value::BIGINT(99), Value::BIGINT(-1)}};
	for (idx_t i = 0; i < expected.size(); i++) {
		auto &partition = *target.GetPartitions()[i];
		REQUIRE(partition.Count() == expected[i].size());
		TupleDataScanState scan;
		partition.InitializeScan(scan);
		idx_t row = 0;
		while (partition.Scan(scan, chunk)) {
			for (idx_t j = 0; j < chunk.size(); j++) {
				REQUIRE(Value::NotDistinctFrom(chunk.GetValue(0, j), expected[i][row++]));
			}
		}
		REQUIRE(row == expected[i].size());
	}
}

TEST_CASE("Tuple data allocation accounting follows retained blocks", "[tuple_data_allocation]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &buffer_manager = BufferManager::GetBufferManager(*con.context);
	vector<LogicalType> types {LogicalType::BIGINT, LogicalType::VARCHAR};
	auto layout = make_shared_ptr<TupleDataLayout>();
	layout->Initialize(types, TupleDataValidityType::CAN_HAVE_NULL_VALUES);
	TupleDataCollection first(buffer_manager, layout, MemoryTag::HASH_TABLE);
	TupleDataCollection second(buffer_manager, layout, MemoryTag::HASH_TABLE);
	REQUIRE(first.GetBlockAllocationSize() == 0);

	DataChunk chunk;
	chunk.Initialize(Allocator::DefaultAllocator(), types);
	{
		auto keys = FlatVector::Writer<int64_t>(chunk.data[0], STANDARD_VECTOR_SIZE);
		auto strings = FlatVector::Writer<string_t>(chunk.data[1], STANDARD_VECTOR_SIZE);
		const string payload(4096, 'x');
		for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
			keys.WriteValue(NumericCast<int64_t>(i));
			strings.WriteValue(string_t(payload));
		}
	}
	chunk.CheckCardinality(STANDARD_VECTOR_SIZE);
	first.Append(chunk);
	const auto first_size = first.GetBlockAllocationSize();
	REQUIRE(first_size >= STANDARD_VECTOR_SIZE * (4096 + layout->GetRowWidth()));
	first.Unpin();
	REQUIRE(first.GetBlockAllocationSize() == first_size);

	second.Append(chunk);
	const auto second_size = second.GetBlockAllocationSize();
	first.Combine(second);
	REQUIRE(first.GetBlockAllocationSize() == first_size + second_size);
	REQUIRE(second.GetBlockAllocationSize() == 0);

	// Reusing the source must not release allocations transferred to the destination.
	second.Append(chunk);
	second.Reset();
	REQUIRE(second.GetBlockAllocationSize() == 0);
	REQUIRE(first.GetBlockAllocationSize() == first_size + second_size);

	{
		TupleDataChunkIterator iterator(first, TupleDataPinProperties::DESTROY_AFTER_DONE, true);
		while (!iterator.Done()) {
			iterator.Next();
		}
	}
	REQUIRE(first.GetBlockAllocationSize() == 0);
	first.Reset();
	first.Append(chunk);
	REQUIRE(first.GetBlockAllocationSize() == first_size);
	first.Reset();
	REQUIRE(first.GetBlockAllocationSize() == 0);
}

TEST_CASE("Tuple data heap reclamation crosses chunks without heap references", "[tuple_data_allocation]") {
	struct ReclamationCase {
		string name;
		vector<idx_t> string_lengths;
		idx_t begin;
		idx_t end;
		bool releases_heap;
	};
	vector<ReclamationCase> cases {
	    {"empty boundaries", {0, 1024, 0, 32}, 0, 2, true},
	    {"empty begin", {0, 1024, 32}, 0, 2, true},
	    {"empty end", {1024, 0, 0, 32}, 0, 1, true},
	    {"trailing empty chunks", {0, 1024, 0}, 0, 2, true},
	    {"later incremental range", {32, 0, 1024, 0, 32}, 1, 3, true},
	    {"all-empty range", {0, 0, 32}, 0, 2, false},
	    {"shared heap across a gap", {32, 0, 32, 0}, 0, 2, false},
	};
	vector<idx_t> long_gap(64, 0);
	long_gap.front() = 1024;
	long_gap.back() = 32;
	cases.push_back({"long empty gap", std::move(long_gap), 0, 1, true});

	for (const auto &test_case : cases) {
		DYNAMIC_SECTION(test_case.name) {
			DuckDB db(nullptr);
			Connection con(db);
			auto &buffer_manager = BufferManager::GetBufferManager(*con.context);
			vector<LogicalType> types {LogicalType::VARCHAR};
			auto layout = make_shared_ptr<TupleDataLayout>();
			layout->Initialize(types, TupleDataValidityType::CAN_HAVE_NULL_VALUES);
			TupleDataCollection collection(buffer_manager, layout, MemoryTag::ORDER_BY);
			const auto large_string_size =
			    MaxValue<idx_t>(1024, 4 * buffer_manager.GetBlockSize() / STANDARD_VECTOR_SIZE);
			auto payload_for_chunk = [&](idx_t chunk_idx) {
				const auto length = test_case.string_lengths[chunk_idx];
				return length ? string(length == 1024 ? large_string_size : length,
				                       static_cast<char>('a' + chunk_idx % 26))
				              : "inline";
			};
			TupleDataAppendState append_state;
			collection.InitializeAppend(append_state, TupleDataPinProperties::KEEP_EVERYTHING_PINNED);
			DataChunk chunk;
			chunk.Initialize(Allocator::DefaultAllocator(), types);
			for (idx_t chunk_idx = 0; chunk_idx < test_case.string_lengths.size(); chunk_idx++) {
				chunk.Reset();
				const auto length = test_case.string_lengths[chunk_idx];
				const auto payload = payload_for_chunk(chunk_idx);
				{
					auto writer = FlatVector::Writer<string_t>(chunk.data[0], STANDARD_VECTOR_SIZE);
					for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
						// One small heap value keeps sharing independent of the configured vector size.
						if (!length || (length == 32 && i != 0)) {
							if (i % 2 == 0) {
								writer.WriteNull();
							} else {
								writer.WriteValue(string_t("inline"));
							}
						} else {
							writer.WriteValue(string_t(payload));
						}
					}
				}
				collection.Append(append_state, chunk);
			}
			collection.FinalizePinState(append_state.pin_state);
			REQUIRE(collection.ChunkCount() == test_case.string_lengths.size());
			// Releasing more than all row memory before the end boundary proves that heap memory was released.
			const auto max_row_memory = (test_case.end * STANDARD_VECTOR_SIZE / collection.TuplesPerBlock() + 1) *
			                            buffer_manager.GetBlockAllocSize();
			if (test_case.begin) {
				collection.DestroyChunks(0, test_case.begin);
			}
			const auto before = buffer_manager.GetUsedMemory();
			collection.DestroyChunks(test_case.begin, test_case.end);
			collection.Unpin();
			const auto after = buffer_manager.GetUsedMemory();
			REQUIRE(after <= before);
			if (test_case.releases_heap) {
				REQUIRE(before - after > max_row_memory);
			} else {
				REQUIRE(before - after <= max_row_memory);
			}

			// Fresh pins must still work for later chunks, including a heap shared across an empty gap.
			TupleDataScanState scan_state;
			collection.InitializeScan(scan_state);
			collection.Seek(scan_state, test_case.end);
			idx_t chunk_idx = test_case.end;
			while (collection.Scan(scan_state, chunk)) {
				const auto length = test_case.string_lengths[chunk_idx];
				const auto expected = payload_for_chunk(chunk_idx);
				idx_t row_idx = 0;
				for (auto value : chunk.data[0].Values<string_t>()) {
					const auto inline_row = !length || (length == 32 && row_idx != 0);
					if (inline_row && row_idx % 2 == 0) {
						REQUIRE_FALSE(value.IsValid());
					} else {
						REQUIRE(value.IsValid());
						REQUIRE(value.GetValue().GetString() == (inline_row ? "inline" : expected));
					}
					row_idx++;
				}
				REQUIRE(row_idx == STANDARD_VECTOR_SIZE);
				chunk_idx++;
			}
			REQUIRE(chunk_idx == test_case.string_lengths.size());
		}
	}
}

//! Poisons the string slots of NULL rows, so that AddressSanitizer reports any read of them.
struct PoisonedNullStrings {
	vector<string_t *> slots;

	void Poison(string_t &slot) {
		slots.push_back(&slot);
#ifdef DUCKDB_ARENA_ALLOCATOR_ASAN
		__asan_poison_memory_region(&slot, sizeof(string_t));
#endif
	}
	~PoisonedNullStrings() {
#ifdef DUCKDB_ARENA_ALLOCATOR_ASAN
		for (auto slot : slots) {
			__asan_unpoison_memory_region(slot, sizeof(string_t));
		}
#endif
	}
};

TEST_CASE("Tuple data heap sizes do not read the strings of NULL rows", "[tuple_data_allocation]") {
	constexpr idx_t ROW_COUNT = 8;
	if (STANDARD_VECTOR_SIZE < ROW_COUNT) {
		return;
	}
	const string long_string(40, 'x');
	auto is_null_row = [](idx_t row) {
		return row % 2 == 1;
	};
	auto row_string = [&](idx_t row) {
		return row % 4 == 0 ? long_string : string("short");
	};
	auto make_selection = [](const vector<idx_t> &rows) {
		SelectionVector selection(rows.size());
		for (idx_t i = 0; i < rows.size(); i++) {
			selection.set_index(i, rows[i]);
		}
		return selection;
	};
	const vector<idx_t> dictionary_rows {7, 4, 1, 0, 3, 6};
	const vector<idx_t> append_rows {5, 2, 0, 3};

	const vector<string> shapes {
	    "flat",         "append selection", "dictionary source", "append selection over dictionary source",
	    "struct child", "null struct row",  "constant NULL"};
	for (const auto &shape : shapes) {
		DYNAMIC_SECTION(shape) {
			DuckDB db(nullptr);
			Connection con(db);
			auto &buffer_manager = BufferManager::GetBufferManager(*con.context);
			const auto is_struct = shape == "struct child" || shape == "null struct row";
			const auto is_constant = shape == "constant NULL";
			const auto has_dictionary = shape.find("dictionary") != string::npos;
			const auto has_append_selection = shape.find("append selection") != string::npos;
			const auto string_type = LogicalType::VARCHAR;
			const auto column_type = is_struct ? LogicalType::STRUCT({{"s", string_type}}) : string_type;
			DataChunk chunk;
			chunk.Initialize(Allocator::DefaultAllocator(), {column_type});
			PoisonedNullStrings poisoned;
			if (is_constant) {
				chunk.data[0].Reference(Value(string_type), count_t(ROW_COUNT));
				auto constant_data = ConstantVector::GetData<string_t>(chunk.data[0]);
				REQUIRE(constant_data);
				poisoned.Poison(constant_data[0]);
			} else {
				if (is_struct) {
					auto writer = FlatVector::Writer<VectorStructType<string_t>>(chunk.data[0], ROW_COUNT);
					for (idx_t row = 0; row < ROW_COUNT; row++) {
						if (is_null_row(row) && shape == "null struct row") {
							writer.WriteNull();
							continue;
						}
						writer.WriteValue([&](VectorWriter<string_t> &child) {
							if (is_null_row(row)) {
								child.WriteNull();
							} else {
								child.WriteValue(string_t(row_string(row)));
							}
						});
					}
				} else {
					auto writer = FlatVector::Writer<string_t>(chunk.data[0], ROW_COUNT);
					for (idx_t row = 0; row < ROW_COUNT; row++) {
						if (is_null_row(row)) {
							writer.WriteNull();
						} else {
							writer.WriteValue(string_t(row_string(row)));
						}
					}
				}
				auto &strings = is_struct ? StructVector::GetEntries(chunk.data[0])[0] : chunk.data[0];
				auto string_data = FlatVector::GetDataMutable<string_t>(strings);
				for (idx_t row = 0; row < ROW_COUNT; row++) {
					if (is_null_row(row)) {
						poisoned.Poison(string_data[row]);
					}
				}
			}

			vector<idx_t> appended_rows;
			for (idx_t row = 0; row < ROW_COUNT; row++) {
				appended_rows.push_back(row);
			}
			if (has_dictionary) {
				chunk.data[0].Slice(make_selection(dictionary_rows), dictionary_rows.size());
				appended_rows = dictionary_rows;
			}
			const auto append_selection = make_selection(append_rows);
			if (has_append_selection) {
				vector<idx_t> selected_rows;
				for (auto append_row : append_rows) {
					selected_rows.push_back(appended_rows[append_row]);
				}
				appended_rows = selected_rows;
			}

			auto layout = make_shared_ptr<TupleDataLayout>();
			layout->Initialize({column_type}, TupleDataValidityType::CAN_HAVE_NULL_VALUES);
			TupleDataCollection collection(buffer_manager, layout, MemoryTag::ORDER_BY);
			TupleDataAppendState append_state;
			collection.InitializeAppend(append_state, TupleDataPinProperties::KEEP_EVERYTHING_PINNED);
			if (has_append_selection) {
				collection.Append(append_state, chunk, append_selection, append_rows.size());
			} else {
				collection.Append(append_state, chunk);
			}
			collection.FinalizePinState(append_state.pin_state);
			REQUIRE(collection.Count() == appended_rows.size());

			DataChunk result;
			result.Initialize(Allocator::DefaultAllocator(), {column_type});
			TupleDataScanState scan_state;
			collection.InitializeScan(scan_state);
			REQUIRE(collection.Scan(scan_state, result));
			REQUIRE(result.size() == appended_rows.size());
			for (idx_t i = 0; i < appended_rows.size(); i++) {
				const auto row = appended_rows[i];
				Value expected(column_type);
				if (!is_constant && (!is_null_row(row) || shape == "struct child")) {
					const auto string_value = is_null_row(row) ? Value(string_type) : Value(row_string(row));
					expected = is_struct ? Value::STRUCT({{"s", string_value}}) : string_value;
				}
				REQUIRE(Value::NotDistinctFrom(result.GetValue(0, i), expected));
			}
		}
	}
}
} // namespace duckdb
