#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_casting.hpp"

namespace duckdb {
bool CSVSniffer::TryCastVector(Vector &parse_chunk_col, idx_t size, const LogicalType &sql_type,
                               const StrpTimeFormat &date_time_format) const {
	if (sql_type.IsNumeric() && options.thousands_separator != '\0') {
		// The vector cast does not remove the thousands separator, which detection and the scan remove
		const auto values = parse_chunk_col.Values<string_t>();
		for (idx_t row_idx = 0; row_idx < size; row_idx++) {
			const auto value = values[row_idx];
			if (value.IsValid() &&
			    !CanYouCastIt(buffer_manager->context, value.GetValue(), sql_type, options.dialect_options, false,
			                  options.decimal_separator[0], options.thousands_separator)) {
				return false;
			}
		}
		return true;
	}
	// try vector-cast from string to sql_type
	Vector dummy_result(sql_type, size);
	if (!date_time_format.Empty() &&
	    (sql_type.id() == LogicalTypeId::DATE || sql_type.id() == LogicalTypeId::TIMESTAMP)) {
		map<LogicalTypeId, CSVOption<StrpTimeFormat>> formats;
		formats[sql_type.id()].Set(date_time_format, false);
		string error_message;
		CastParameters parameters(false, &error_message);
		if (sql_type.id() == LogicalTypeId::DATE) {
			idx_t line_error;
			return CSVCast::TryCastDateVector(formats, parse_chunk_col, dummy_result, size, parameters, line_error);
		}
		return CSVCast::TryCastTimestampVector(formats, parse_chunk_col, dummy_result, size, parameters);
	}
	if ((sql_type.id() == LogicalTypeId::DOUBLE || sql_type.id() == LogicalTypeId::FLOAT) &&
	    options.decimal_separator == ",") {
		string error_message;
		CastParameters parameters(false, &error_message);
		idx_t line_error;
		return CSVCast::TryCastFloatingVectorCommaSeparated(options, parse_chunk_col, dummy_result, size, parameters,
		                                                    sql_type, line_error);
	}
	if (sql_type.id() == LogicalTypeId::DECIMAL && options.decimal_separator == ",") {
		string error_message;
		CastParameters parameters(false, &error_message);
		idx_t line_error;
		return CSVCast::TryCastDecimalVectorCommaSeparated(options, parse_chunk_col, dummy_result, size, parameters,
		                                                   sql_type, line_error);
	}
	if (sql_type.id() == LogicalTypeId::BIGNUM) {
		auto vector_data = FlatVector::GetData<string_t>(parse_chunk_col);
		auto &validity = FlatVector::ValidityMutable(parse_chunk_col);
		string strip_thousands;
		for (idx_t row_idx = 0; row_idx < size; row_idx++) {
			if (validity.RowIsValid(row_idx)) {
				auto value_ptr = vector_data[row_idx].GetData();
				auto value_size = vector_data[row_idx].GetSize();
				if (options.thousands_separator != '\0') {
					strip_thousands = BaseScanner::RemoveSeparator(value_ptr, value_size, options.thousands_separator);
					value_ptr = strip_thousands.c_str();
					value_size = strip_thousands.size();
				}
				if (!CSVSniffer::CanYouCastBignum(value_ptr, value_size)) {
					return false;
				}
			}
		}
		return true;
	}
	// target type is not varchar: perform a cast
	string error_message;
	return VectorOperations::DefaultTryCast(parse_chunk_col, dummy_result, size, &error_message, true);
}

bool CSVSniffer::ReadsChunk(Vector &parse_chunk_col, idx_t size, const LogicalType &sql_type,
                            const StrpTimeFormat &date_time_format, optional_idx first_valid_row) const {
	if (first_valid_row.IsValid() && size > 1) {
		// Most candidates that miss a chunk miss its first value, which is cheaper to cast on its own
		Vector first_value(parse_chunk_col, first_valid_row.GetIndex(), first_valid_row.GetIndex() + 1);
		if (!TryCastVector(first_value, 1, sql_type, date_time_format)) {
			return false;
		}
	}
	return TryCastVector(parse_chunk_col, size, sql_type, date_time_format);
}

bool CSVSniffer::RefineCandidate(Vector &parse_chunk_col, idx_t size, const LogicalType &sql_type,
                                 ColumnFormatCandidates &column_formats, optional_idx first_valid_row) const {
	if (!IsDateOrTimestamp(sql_type)) {
		return ReadsChunk(parse_chunk_col, size, sql_type, StrpTimeFormat(), first_valid_row);
	}
	auto &format_candidate = column_formats[sql_type.id()];
	// a template is set up only where the column's earlier values are at hand: in detection, or here when it has none
	SetUpFormatsFromChunk(format_candidate, parse_chunk_col, size, sql_type);
	auto &formats = format_candidate.format;
	if (formats.empty() || format_candidate.fixed) {
		if (!ReadsChunk(parse_chunk_col, size, sql_type, CurrentFormat(format_candidate), first_valid_row)) {
			return false;
		}
	} else {
		vector<StrpTimeFormat> reading_formats;
		for (auto &format : formats) {
			if (ReadsChunk(parse_chunk_col, size, sql_type, format, first_valid_row)) {
				reading_formats.push_back(format);
			}
		}
		if (!reading_formats.empty()) {
			format_candidate.had_match = true;
			formats = std::move(reading_formats);
		} else if (format_candidate.had_match) {
			return false;
		} else {
			// no template read the column, so the plain cast reads it, as in detection
			formats.clear();
			if (!ReadsChunk(parse_chunk_col, size, sql_type, StrpTimeFormat(), first_valid_row)) {
				return false;
			}
		}
	}
	if (first_valid_row.IsValid()) {
		format_candidate.read_value = true;
	}
	return true;
}

void CSVSniffer::RefineCandidatesWithChunk(DataChunk &chunk, idx_t first_row, bool check_top) {
	if (first_row >= chunk.size()) {
		return;
	}
	const auto size = chunk.size() - first_row;
	for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
		auto entry = best_sql_types_candidates_per_column_idx.find(col);
		if (entry == best_sql_types_candidates_per_column_idx.end() || entry->second.empty()) {
			continue;
		}
		vector<LogicalType> &col_type_candidates = entry->second;
		if (col >= best_format_candidates.size()) {
			best_format_candidates.resize(col + 1, StartingFormatCandidates(false));
		}
		idx_t candidate_idx = col_type_candidates.size() - 1;
		if (!check_top) {
			if (candidate_idx == 0) {
				continue;
			}
			candidate_idx--;
		}
		Vector values(chunk.data[col], first_row, chunk.size());
		optional_idx first_valid_row;
		auto &validity = FlatVector::Validity(values);
		for (idx_t row = 0; row < size; row++) {
			if (validity.RowIsValid(row)) {
				first_valid_row = row;
				break;
			}
		}
		const auto cur_top_candidate = col_type_candidates.back();
		// Every candidate must read every chunk, so that the one that takes over after a miss reads the earlier ones
		for (; candidate_idx > 0; candidate_idx--) {
			if (!RefineCandidate(values, size, col_type_candidates[candidate_idx], best_format_candidates[col],
			                     first_valid_row)) {
				col_type_candidates.erase(col_type_candidates.begin() + NumericCast<int64_t>(candidate_idx));
			}
		}
		if (cur_top_candidate == LogicalType::BOOLEAN && col_type_candidates.back() != LogicalType::BOOLEAN) {
			// If we thought this was a boolean value (i.e., T,F, True, False) and it is not, we
			// immediately pop to varchar.
			col_type_candidates.erase(col_type_candidates.begin() + 1, col_type_candidates.end());
		}
	}
}

