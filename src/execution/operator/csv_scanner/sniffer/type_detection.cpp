#include "duckdb/common/algorithm.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/operator/double_cast_operator.hpp"
#include "duckdb/common/operator/integer_cast_operator.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"

namespace duckdb {
struct TryCastFloatingOperator {
	template <class OP, class T>
	static bool Operation(string_t input) {
		T result;
		string error_message;
		CastParameters parameters(false, &error_message);
		return OP::Operation(input, result, parameters);
	}
};

static bool StartsWithNumericDate(string &separator, const string_t &value) {
	auto begin = value.GetData();
	auto end = begin + value.GetSize();

	//	StrpTimeFormat::Parse will skip whitespace, so we can too
	auto field1 = std::find_if_not(begin, end, StringUtil::CharacterIsSpace);
	if (field1 == end) {
		return false;
	}

	//	first numeric field must start immediately
	if (!StringUtil::CharacterIsDigit(*field1)) {
		return false;
	}
	auto literal1 = std::find_if_not(field1, end, StringUtil::CharacterIsDigit);
	if (literal1 == end) {
		return false;
	}

	//	second numeric field must exist
	auto field2 = std::find_if(literal1, end, StringUtil::CharacterIsDigit);
	if (field2 == end) {
		return false;
	}
	auto literal2 = std::find_if_not(field2, end, StringUtil::CharacterIsDigit);
	if (literal2 == end) {
		return false;
	}

	//	third numeric field must exist
	auto field3 = std::find_if(literal2, end, StringUtil::CharacterIsDigit);
	if (field3 == end) {
		return false;
	}

	//	second literal must match first
	if (((field3 - literal2) != (field2 - literal1)) ||
	    strncmp(literal1, literal2, NumericCast<size_t>((field2 - literal1))) != 0) {
		return false;
	}

	//	copy the literal as the separator, escaping percent signs
	separator.clear();
	while (literal1 < field2) {
		const auto literal_char = *literal1++;
		if (literal_char == '%') {
			separator.push_back(literal_char);
		}
		separator.push_back(literal_char);
	}

	return true;
}

string GenerateDateFormat(const string &separator, const char *format_template) {
	string format_specifier = format_template;
	const auto amount_of_dashes = NumericCast<idx_t>(std::count(format_specifier.begin(), format_specifier.end(), '-'));
	// All our date formats must have at least one -
	D_ASSERT(amount_of_dashes);
	string result;
	result.reserve(format_specifier.size() - amount_of_dashes + (amount_of_dashes * separator.size()));
	for (const auto &character : format_specifier) {
		if (character == '-') {
			result += separator;
		} else {
			result += character;
		}
	}
	return result;
}

idx_t CSVSniffer::LinesSniffed() const {
	return lines_sniffed;
}

bool CSVSniffer::EmptyOrOnlyHeader() const {
	return (single_row_file && best_candidate->state_machine->dialect_options.header.GetValue()) || lines_sniffed == 0;
}

bool CSVSniffer::CanYouCastBignum(const char *value_ptr, idx_t value_size) {
	while (value_size > 0 && StringUtil::CharacterIsSpace(*value_ptr)) {
		value_ptr++;
		value_size--;
	}
	if (value_size == 0 || *value_ptr == '+') {
		return false;
	}
	auto trimmed_size = value_size;
	while (trimmed_size > 0 && StringUtil::CharacterIsSpace(value_ptr[trimmed_size - 1])) {
		trimmed_size--;
	}
	if (trimmed_size == 0) {
		return false;
	}
	idx_t digit_pos = value_ptr[0] == '-' ? 1 : 0;
	if (digit_pos == trimmed_size) {
		return false;
	}
	if (!StringUtil::CharacterIsDigit(value_ptr[digit_pos])) {
		return false;
	}
	if (digit_pos + 1 < trimmed_size && value_ptr[digit_pos] == '0' &&
	    StringUtil::CharacterIsDigit(value_ptr[digit_pos + 1])) {
		return false;
	}
	for (idx_t pos = digit_pos + 1; pos < trimmed_size; pos++) {
		if (!StringUtil::CharacterIsDigit(value_ptr[pos])) {
			return false;
		}
	}
	try {
		Bignum::VarcharToBignum(string_t(value_ptr, NumericCast<uint32_t>(trimmed_size)));
		return true;
	} catch (const ConversionException &) {
		return false;
	}
}

bool CSVSniffer::CanYouCastIt(ClientContext &context, const string_t value, const LogicalType &type,
                              const DialectOptions &dialect_options, const bool is_null, const char decimal_separator,
                              const char thousands_separator) {
	if (is_null) {
		return true;
	}
	auto value_ptr = value.GetData();
	auto value_size = value.GetSize();
	string strip_thousands;
	if ((type.IsNumeric() || type.id() == LogicalTypeId::BIGNUM) && thousands_separator != '\0') {
		// If we have a thousands separator we should try to use that
		strip_thousands = BaseScanner::RemoveSeparator(value_ptr, value_size, thousands_separator);
		value_ptr = strip_thousands.c_str();
		value_size = strip_thousands.size();
	}
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN: {
		bool dummy_value;
		return TryCastStringBool(value_ptr, value_size, dummy_value, true);
	}
	case LogicalTypeId::TINYINT: {
		int8_t dummy_value;
		return TrySimpleIntegerCast(value_ptr, value_size, dummy_value, false);
	}
	case LogicalTypeId::SMALLINT: {
		int16_t dummy_value;
		return TrySimpleIntegerCast(value_ptr, value_size, dummy_value, true);
	}
	case LogicalTypeId::INTEGER: {
		int32_t dummy_value;
		return TrySimpleIntegerCast(value_ptr, value_size, dummy_value, true);
	}
	case LogicalTypeId::BIGINT: {
		int64_t dummy_value;
		return TrySimpleIntegerCast(value_ptr, value_size, dummy_value, true);
	}
	case LogicalTypeId::HUGEINT: {
		hugeint_t dummy_value;
		return TryCast::Operation<string_t, hugeint_t>(string_t(value_ptr, NumericCast<uint32_t>(value_size)),
		                                               dummy_value, true);
	}
	case LogicalTypeId::UTINYINT: {
		uint8_t dummy_value;
		return TrySimpleIntegerCast(value_ptr, value_size, dummy_value, true);
	}
	case LogicalTypeId::USMALLINT: {
		uint16_t dummy_value;
		return TrySimpleIntegerCast(value_ptr, value_size, dummy_value, true);
	}
	case LogicalTypeId::UINTEGER: {
		uint32_t dummy_value;
		return TrySimpleIntegerCast(value_ptr, value_size, dummy_value, true);
	}
	case LogicalTypeId::UBIGINT: {
		uint64_t dummy_value;
		return TrySimpleIntegerCast(value_ptr, value_size, dummy_value, true);
	}
	case LogicalTypeId::UHUGEINT: {
		uhugeint_t dummy_value;
		return TryCast::Operation<string_t, uhugeint_t>(string_t(value_ptr, NumericCast<uint32_t>(value_size)),
		                                                dummy_value, true);
	}
	case LogicalTypeId::BIGNUM:
		return CanYouCastBignum(value_ptr, value_size);
	case LogicalTypeId::DOUBLE: {
		double dummy_value;
		return TryDoubleCast<double>(value_ptr, value_size, dummy_value, true, decimal_separator);
	}
	case LogicalTypeId::FLOAT: {
		float dummy_value;
		return TryDoubleCast<float>(value_ptr, value_size, dummy_value, true, decimal_separator);
	}
	case LogicalTypeId::DATE: {
		if (!dialect_options.date_format.find(LogicalTypeId::DATE)->second.GetValue().Empty()) {
			date_t result;
			string error_message;
			return dialect_options.date_format.find(LogicalTypeId::DATE)
			    ->second.GetValue()
			    .TryParseDate(value, result, error_message);
		}
		idx_t pos;
		bool special;
		date_t dummy_value;
		return Date::TryConvertDate(value_ptr, value_size, pos, dummy_value, special, true) == DateCastResult::SUCCESS;
	}
	case LogicalTypeId::TIMESTAMP: {
		timestamp_t dummy_value;
		if (!dialect_options.date_format.find(LogicalTypeId::TIMESTAMP)->second.GetValue().Empty()) {
			string error_message;
			return dialect_options.date_format.find(LogicalTypeId::TIMESTAMP)
			    ->second.GetValue()
			    .TryParseTimestamp(value, dummy_value, error_message);
		}
		return Timestamp::TryConvertTimestamp(value_ptr, value_size, dummy_value, false, nullptr, true) ==
		       TimestampCastResult::SUCCESS;
	}
	case LogicalTypeId::TIME: {
		idx_t pos;
		dtime_t dummy_value;
		return Time::TryConvertTime(value_ptr, value_size, pos, dummy_value, true);
	}
	case LogicalTypeId::DECIMAL: {
		uint32_t width, scale;
		type.GetDecimalProperties(width, scale);
		if (decimal_separator == ',') {
			switch (type.InternalType()) {
			case PhysicalType::INT16: {
				int16_t dummy_value;
				return TryDecimalStringCast<int16_t, ','>(value_ptr, value_size, dummy_value, width, scale);
			}

			case PhysicalType::INT32: {
				int32_t dummy_value;
				return TryDecimalStringCast<int32_t, ','>(value_ptr, value_size, dummy_value, width, scale);
			}

			case PhysicalType::INT64: {
				int64_t dummy_value;
				return TryDecimalStringCast<int64_t, ','>(value_ptr, value_size, dummy_value, width, scale);
			}

			case PhysicalType::INT128: {
				hugeint_t dummy_value;
				return TryDecimalStringCast<hugeint_t, ','>(value_ptr, value_size, dummy_value, width, scale);
			}

			default:
				throw InternalException("Invalid Physical Type for Decimal Value. Physical Type: " +
				                        TypeIdToString(type.InternalType()));
			}

		} else if (decimal_separator == '.') {
			switch (type.InternalType()) {
			case PhysicalType::INT16: {
				int16_t dummy_value;
				return TryDecimalStringCast(value_ptr, value_size, dummy_value, width, scale);
			}

			case PhysicalType::INT32: {
				int32_t dummy_value;
				return TryDecimalStringCast(value_ptr, value_size, dummy_value, width, scale);
			}

			case PhysicalType::INT64: {
				int64_t dummy_value;
				return TryDecimalStringCast(value_ptr, value_size, dummy_value, width, scale);
			}

			case PhysicalType::INT128: {
				hugeint_t dummy_value;
				return TryDecimalStringCast(value_ptr, value_size, dummy_value, width, scale);
			}

			default:
				throw InternalException("Invalid Physical Type for Decimal Value. Physical Type: " +
				                        TypeIdToString(type.InternalType()));
			}
		}
		throw InvalidInputException("Decimals can only have ',' and '.' as decimal separators");
	}
	case LogicalTypeId::VARCHAR:
		return true;
	default: {
		// We do Value Try Cast for non-basic types.
		string error_message;
		Value str_value(value);
		auto new_value = str_value.TryCastAs(context, type, &error_message, true);
		return new_value.has_value() && error_message.empty();
	}
	}
}

