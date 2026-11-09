//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/csv_scanner/csv_state_machine.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/execution/operator/csv_scanner/column_count_scanner.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_schema.hpp"
#include "duckdb/execution/operator/csv_scanner/header_value.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/sniff_result.hpp"

namespace duckdb {
//! The date or timestamp formats that read every value sampled so far, in ascending order of preference
struct DateTimestampSniffing {
	bool initialized = false;
	bool had_match = false;
	//! The format is given (the user's, or in a minimal sniff the one the scan of the file will use) and not narrowed
	bool fixed = false;
	//! The type read a sampled value of the column, so the formats constrain the file's format
	bool read_value = false;
	vector<StrpTimeFormat> format;
};

//! The date and timestamp formats of one column
using ColumnFormatCandidates = map<LogicalTypeId, DateTimestampSniffing>;

struct QuoteEscapeCombination {
	QuoteEscapeCombination(char quote, char escape) : quote(quote), escape(escape) {
	}
	char quote;
	char escape;
};

//! Current stats of candidate analysis
struct CandidateStats {
	//! Number of rows read
	idx_t rows_read = 0;
	//! Best Number of consistent rows (i.e., presenting all columns)
	idx_t best_consistent_rows = 0;
	//! If padding was necessary (i.e., rows are missing some columns, how many)
	idx_t prev_padding_count = 0;
	//! Min number of ignored rows
	idx_t min_ignored_rows = 0;
};

//! All the options that will be used to sniff the dialect of the CSV file
struct DialectCandidates {
	//! The constructor populates all of our the options that will be used in our sniffer search space
	explicit DialectCandidates(const CSVStateMachineOptions &options);

	//! Static functions to get defaults of the search space
	static vector<string> GetDefaultDelimiter();
	//! Default Quote/Escape combinations in priority order
	static vector<QuoteEscapeCombination> GetDefaultQuoteEscapeCombination();

	static vector<char> GetDefaultComment();

	string Print();

	//! Candidates for the delimiter
	vector<string> delim_candidates;
	//! Candidates for the comment
	vector<char> comment_candidates;
	//! Candidates for combinations of quotes and escapes
	vector<QuoteEscapeCombination> quote_escape_candidates;
};

//! Sniffer that detects Header, Dialect and Types of CSV Files
class CSVSniffer {
public:
	explicit CSVSniffer(CSVReaderOptions &options_p, const MultiFileOptions &file_options,
	                    shared_ptr<CSVBufferManager> buffer_manager_p, CSVStateMachineCache &state_machine_cache,
	                    bool default_null_to_varchar = true);

	//! Main method that sniffs the CSV file, returns the types, names and options as a result
	//! CSV Sniffing consists of five steps:
	//! 1. Dialect Detection: Generate the CSV Options (delimiter, quote, escape, etc.)
	//! 2. Type Detection: Figures out the types of the columns (For one chunk)
	//! 3. Type Refinement: Refines the types of the columns for the remaining chunks
	//! 4. Header Detection: Figures out if  the CSV file has a header and produces the names of the columns
	//! 5. Type Replacement: Replaces the types of the columns if the user specified them
	SnifferResult SniffCSV(bool force_match = false);

	//! I call it adaptive, since that's a sexier term.
	//! In practice this function sniffs up to STANDARD_VECTOR_SIZE rows to verify if a header exists and determine the
	//! data types. It does this considering a priorly set CSV schema. If there is a mismatch of the schema it runs the
	//! full on blazing all guns sniffer, if that still fails it tells the user to union_by_name.
	//! It returns the projection order.
	SnifferResult AdaptiveSniff(const CSVSchema &file_schema);

	//! Function that sniffs up to STANDARD_VECTOR_SIZE rows to detect a header and data types
	AdaptiveSnifferResult MinimalSniff();

	static NewLineIdentifier DetectNewLineDelimiter(CSVBufferManager &buffer_manager);

	//! If a string_t value can be cast to a type
	static bool CanYouCastIt(ClientContext &context, const string_t value, const LogicalType &type,
	                         const DialectOptions &dialect_options, const bool is_null, const char decimal_separator,
	                         const char thousands_separator);
	static bool CanYouCastBignum(const char *value_ptr, idx_t value_size);

	idx_t LinesSniffed() const;

