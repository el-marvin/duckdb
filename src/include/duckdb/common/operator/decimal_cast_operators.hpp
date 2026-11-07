//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/operator/decimal_cast_operators.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/operator/integer_cast_operator.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Decimal Casts
//===--------------------------------------------------------------------===//
struct TryCastToDecimal {
	template <class SRC, class DST>
	static inline bool Operation(SRC input, DST &result, CastParameters &parameters, uint32_t width, uint32_t scale) {
		NotImplementedException::Throw("Unimplemented type for TryCastToDecimal!");
	}
};

struct TryCastToDecimalCommaSeparated {
	template <class SRC, class DST>
	static inline bool Operation(SRC input, DST &result, CastParameters &parameters, uint32_t width, uint32_t scale) {
		NotImplementedException::Throw("Unimplemented type for TryCastToDecimal!");
	}
};

struct TryCastFromDecimal {
	template <class SRC, class DST>
	static inline bool Operation(SRC input, DST &result, CastParameters &parameters, uint32_t width, uint32_t scale) {
		NotImplementedException::Throw("Unimplemented type for TryCastFromDecimal!");
	}
};

//===--------------------------------------------------------------------===//
// Cast Decimal <-> bool
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(bool input, int16_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(bool input, int32_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(bool input, int64_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(bool input, hugeint_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, bool &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, bool &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, bool &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, bool &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> int8_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int8_t input, int16_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int8_t input, int32_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int8_t input, int64_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int8_t input, hugeint_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, int8_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, int8_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, int8_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, int8_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> int16_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int16_t input, int16_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int16_t input, int32_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int16_t input, int64_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int16_t input, hugeint_t &result, CastParameters &parameters,
                                            uint32_t width, uint32_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, int16_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, int16_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, int16_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, int16_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> int32_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int32_t input, int16_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int32_t input, int32_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int32_t input, int64_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int32_t input, hugeint_t &result, CastParameters &parameters,
                                            uint32_t width, uint32_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, int32_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, int32_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, int32_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, int32_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> int64_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int64_t input, int16_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int64_t input, int32_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int64_t input, int64_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int64_t input, hugeint_t &result, CastParameters &parameters,
                                            uint32_t width, uint32_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, int64_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, int64_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, int64_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, int64_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> hugeint_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(hugeint_t input, int16_t &result, CastParameters &parameters,
                                            uint32_t width, uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(hugeint_t input, int32_t &result, CastParameters &parameters,
                                            uint32_t width, uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(hugeint_t input, int64_t &result, CastParameters &parameters,
                                            uint32_t width, uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(hugeint_t input, hugeint_t &result, CastParameters &parameters,
                                            uint32_t width, uint32_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, hugeint_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, hugeint_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, hugeint_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, hugeint_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> uhugeint_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uhugeint_t input, int16_t &result, CastParameters &parameters,
                                            uint32_t width, uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uhugeint_t input, int32_t &result, CastParameters &parameters,
                                            uint32_t width, uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uhugeint_t input, int64_t &result, CastParameters &parameters,
                                            uint32_t width, uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uhugeint_t input, hugeint_t &result, CastParameters &parameters,
                                            uint32_t width, uint32_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, uhugeint_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, uhugeint_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, uhugeint_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, uhugeint_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> uint8_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint8_t input, int16_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint8_t input, int32_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint8_t input, int64_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint8_t input, hugeint_t &result, CastParameters &parameters,
                                            uint32_t width, uint32_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, uint8_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, uint8_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, uint8_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, uint8_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> uint16_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint16_t input, int16_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint16_t input, int32_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint16_t input, int64_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint16_t input, hugeint_t &result, CastParameters &parameters,
                                            uint32_t width, uint32_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, uint16_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, uint16_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, uint16_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, uint16_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> uint32_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint32_t input, int16_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint32_t input, int32_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint32_t input, int64_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint32_t input, hugeint_t &result, CastParameters &parameters,
                                            uint32_t width, uint32_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, uint32_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, uint32_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, uint32_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, uint32_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> uint64_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint64_t input, int16_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint64_t input, int32_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint64_t input, int64_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint64_t input, hugeint_t &result, CastParameters &parameters,
                                            uint32_t width, uint32_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, uint64_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, uint64_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, uint64_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, uint64_t &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> float
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(float input, int16_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(float input, int32_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(float input, int64_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(float input, hugeint_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, float &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, float &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, float &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, float &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> double
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(double input, int16_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(double input, int32_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(double input, int64_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(double input, hugeint_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, double &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, double &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, double &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, double &result, CastParameters &parameters, uint32_t width,
                                   uint32_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> VARCHAR
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(string_t input, int16_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(string_t input, int32_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(string_t input, int64_t &result, CastParameters &parameters, uint32_t width,
                                            uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(string_t input, hugeint_t &result, CastParameters &parameters,
                                            uint32_t width, uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimalCommaSeparated::Operation(string_t input, int16_t &result, CastParameters &parameters,
                                                          uint32_t width, uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimalCommaSeparated::Operation(string_t input, int32_t &result, CastParameters &parameters,
                                                          uint32_t width, uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimalCommaSeparated::Operation(string_t input, int64_t &result, CastParameters &parameters,
                                                          uint32_t width, uint32_t scale);
template <>
DUCKDB_API bool TryCastToDecimalCommaSeparated::Operation(string_t input, hugeint_t &result, CastParameters &parameters,
                                                          uint32_t width, uint32_t scale);

struct StringCastFromDecimal {
	template <class SRC>
	static inline string_t Operation(SRC input, uint32_t width, uint32_t scale, StringHeap &heap) {
		NotImplementedException::Throw("Unimplemented type for string cast!");
	}
};

template <>
string_t StringCastFromDecimal::Operation(int16_t input, uint32_t width, uint32_t scale, StringHeap &heap);
template <>
string_t StringCastFromDecimal::Operation(int32_t input, uint32_t width, uint32_t scale, StringHeap &heap);
template <>
string_t StringCastFromDecimal::Operation(int64_t input, uint32_t width, uint32_t scale, StringHeap &heap);
template <>
string_t StringCastFromDecimal::Operation(hugeint_t input, uint32_t width, uint32_t scale, StringHeap &heap);

//===--------------------------------------------------------------------===//
// Cast VARCHAR <-> Decimal
//===--------------------------------------------------------------------===//
template <typename T>
struct DecimalCastTraits {
	using POWERS_OF_TEN_CLASS = NumericHelper;
};

template <>
struct DecimalCastTraits<hugeint_t> {
	using POWERS_OF_TEN_CLASS = Hugeint;
};

template <>
struct DecimalCastTraits<uhugeint_t> {
	using POWERS_OF_TEN_CLASS = Uhugeint;
};

//! The text is read as result * 10^exponent, so the width and scale apply once the exponent is known.
template <class T>
struct DecimalCastData {
	using StoreType = T;

	//! Not a constructor, so the struct stays trivial for the memset TryIntegerCast compiles
	void Initialize(uint32_t width_p, uint32_t scale_p) {
		D_ASSERT(width_p <= DecimalWidth<T>::max);
		result = 0;
		width = width_p;
		scale = scale_p;
		digit_count = 0;
		exponent = 0;
		has_dropped_digit = false;
		round_dropped_digit = false;
		limit = UnsafeNumericCast<T>(DecimalCastTraits<T>::POWERS_OF_TEN_CLASS::POWERS_OF_TEN[width_p]);
	}

	//! Holds at most DecimalWidth<StoreType>::max digits, so neither scaling to the width nor rounding overflows
	StoreType result;
	uint32_t width;
	uint32_t scale;
	uint32_t digit_count;
	int64_t exponent;
	//! Only the first significant digit that does not fit in result can change the rounding
	bool has_dropped_digit;
	bool round_dropped_digit;
	StoreType limit;
};

struct DecimalCastOperation {
	template <class T, bool NEGATIVE>
	static bool HandleDigit(T &state, uint8_t digit) {
		if (!AddDigit<T, NEGATIVE>(state, digit)) {
			state.exponent += 1;
		}
		return true;
	}

	template <class T, bool NEGATIVE>
	static bool HandleHexDigit(T &state, uint8_t digit) {
		return false;
	}

	template <class T, bool NEGATIVE>
	static bool HandleBinaryDigit(T &state, uint8_t digit) {
		return false;
	}

	template <class T, bool NEGATIVE, bool ALLOW_EXPONENT>
	static bool HandleDecimal(T &state, uint8_t digit) {
		if (AddDigit<T, NEGATIVE>(state, digit)) {
			state.exponent -= 1;
		}
		return true;
	}

	template <class T, bool NEGATIVE>
	static bool HandleExponent(T &state, int32_t exponent) {
		state.exponent += exponent;
		return Finalize<T, NEGATIVE>(state);
	}

	template <class T, bool NEGATIVE>
	static bool Finalize(T &state) {
		using STORE_TYPE = typename T::StoreType;
		using POWERS_OF_TEN_CLASS = typename DecimalCastTraits<STORE_TYPE>::POWERS_OF_TEN_CLASS;
		if (state.result == 0) {
			return true;
		}
		const int64_t shift = state.exponent + int64_t(state.scale);
		bool round_up;
		if (shift >= 0) {
			if (int64_t(state.digit_count) + shift > int64_t(state.width)) {
				return false;
			}
			state.result *= UnsafeNumericCast<STORE_TYPE>(POWERS_OF_TEN_CLASS::POWERS_OF_TEN[shift]);
			// a dropped digit passes the width check only at shift zero, where it is the first digit past the scale
			round_up = state.round_dropped_digit;
		} else {
			const auto removed = uint64_t(-shift);
			if (removed > state.digit_count) {
				state.result = 0;
				return true;
			}
			const auto divisor = UnsafeNumericCast<STORE_TYPE>(POWERS_OF_TEN_CLASS::POWERS_OF_TEN[removed]);
			const auto quotient = state.result / divisor;
			const auto remainder = state.result - quotient * divisor;
			state.result = quotient;
			// compares twice the remainder with the divisor without forming it, which overflows at 38 digits
			round_up = NEGATIVE ? remainder <= -divisor - remainder : remainder >= divisor - remainder;
		}
		// added as a value because a branch on the removed digit is mispredicted half the time
		if (NEGATIVE) {
			state.result -= STORE_TYPE(round_up);
		} else {
			state.result += STORE_TYPE(round_up);
		}
		if (NEGATIVE) {
			return state.result > -state.limit;
		} else {
			return state.result < state.limit;
		}
	}

private:
	//! Returns false for a significant digit that does not fit in result; a leading zero is not stored
	template <class T, bool NEGATIVE>
	static bool AddDigit(T &state, uint8_t digit) {
		if (state.result == 0 && digit == 0) {
			return true;
		}
		if (state.digit_count == DecimalWidth<typename T::StoreType>::max) {
			if (!state.has_dropped_digit) {
				state.has_dropped_digit = true;
				state.round_dropped_digit = digit >= 5;
			}
			return false;
		}
		state.digit_count++;
		if (NEGATIVE) {
			state.result = state.result * 10 - digit;
		} else {
			state.result = state.result * 10 + digit;
		}
		return true;
	}
};

template <class T, char decimal_separator = '.'>
bool TryDecimalStringCast(string_t input, T &result, CastParameters &parameters, uint32_t width, uint32_t scale) {
	return TryDecimalStringCast<T, decimal_separator>(input.GetData(), input.GetSize(), result, parameters, width,
	                                                  scale);
}

template <class T, char decimal_separator = '.'>
bool TryDecimalStringCast(const char *string_ptr, idx_t string_size, T &result, CastParameters &parameters,
                          uint32_t width, uint32_t scale) {
	DecimalCastData<T> state;
	state.Initialize(width, scale);
	if (!TryIntegerCast<DecimalCastData<T>, true, true, DecimalCastOperation, false, decimal_separator>(
	        string_ptr, string_size, state, false)) {
		string_t value(string_ptr, (uint32_t)string_size);
		string error = StringUtil::Format("Could not convert string \"%s\" to DECIMAL(%d,%d)", value.GetString(),
		                                  (int)width, (int)scale);
		HandleCastError::AssignError(error, parameters);
		return false;
	}
	result = state.result;
	return true;
}

template <class T, char decimal_separator = '.'>
bool TryDecimalStringCast(const char *string_ptr, idx_t string_size, T &result, uint32_t width, uint32_t scale) {
	DecimalCastData<T> state;
	state.Initialize(width, scale);
	if (!TryIntegerCast<DecimalCastData<T>, true, true, DecimalCastOperation, false, decimal_separator>(
	        string_ptr, string_size, state, false)) {
		return false;
	}
	result = state.result;
	return true;
}

} // namespace duckdb
