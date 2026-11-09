#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"

namespace duckdb {
void CSVSniffer::ReplaceTypes() {
	auto &sniffing_state_machine = best_candidate->GetStateMachine();
	manually_set = vector<bool>(detected_types.size(), false);
	if (sniffing_state_machine.options.sql_type_list.empty() || sniffing_state_machine.options.columns_set) {
		return;
	}
	// user-defined types were supplied for certain columns
	// override the types
	if (!sniffing_state_machine.options.sql_types_per_column.empty()) {
		// types supplied as name -> value map
		idx_t found = 0;
		for (idx_t i = 0; i < names.size(); i++) {
			auto it = sniffing_state_machine.options.sql_types_per_column.find(names[i]);
			if (it != sniffing_state_machine.options.sql_types_per_column.end()) {
				best_sql_types_candidates_per_column_idx[i] = {
				    sniffing_state_machine.options.sql_type_list[it->second]};
				detected_types[i] = sniffing_state_machine.options.sql_type_list[it->second];
				manually_set[i] = true;
				found++;
			}
		}
		if (!file_options.union_by_name && found < sniffing_state_machine.options.sql_types_per_column.size()) {
			auto error_msg = CSVError::ColumnTypesError(options.sql_types_per_column, names);
			error_handler->Error(error_msg);
		}
		return;
	}
	// types supplied as list
	if (names.size() < sniffing_state_machine.options.sql_type_list.size()) {
		throw BinderException("read_csv: %d types were provided, but CSV file only has %d columns",
		                      sniffing_state_machine.options.sql_type_list.size(), names.size());
	}
	for (idx_t i = 0; i < sniffing_state_machine.options.sql_type_list.size(); i++) {
		detected_types[i] = sniffing_state_machine.options.sql_type_list[i];
		manually_set[i] = true;
	}
}

void CSVSniffer::ChooseFormatsForDeclaredTypes() {
	vector<bool> declared(detected_types.size(), false);
	vector<LogicalType> column_types = detected_types;
	bool any_declared = false;
	for (idx_t col_idx = 0; col_idx < detected_types.size(); col_idx++) {
		if (set_columns.IsSet() && col_idx < set_columns.Size()) {
			column_types[col_idx] = (*set_columns.types)[col_idx];
		} else if (col_idx >= manually_set.size() || !manually_set[col_idx]) {
			continue;
		}
		declared[col_idx] = true;
		any_declared = true;
	}
	if (!any_declared) {
		return;
	}
	// The choice before header detection did not know the declared columns, so the columns it made VARCHAR get their
	// types back and the choice is made again
	if (!best_candidate->GetStateMachine().options.all_varchar) {
		for (auto &lost : types_lost_to_format_choice) {
			const auto col_idx = lost.first;
			// a header only file has SQLNULL types by now, which stay
			if (col_idx < column_types.size() && !declared[col_idx] && column_types[col_idx] == LogicalType::VARCHAR) {
				best_sql_types_candidates_per_column_idx[col_idx] = lost.second;
				column_types[col_idx] = DetectedType(col_idx);
			}
		}
	}
	types_lost_to_format_choice.clear();
	ChooseDateAndTimestampFormats(column_types, declared, first_row_is_data ? &best_header_row : nullptr);
	for (idx_t col_idx = 0; col_idx < detected_types.size(); col_idx++) {
		if (!declared[col_idx]) {
			detected_types[col_idx] = column_types[col_idx];
		}
	}
}
} // namespace duckdb