void CSVSniffer::InitializeDateAndTimeStampDetection(DateTimestampSniffing &format_candidate, const string &separator,
                                                     const LogicalType &sql_type) const {
	format_candidate.initialized = true;
	auto entry = format_template_candidates.find(sql_type.id());
	if (entry == format_template_candidates.end()) {
		return;
	}
	for (const auto &t : entry->second) {
		const auto format_string = GenerateDateFormat(separator, t);
		// ISO 8601 is left to the plain cast
		if (format_string.find("%Y-%m-%d") == string::npos) {
			StrpTimeFormat format;
			StrTimeFormat::ParseFormatSpecifier(format_string, format);
			format_candidate.format.push_back(std::move(format));
		}
	}
}

static bool FormatReadsValue(const StrpTimeFormat &format, const string_t &value, const LogicalTypeId sql_type) {
	StrpTimeFormat::ParseResult result;
	if (!format.Parse(value, result, true)) {
		return false;
	}
	if (sql_type == LogicalTypeId::DATE) {
		date_t date;
		return result.TryToDate(date);
	}
	timestamp_t timestamp;
	return result.TryToTimestamp(timestamp);
}

//! Returns whether the list changed
static bool NarrowFormatCandidates(DateTimestampSniffing &format_candidate, const string_t &value,
                                   const LogicalTypeId sql_type) {
	auto &formats = format_candidate.format;
	if (formats.empty()) {
		return false;
	}
	// A special value such as 'infinity' parses under every format, so it tells no format apart
	StrpTimeFormat::ParseResult special_check;
	if (formats.back().Parse(value, special_check, true) && special_check.is_special) {
		return false;
	}
	const auto reads_value = [&](const StrpTimeFormat &format) {
		return FormatReadsValue(format, value, sql_type);
	};
	if (std::none_of(formats.begin(), formats.end(), reads_value)) {
		if (format_candidate.had_match) {
			return false;
		}
		// No candidate read the values of the column so far, so only the plain cast can read the column
		formats.clear();
		return true;
	}
	format_candidate.had_match = true;
	const auto old_size = formats.size();
	formats.erase(std::remove_if(formats.begin(), formats.end(),
	                             [&](const StrpTimeFormat &format) { return !reads_value(format); }),
	              formats.end());
	return formats.size() != old_size;
}

