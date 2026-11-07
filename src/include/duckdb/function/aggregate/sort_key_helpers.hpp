//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/aggregate/sort_key_helpers.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/owning_string_map.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/function/create_sort_key.hpp"

namespace duckdb {

struct AggregateSortKeyHelpers {
	template <class STATE, class OP, OrderType ORDER_TYPE = OrderType::ASCENDING, bool IGNORE_NULLS = true,
	          SortKeyEncoding ENCODING = SortKeyEncoding::SORT_KEY>
	static void UnaryUpdate(Vector inputs[], AggregateInputData &input_data, idx_t input_count, Vector &state_vector,
	                        idx_t count) {
		D_ASSERT(input_count == 1);
		auto &input = inputs[0];

		Vector sort_key(LogicalType::BLOB);
		auto modifiers = OrderModifiers(ORDER_TYPE, OrderByNullType::NULLS_LAST);
		CreateSortKeyHelpers::CreateSortKey(input, input.size(), modifiers, sort_key, ENCODING);

		optional<VectorValidityIterator> input_validity;
		if (IGNORE_NULLS) {
			input_validity = input.Validity();
		}

		UnifiedVectorFormat kdata;
		sort_key.ToUnifiedFormat(kdata);

		UnifiedVectorFormat sdata;
		state_vector.ToUnifiedFormat(sdata);

		auto key_data = UnifiedVectorFormat::GetData<string_t>(kdata);
		auto states = UnifiedVectorFormat::GetData<STATE *>(sdata);
		for (idx_t i = 0; i < count; i++) {
			const auto sidx = sdata.sel->get_index(i);
			if (IGNORE_NULLS) {
				if (!input_validity.value().IsValid(i)) {
					continue;
				}
			}
			const auto key_idx = kdata.sel->get_index(i);
			auto &state = *states[sidx];
			OP::template Execute<string_t, STATE, OP>(state, key_data[key_idx], input_data);
		}
	}
};

//! Maps over SORT_KEY_WITH_VALUE keys: values that compare equal share an entry, which keeps the first one inserted.
template <class T>
using SortKeyWithValueMap =
    OwningStringMap<T, unordered_map<string_t, T, SortKeyWithValueHash, SortKeyWithValueEquality>>;
template <class T>
using OrderedSortKeyWithValueMap = OwningStringMap<T, map<string_t, T, SortKeyWithValueLess>>;

} // namespace duckdb