	bool EmptyOrOnlyHeader() const;

private:
	//! If all our candidates failed due to lines being bigger than the max line size.
	bool all_fail_max_line_size = true;
	CSVError line_error;
	//! CSV State Machine Cache
	CSVStateMachineCache &state_machine_cache;
	//! Highest number of columns found
	idx_t max_columns_found = 0;
	idx_t max_columns_found_error = 0;
	bool best_candidate_is_strict = false;
	//! Current Candidates being considered
	vector<unique_ptr<ColumnCountScanner>> candidates;
	//! Reference to original CSV Options, it will be modified as a result of the sniffer.
	CSVReaderOptions &options;
	//! The multi-file reader options
	const MultiFileOptions &file_options;
	//! Buffer being used on sniffer
	shared_ptr<CSVBufferManager> buffer_manager;
	//! Information regarding columns that were set by user/query
	SetColumns set_columns;
	shared_ptr<CSVErrorHandler> error_handler;
	shared_ptr<CSVErrorHandler> detection_error_handler;
	//! Number of lines sniffed in this sniffer
	idx_t lines_sniffed;
	//! Sets the result options
	void SetResultOptions() const;

	//! ------------------------------------------------------//
	//! ----------------- Dialect Detection ----------------- //
	//! ------------------------------------------------------//
	//! First phase of auto detection: detect CSV dialect (i.e. delimiter, quote rules, etc)
	void DetectDialect();
	//! Functions called in the main DetectDialect(); function
	//! 1. Generates the search space candidates for the state machines
	void GenerateStateMachineSearchSpace(vector<unique_ptr<ColumnCountScanner>> &column_count_scanners,
	                                     const DialectCandidates &dialect_candidates);

	//! 2. Analyzes if a dialect candidate is a good candidate to be considered, if so, it adds it to the candidates
	void AnalyzeDialectCandidate(unique_ptr<ColumnCountScanner>, CandidateStats &stats,
	                             vector<unique_ptr<ColumnCountScanner>> &successful_candidates);
	//! 3. Refine Candidates over remaining chunks
	void RefineCandidates();

	//! Checks if candidate still produces good values for the next chunk
	bool RefineCandidateNextChunk(ColumnCountScanner &candidate) const;

	//! ------------------------------------------------------//
	//! ------------------- Type Detection ------------------ //
	//! ------------------------------------------------------//
	//! Second phase of auto-detection: detect types, format template candidates
	//! ordered by descending specificity (~ from high to low)
	void DetectTypes();

	//! Function that initialized the necessary variables used for date and timestamp detection
	void InitializeDateAndTimeStampDetection(DateTimestampSniffing &format_candidate, const string &separator,
	                                         const LogicalType &sql_type) const;
	//! The formats a column starts from: the user's, or with fix_to_options the formats in the options, as fixed
	ColumnFormatCandidates StartingFormatCandidates(bool fix_to_options) const;
	//! Sets the column's current date and timestamp formats on the state machine
	static void SetColumnFormats(CSVStateMachine &candidate, const ColumnFormatCandidates &column_formats);
	//! The format the candidate reads with: the most preferred one, or the plain cast
	static StrpTimeFormat CurrentFormat(const DateTimestampSniffing &format_candidate);
	static bool IsDateOrTimestamp(const LogicalType &type);
	//! Sets up the templates of a column that has no earlier value, from the first value of the chunk that looks like a
	//! date
	void SetUpFormatsFromChunk(DateTimestampSniffing &format_candidate, Vector &values, idx_t size,
	                           const LogicalType &sql_type) const;
	//! Whether the type reads the column's values from first_row up to row_idx
	bool ReadsEarlierValues(CSVStateMachine &state_machine, const VectorIterator<string_t> &values,
	                        const LogicalType &sql_type, idx_t first_row, idx_t row_idx) const;
	//! Detects the date or timestamp format from the value at row_idx and the column's earlier values
	void DetectDateAndTimeStampFormats(CSVStateMachine &candidate, DateTimestampSniffing &format_candidate,
	                                   const LogicalType &sql_type, const VectorIterator<string_t> &values,
	                                   idx_t first_row, idx_t row_idx) const;
	//! Sniffs the types from a data chunk
	void SniffTypes(DataChunk &data_chunk, CSVStateMachine &state_machine,
	                unordered_map<idx_t, vector<LogicalType>> &info_sql_types_candidates, idx_t start_idx_detection,
	                vector<ColumnFormatCandidates> &column_formats);
	//! Chooses the file's date and timestamp formats from the columns of those types, the declared columns first; a
	//! sniffed column that shares no format with the columns before it becomes VARCHAR
	void ChooseDateAndTimestampFormats(vector<LogicalType> &column_types, const vector<bool> &declared,
	                                   optional_ptr<const vector<HeaderValue>> first_row);