bool ValidSeparator(const string &separator) {
	// We use https://en.wikipedia.org/wiki/List_of_date_formats_by_country as reference
	return separator == "-" || separator == "." || separator == "/" || separator == " ";
}
void CSVSniffer::DetectDateAndTimeStampFormats(CSVStateMachine &candidate, DateTimestampSniffing &format_candidate,
                                               const LogicalType &sql_type, const VectorIterator<string_t> &values,
                                               idx_t first_row, idx_t row_idx) const {
	if (format_candidate.fixed) {
		return;
	}
	const auto &value = values[row_idx].GetValue();
	bool changed = false;
	if (!format_candidate.initialized) {
		// The templates take their separator from the first value that starts like a numeric date
		string separator;
		if (!StartsWithNumericDate(separator, value) || !ValidSeparator(separator)) {
			return;
		}
		InitializeDateAndTimeStampDetection(format_candidate, separator, sql_type);
		changed = true;
		// The plain cast read the earlier values of the column, and a format must read them too
		for (idx_t earlier_row = first_row; earlier_row < row_idx; earlier_row++) {
			const auto earlier_value = values[earlier_row];
			if (earlier_value.IsValid()) {
				NarrowFormatCandidates(format_candidate, earlier_value.GetValue(), sql_type.id());
			}
		}
	}
	changed = NarrowFormatCandidates(format_candidate, value, sql_type.id()) || changed;
	if (!changed) {
		return;
	}
	candidate.dialect_options.date_format[sql_type.id()].Set(CurrentFormat(format_candidate), false);
}

