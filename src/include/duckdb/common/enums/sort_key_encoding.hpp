//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/sort_key_encoding.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class SortKeyEncoding : uint8_t {
	//! -0.0 and 0.0 share one key, which decodes as 0.0.
	SORT_KEY = 0,
	//! -0.0 has its own key just below 0.0 and decodes as -0.0, for states that store a value without comparing it.
	KEEP_ZERO_SIGN = 1,
	//! The SORT_KEY key followed by the KEEP_ZERO_SIGN key of the same value, both of one length, for states that order
	//! or group by the first and return the second.
	SORT_KEY_WITH_VALUE = 2
};

} // namespace duckdb