LogicalType CSVSniffer::DetectedType(idx_t column_idx) const {
	auto &col_type_candidates = best_sql_types_candidates_per_column_idx.at(column_idx);
	if (col_type_candidates.size() == best_candidate->GetStateMachine().options.auto_type_candidates.size() &&
	    default_null_to_varchar && !best_candidate->FinishedFile()) {
		// We only default SQLNull to Varchar if we haven't finished the file yet.
		return LogicalType::VARCHAR;
	}
	return col_type_candidates.back();
}

void CSVSniffer::RefineTypes() {
	auto &sniffing_state_machine = best_candidate->GetStateMachine();
	// if data types were provided, exit here if the number of columns does not match
	detected_types.assign(sniffing_state_machine.dialect_options.num_cols, LogicalType::VARCHAR);
	if (sniffing_state_machine.options.all_varchar) {
		// return all types varchar, with the formats the header detection reads the first row with
		vector<LogicalType> column_types;
		for (idx_t column_idx = 0; column_idx < best_sql_types_candidates_per_column_idx.size(); column_idx++) {
			column_types.push_back(best_sql_types_candidates_per_column_idx[column_idx].back());
		}
		ChooseDateAndTimestampFormats(column_types, {}, nullptr);
		return;
	}
	for (idx_t i = 1; i < sniffing_state_machine.options.sample_size_chunks && !best_candidate->FinishedFile(); i++) {
		auto &parse_chunk = best_candidate->ParseChunk().ToChunk();
		RefineCandidatesWithChunk(parse_chunk, 0, true);
		// reset parse chunk for the next iteration
		parse_chunk.Reset();
	}
	detected_types.clear();
	for (idx_t column_idx = 0; column_idx < best_sql_types_candidates_per_column_idx.size(); column_idx++) {
		detected_types.push_back(DetectedType(column_idx));
	}
	// header detection reads the first row with these formats
	ChooseDateAndTimestampFormats(detected_types, {}, nullptr);
}
} // namespace duckdb