StrpTimeFormat CSVSniffer::CurrentFormat(const DateTimestampSniffing &format_candidate) {
	if (format_candidate.format.empty()) {
		// the plain cast
		return StrpTimeFormat();
	}
	return format_candidate.format.back();
}

ColumnFormatCandidates CSVSniffer::StartingFormatCandidates(bool fix_to_options) const {
	ColumnFormatCandidates result;
	for (const auto type : {LogicalTypeId::DATE, LogicalTypeId::TIMESTAMP}) {
		auto &format_candidate = result[type];
		const auto &option = options.dialect_options.date_format.at(type);
		if (option.IsSetByUser() || fix_to_options) {
			format_candidate.fixed = true;
			format_candidate.initialized = true;
			if (!option.GetValue().Empty()) {
				format_candidate.format.push_back(option.GetValue());
			}
		}
	}
	return result;
}

void CSVSniffer::SetColumnFormats(CSVStateMachine &candidate, const ColumnFormatCandidates &column_formats) {
	for (auto &entry : column_formats) {
		candidate.dialect_options.date_format[entry.first].Set(CurrentFormat(entry.second), false);
	}
}

void CSVSniffer::SetUpFormatsFromChunk(DateTimestampSniffing &format_candidate, Vector &values, idx_t size,
                                       const LogicalType &sql_type) const {
	if (format_candidate.fixed || format_candidate.initialized || format_candidate.read_value) {
		return;
	}
	const auto column_values = values.Values<string_t>();
	for (idx_t row_idx = 0; row_idx < size; row_idx++) {
		const auto value = column_values[row_idx];
		string separator;
		if (value.IsValid() && StartsWithNumericDate(separator, value.GetValue()) && ValidSeparator(separator)) {
			InitializeDateAndTimeStampDetection(format_candidate, separator, sql_type);
			return;
		}
	}
}

