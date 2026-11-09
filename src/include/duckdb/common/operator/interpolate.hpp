//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/operator/interpolate.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

//! A position held exactly as numerator * 2^exponent / denominator, with a sign
struct ExactPosition {
	bool negative = false;
	uhugeint_t numerator {0, 0};
	uhugeint_t denominator {0, 1};
	int64_t exponent = 0;

	//! The exact value of a finite double
	static ExactPosition FromDouble(double position);
};

struct ExactDifference {
	uhugeint_t magnitude;
	bool negative;
};

//! Linear interpolation of integer backed values that rounds only the result. Values are handled as their distance
//! from the minimum of their type, which fits a uhugeint_t for every integer type, so no difference of two overflows.
struct ExactInterpolation {
	template <typename T>
	static uhugeint_t ToOffset(T value) {
		return uhugeint_t(0, static_cast<uint64_t>(value) - static_cast<uint64_t>(NumericLimits<T>::Minimum()));
	}

	template <typename T>
	static bool TryFromOffset(const uhugeint_t &offset, T &result) {
		if (offset.upper != 0 || offset.lower > ToOffset(NumericLimits<T>::Maximum()).lower) {
			return false;
		}
		result = static_cast<T>(offset.lower + static_cast<uint64_t>(NumericLimits<T>::Minimum()));
		return true;
	}

	static ExactDifference Subtract(const uhugeint_t &lhs, const uhugeint_t &rhs) {
		if (lhs.upper > rhs.upper || (lhs.upper == rhs.upper && lhs.lower >= rhs.lower)) {
			return {uhugeint_t(lhs.upper - rhs.upper - (lhs.lower < rhs.lower), lhs.lower - rhs.lower), false};
		}
		return {uhugeint_t(rhs.upper - lhs.upper - (rhs.lower < lhs.lower), rhs.lower - lhs.lower), true};
	}

	//! y0 + (y1 - y0) * position rounded once to the nearest integer, ties to even; false when it leaves [0, 2^128)
	static bool TryInterpolate(const uhugeint_t &y0, const uhugeint_t &y1, const ExactPosition &position,
	                           uhugeint_t &result);

	//! y0 + (y1 - y0) * position for a position in [0, 1), rounded once to the nearest value, ties to even
	template <typename T>
	static T Interpolate(const T &y0, const double position, const T &y1) {
		D_ASSERT(position >= 0 && position < 1);
		uhugeint_t offset;
		T result;
		if (!TryInterpolate(ToOffset(y0), ToOffset(y1), ExactPosition::FromDouble(position), offset) ||
		    !TryFromOffset(offset, result)) {
			throw InternalException("Interpolation left the range of its values");
		}
		return result;
	}

	//! The DOUBLE nearest to y0 + (y1 - y0) * position for a position in [0, 1), ties to even
	template <typename T>
	static double InterpolateToDouble(const T &y0, const double position, const T &y1) {
		return InterpolateOffsetsToDouble(ToOffset(y0), position, ToOffset(y1), ToOffset(T(0)));
	}

	//! InterpolateToDouble on offsets, where zero is the offset of the value 0
	static double InterpolateOffsetsToDouble(const uhugeint_t &y0, double position, const uhugeint_t &y1,
	                                         const uhugeint_t &zero);
};

template <>
inline uhugeint_t ExactInterpolation::ToOffset(hugeint_t value) {
	return uhugeint_t(static_cast<uint64_t>(value.upper) ^ (uint64_t(1) << 63), value.lower);
}

template <>
inline uhugeint_t ExactInterpolation::ToOffset(uhugeint_t value) {
	return value;
}

template <>
inline bool ExactInterpolation::TryFromOffset(const uhugeint_t &offset, hugeint_t &result) {
	result = hugeint_t(static_cast<int64_t>(offset.upper ^ (uint64_t(1) << 63)), offset.lower);
	return true;
}

template <>
inline bool ExactInterpolation::TryFromOffset(const uhugeint_t &offset, uhugeint_t &result) {
	result = offset;
	return true;
}

//	Linear interpolation between two values
struct InterpolateOperator {
	template <typename TARGET_TYPE>
	static inline TARGET_TYPE Operation(const TARGET_TYPE &lo, const double d, const TARGET_TYPE &hi) {
		if constexpr (NumericLimits<TARGET_TYPE>::IsIntegral()) {
			return ExactInterpolation::Interpolate(lo, d, hi);
		} else {
			const auto delta = static_cast<double>(hi) - static_cast<double>(lo);
			return LossyNumericCast<TARGET_TYPE>(static_cast<double>(lo) + delta * d);
		}
	}
};

template <>
double InterpolateOperator::Operation(const double &lo, const double d, const double &hi);
template <>
dtime_t InterpolateOperator::Operation(const dtime_t &lo, const double d, const dtime_t &hi);
template <>
dtime_tz_t InterpolateOperator::Operation(const dtime_tz_t &lo, const double d, const dtime_tz_t &hi);
template <>
timestamp_t InterpolateOperator::Operation(const timestamp_t &lo, const double d, const timestamp_t &hi);
template <>
interval_t InterpolateOperator::Operation(const interval_t &lo, const double d, const interval_t &hi);

} // namespace duckdb