	//! Variables for Type Detection
	//! Format Candidates for Date and Timestamp Types
	const map<LogicalTypeId, vector<const char *>> format_template_candidates = {
	    {LogicalTypeId::DATE, {"%m-%d-%Y", "%m-%d-%y", "%d-%m-%Y", "%d-%m-%y", "%Y-%m-%d", "%y-%m-%d"}},
	    {LogicalTypeId::TIMESTAMP,
	     {"%Y-%m-%d %H:%M:%S.%f", "%m-%d-%Y %I:%M:%S %p", "%m-%d-%y %I:%M:%S %p", "%d-%m-%Y %H:%M:%S",
	      "%d-%m-%y %H:%M:%S", "%Y-%m-%d %H:%M:%S", "%y-%m-%d %H:%M:%S"}},
	};
	unordered_map<idx_t, vector<LogicalType>> best_sql_types_candidates_per_column_idx;
	//! The type candidates, before the format choice, of the columns it made VARCHAR
	unordered_map<idx_t, vector<LogicalType>> types_lost_to_format_choice;
	//! The date and timestamp formats of each column of the best candidate
	vector<ColumnFormatCandidates> best_format_candidates;
	unique_ptr<StringValueScanner> best_candidate;
	//! The first chunk the best candidate parsed, which its scanner holds until the type refinement
	optional_ptr<DataChunk> best_first_chunk;
	idx_t best_first_chunk_start = 0;
	//! Whether header detection took the first row as data, rather than as the header or a row it skipped
	bool first_row_is_data = false;
	vector<HeaderValue> best_header_row;

	//! ------------------------------------------------------//
	//! ------------------ Type Refinement ------------------ //
	//! ------------------------------------------------------//
	void RefineTypes();
	bool TryCastVector(Vector &parse_chunk_col, idx_t size, const LogicalType &sql_type,
	                   const StrpTimeFormat &date_time_format) const;
	//! Whether the type reads every value of the chunk
	bool ReadsChunk(Vector &parse_chunk_col, idx_t size, const LogicalType &sql_type,
	                const StrpTimeFormat &date_time_format, optional_idx first_valid_row) const;
	//! Whether the type reads the chunk, narrowing the column's formats for a date or timestamp type
	bool RefineCandidate(Vector &parse_chunk_col, idx_t size, const LogicalType &sql_type,
	                     ColumnFormatCandidates &column_formats, optional_idx first_valid_row) const;
	//! Removes the type candidates that do not read the chunk from first_row on; check_top false skips the most
	//! specific candidate of each column, which detection already checked
	void RefineCandidatesWithChunk(DataChunk &chunk, idx_t first_row, bool check_top);
	//! The type of a column as RefineTypes reports it
	LogicalType DetectedType(idx_t column_idx) const;
	//! Chooses the formats again once the declared types are known
	void ChooseFormatsForDeclaredTypes();
	vector<LogicalType> detected_types;
	//! If when finding a SQLNULL type in type detection we default it to varchar
	const bool default_null_to_varchar;
	//! ------------------------------------------------------//
	//! ------------------ Header Detection ----------------- //
	//! ------------------------------------------------------//
	void DetectHeader();
	static bool DetectHeaderWithSetColumn(ClientContext &context, vector<HeaderValue> &best_header_row,
	                                      const SetColumns &set_columns, CSVReaderOptions &options);
	static vector<Identifier>
	DetectHeaderInternal(ClientContext &context, vector<HeaderValue> &best_header_row, CSVStateMachine &state_machine,
	                     const SetColumns &set_columns,
	                     unordered_map<idx_t, vector<LogicalType>> &best_sql_types_candidates_per_column_idx,
	                     CSVReaderOptions &options, const MultiFileOptions &file_options,
	                     CSVErrorHandler &error_handler);
	vector<Identifier> names;
	//! If the file only has a header
	bool single_row_file = false;

	//! ------------------------------------------------------//
	//! ------------------ Type Replacement ----------------- //
	//! ------------------------------------------------------//
	void ReplaceTypes();
	vector<bool> manually_set;
};

} // namespace duckdb