bool CSVSniffer::IsDateOrTimestamp(const LogicalType &type) {
	return type.id() == LogicalTypeId::DATE || type.id() == LogicalTypeId::TIMESTAMP;
}

bool CSVSniffer::ReadsEarlierValues(CSVStateMachine &state_machine, const VectorIterator<string_t> &values,
                                    const LogicalType &sql_type, idx_t first_row, idx_t row_idx) const {
	for (idx_t earlier_row = first_row; earlier_row < row_idx; earlier_row++) {
		const auto earlier_value = values[earlier_row];
		if (earlier_value.IsValid() &&
		    !CanYouCastIt(buffer_manager->context, earlier_value.GetValue(), sql_type, state_machine.dialect_options,
		                  false, state_machine.options.decimal_separator[0],
		                  state_machine.options.thousands_separator)) {
			return false;
		}
	}
	return true;
}

void CSVSniffer::SniffTypes(DataChunk &data_chunk, CSVStateMachine &state_machine,
                            unordered_map<idx_t, vector<LogicalType>> &info_sql_types_candidates,
                            idx_t start_idx_detection, vector<ColumnFormatCandidates> &column_formats) {
	const idx_t chunk_size = data_chunk.size();
	D_ASSERT(column_formats.size() >= data_chunk.ColumnCount());
	for (idx_t col_idx = 0; col_idx < data_chunk.ColumnCount(); col_idx++) {
		auto &cur_vector = data_chunk.data[col_idx];
		D_ASSERT(cur_vector.GetVectorType() == VectorType::FLAT_VECTOR);
		D_ASSERT(cur_vector.GetType() == LogicalType::VARCHAR);
		auto vector_data = FlatVector::GetData<string_t>(cur_vector);
		auto null_mask = FlatVector::ValidityMutable(cur_vector);
		auto &col_type_candidates = info_sql_types_candidates[col_idx];
		auto &col_formats = column_formats[col_idx];
		const auto column_values = cur_vector.Values<string_t>();
		SetColumnFormats(state_machine, col_formats);
		for (idx_t row_idx = start_idx_detection; row_idx < chunk_size; row_idx++) {
			// col_type_candidates can't be empty since anything in a CSV file should at least be a string
			// and we validate utf-8 compatibility when creating the type
			D_ASSERT(!col_type_candidates.empty());
			if (!null_mask.RowIsValid(row_idx)) {
				continue;
			}
			const auto cur_top_candidate = col_type_candidates.back();
			while (col_type_candidates.size() > 1) {
				const auto &sql_type = col_type_candidates.back();
				if (IsDateOrTimestamp(sql_type)) {
					DetectDateAndTimeStampFormats(state_machine, col_formats[sql_type.id()], sql_type, column_values,
					                              start_idx_detection, row_idx);
				}
				// A type that takes over must also read the values its predecessor read
				if (CanYouCastIt(buffer_manager->context, vector_data[row_idx], sql_type, state_machine.dialect_options,
				                 false, state_machine.options.decimal_separator[0],
				                 state_machine.options.thousands_separator) &&
				    (sql_type == cur_top_candidate ||
				     ReadsEarlierValues(state_machine, column_values, sql_type, start_idx_detection, row_idx))) {
					if (IsDateOrTimestamp(sql_type)) {
						col_formats[sql_type.id()].read_value = true;
					}
					break;
				}
				if (row_idx != start_idx_detection &&
				    (cur_top_candidate == LogicalType::BOOLEAN || cur_top_candidate == LogicalType::DATE ||
				     cur_top_candidate == LogicalType::TIME ||
				     (cur_top_candidate == LogicalType::TIMESTAMP &&
				      col_type_candidates[col_type_candidates.size() - 2] != LogicalType::TIMESTAMP_TZ) ||
				     cur_top_candidate == LogicalType::TIMESTAMP_TZ)) {
					// If we thought this was a boolean value (i.e., T,F, True, False) and it is not, we
					// immediately pop to varchar.
					col_type_candidates.erase(col_type_candidates.begin() + 1, col_type_candidates.end());
					break;
				}
				col_type_candidates.pop_back();
			}
		}
	}
}

void CSVSniffer::ChooseDateAndTimestampFormats(vector<LogicalType> &column_types, const vector<bool> &declared,
                                               optional_ptr<const vector<HeaderValue>> first_row) {
	auto &state_machine = best_candidate->GetStateMachine();
	const auto starting_formats = StartingFormatCandidates(false);
	// The first row is read with each format the way the scan reads it
	auto first_row_options = state_machine.dialect_options;
	for (const auto type : {LogicalTypeId::DATE, LogicalTypeId::TIMESTAMP}) {
		vector<StrpTimeFormat> shared;
		bool has_column = false;
		for (const bool declared_columns : {true, false}) {
			for (idx_t col_idx = 0; col_idx < column_types.size(); col_idx++) {
				const bool is_declared = col_idx < declared.size() && declared[col_idx];
				if (is_declared != declared_columns || column_types[col_idx].id() != type ||
				    col_idx >= best_format_candidates.size()) {
					continue;
				}
				auto &format_candidate = best_format_candidates[col_idx][type];
				if (!format_candidate.read_value) {
					// no sampled value tells the formats apart, so every format reads the column
					continue;
				}
				// A column the plain cast reads shares the empty format, which no template equals
				vector<StrpTimeFormat> column_formats {StrpTimeFormat()};
				if (!format_candidate.format.empty()) {
					column_formats = format_candidate.format;
				}
				if (first_row && col_idx < first_row->size() && !(*first_row)[col_idx].is_null) {
					// header detection took the first row as data, so it is a sampled value too
					const auto &first_value = (*first_row)[col_idx].value;
					const string_t value(first_value.c_str(), NumericCast<uint32_t>(first_value.size()));
					vector<StrpTimeFormat> reading_formats;
					for (auto &format : column_formats) {
						first_row_options.date_format[type].Set(format, false);
						if (CanYouCastIt(buffer_manager->context, value, LogicalType(type), first_row_options, false,
						                 options.decimal_separator[0], options.thousands_separator)) {
							reading_formats.push_back(format);
						}
					}
					// the scan fails on a declared column's value that no format reads, whichever format it gets
					if (!is_declared || !reading_formats.empty()) {
						column_formats = std::move(reading_formats);
					}
				}
				vector<StrpTimeFormat> common;
				if (!has_column) {
					common = column_formats;
				}
				for (idx_t format_idx = 0; has_column && format_idx < shared.size(); format_idx++) {
					for (auto &column_format : column_formats) {
						if (shared[format_idx].format_specifier == column_format.format_specifier) {
							common.push_back(shared[format_idx]);
							break;
						}
					}
				}
				if (!common.empty()) {
					has_column = true;
					shared = std::move(common);
				} else if (!is_declared) {
					// As a date that misses a sampled value after the first row, the column becomes VARCHAR
					auto entry = best_sql_types_candidates_per_column_idx.find(col_idx);
					if (entry != best_sql_types_candidates_per_column_idx.end()) {
						types_lost_to_format_choice.emplace(col_idx, entry->second);
						entry->second.erase(entry->second.begin() + 1, entry->second.end());
					}
					column_types[col_idx] = LogicalType::VARCHAR;
				}
			}
		}
		state_machine.dialect_options.date_format[type].Set(
		    has_column ? shared.back() : CurrentFormat(starting_formats.at(type)), false);
	}
}

void CSVSniffer::DetectTypes() {
	idx_t min_varchar_cols = max_columns_found + 1;
	idx_t min_errors = NumericLimits<idx_t>::Maximum();
	vector<LogicalType> return_types;
	const auto starting_formats = StartingFormatCandidates(false);
	// check which info candidate leads to the minimum number of non-varchar columns...
	for (auto &candidate_cc : candidates) {
		auto &sniffing_state_machine = candidate_cc->GetStateMachine();
		unordered_map<idx_t, vector<LogicalType>> info_sql_types_candidates;
		for (idx_t i = 0; i < max_columns_found; i++) {
			info_sql_types_candidates[i] = sniffing_state_machine.options.auto_type_candidates;
		}
		D_ASSERT(max_columns_found > 0);

		// Set all return_types to VARCHAR, so we can do datatype detection based on VARCHAR values
		return_types.clear();
		return_types.assign(max_columns_found, LogicalType::VARCHAR);

		// Reset candidate for parsing
		auto candidate = candidate_cc->UpgradeToStringValueScanner();
		// Parse chunk and read csv with info-candidate
		auto &data_chunk = candidate->ParseChunk().ToChunk();
		if (candidate->error_handler->AnyErrors() && !candidate->error_handler->HasError(MAXIMUM_LINE_SIZE) &&
		    !candidate->state_machine->options.ignore_errors.GetValue()) {
			continue;
		}
		idx_t start_idx_detection = 0;
		idx_t chunk_size = data_chunk.size();
		if (chunk_size > 1 &&
		    (!options.dialect_options.header.IsSetByUser() ||
		     (options.dialect_options.header.IsSetByUser() && options.dialect_options.header.GetValue()))) {
			// This means we have more than one row, hence we can use the first row to detect if we have a header
			start_idx_detection = 1;
		}
		// Each dialect candidate starts from the starting formats, so one candidate's columns do not narrow another's
		vector<ColumnFormatCandidates> format_candidates(MaxValue(max_columns_found, data_chunk.ColumnCount()),
		                                                 starting_formats);
		// First line where we start our type detection
		SniffTypes(data_chunk, sniffing_state_machine, info_sql_types_candidates, start_idx_detection,
		           format_candidates);
		// Count the number of varchar columns
		idx_t varchar_cols = 0;
		for (idx_t col = 0; col < info_sql_types_candidates.size(); col++) {
			auto &col_type_candidates = info_sql_types_candidates[col];
			// check the number of varchar columns
			const auto &col_type = col_type_candidates.back();
			if (col_type == LogicalType::VARCHAR) {
				varchar_cols++;
			}
		}

		// it's good if the dialect creates more non-varchar columns
		const bool has_less_varchar_cols = varchar_cols < min_varchar_cols;
		// but only if we sacrifice < 30% of best_num_cols.
		const bool acceptable_best_num_cols =
		    static_cast<double>(info_sql_types_candidates.size()) > static_cast<double>(max_columns_found) * 0.7;
		const idx_t number_of_errors = candidate->error_handler->GetSize();
		const bool better_strictness = best_candidate_is_strict ? !candidate->used_unstrictness : true;
		const bool acceptable_candidate = has_less_varchar_cols && acceptable_best_num_cols && better_strictness;
		// If we escaped an unquoted character when strict is false.
		if (!best_candidate ||
		    (acceptable_candidate && (!options.ignore_errors.GetValue() || number_of_errors < min_errors))) {
			min_errors = number_of_errors;
			best_header_row.clear();
			// we have a new best_options candidate
			best_candidate = std::move(candidate);
			min_varchar_cols = varchar_cols;
			best_candidate_is_strict = !best_candidate->used_unstrictness;
			best_sql_types_candidates_per_column_idx = info_sql_types_candidates;
			best_format_candidates = std::move(format_candidates);
			best_first_chunk = &data_chunk;
			best_first_chunk_start = start_idx_detection;
			if (chunk_size > 0) {
				single_row_file = chunk_size == 1;
				for (idx_t col_idx = 0; col_idx < data_chunk.ColumnCount(); col_idx++) {
					auto &cur_vector = data_chunk.data[col_idx];
					auto vector_data = FlatVector::GetData<string_t>(cur_vector);
					auto null_mask = FlatVector::ValidityMutable(cur_vector);
					if (null_mask.RowIsValid(0)) {
						auto value = HeaderValue(vector_data[0]);
						best_header_row.push_back(value);
					} else {
						best_header_row.push_back({});
					}
				}
			}
		}
	}
	if (!best_candidate) {
		DialectCandidates dialect_candidates(options.dialect_options.state_machine_options);
		auto error = CSVError::SniffingError(options, dialect_candidates.Print(), max_columns_found, set_columns, true);
		error_handler->Error(error, true);
	}
	// Assert that it's all good at this point.
	D_ASSERT(best_candidate);
	// The type refinement needs every candidate a column keeps to have read the first chunk too; the best candidate's
	// scanner still holds it
	RefineCandidatesWithChunk(*best_first_chunk, best_first_chunk_start, false);
}

} // namespace duckdb
