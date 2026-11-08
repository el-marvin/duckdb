//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/column_qualifier.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/binder.hpp"

namespace duckdb {
class ColumnAliasBinder;
class HavingBinder;
class LambdaExpression;

class ColumnQualifier {
public:
	explicit ColumnQualifier(Binder &binder, optional_ptr<vector<DummyBinding>> lambda_bindings = nullptr,
	                         optional_ptr<ColumnAliasBinder> alias_binder = nullptr,
	                         optional_ptr<HavingBinder> having_binder = nullptr);
	virtual ~ColumnQualifier() = default;

	//! The function a bare SQL value function name (e.g. current_timestamp) converts to, or empty
	static Identifier SQLValueFunctionName(const Identifier &column_name);

	//! Returns the STRUCT_EXTRACT operator expression
	unique_ptr<ParsedExpression> CreateStructExtract(unique_ptr<ParsedExpression> base, const Identifier &field_name);
	//! Returns a STRUCT_PACK function expression
	unique_ptr<ParsedExpression> CreateStructPack(ColumnRefExpression &col_ref);

	//! Returns a qualified column reference from a column name
	unique_ptr<ParsedExpression> QualifyColumnName(const ParsedExpression &expr, const Identifier &column_name,
	                                               ErrorData &error);
	//! Returns a qualified column reference from a column reference with column_names.size() > 2
	unique_ptr<ParsedExpression> QualifyColumnNameWithManyDots(ColumnRefExpression &col_ref, ErrorData &error);
	//! Returns a qualified column reference from a column reference
	unique_ptr<ParsedExpression> QualifyColumnName(ColumnRefExpression &col_ref, ErrorData &error);
	//! Enables special-handling of lambda parameters by tracking them in the lambda_params vector
	void QualifyColumnNamesInLambda(FunctionExpression &function, vector<identifier_set_t> &lambda_params);
	//! Recursively qualifies the column references in the (children) of the expression. Passes on the
	//! within_function_expression state from outer expressions, or sets it
	void QualifyColumnNames(unique_ptr<ParsedExpression> &expr, vector<identifier_set_t> &lambda_params,
	                        const bool within_function_expression = false);

	optional_ptr<CatalogEntry> QualifyFunction(FunctionExpression &function);

	//! Whether argument index of the call binds as a lambda, filling in the parameter names it declares
	static bool TryGetLambdaParameters(Binder &binder, const FunctionExpression &function, idx_t index,
	                                   identifier_set_t &parameters);
	//! The receiver of a dot call "x.lower()" as written, as a column reference, or nullptr if the call is not one
	static unique_ptr<ColumnRefExpression> GetDotCallReceiver(Binder &binder, const FunctionExpression &function);
	//! Visits the children of a call, handing each argument that binds as a lambda to lambda_callback instead
	static void EnumerateCallChildren(
	    Binder &binder, FunctionExpression &function,
	    const std::function<void(unique_ptr<ParsedExpression> &child)> &callback,
	    const std::function<void(LambdaExpression &lambda, identifier_set_t &parameters)> &lambda_callback);

private:
	Binder &binder;
	optional_ptr<vector<DummyBinding>> lambda_bindings;
	optional_ptr<ColumnAliasBinder> alias_binder;
	optional_ptr<HavingBinder> having_binder;

private:
	unique_ptr<ParsedExpression> QualifyColumnNameWithManyDotsInternal(ColumnRefExpression &col_ref, ErrorData &error,
	                                                                   idx_t &struct_extract_start);
	//! Whether the given (possibly nested) table qualification matches a binding in the current bind context
	bool TableBindingExists(const BindingAlias &alias);
	//! Whether any binding in the current bind context has the given table name (ignoring catalog/schema)
	bool TableNameExists(const Identifier &table_name);

	unique_ptr<ParsedExpression> QualifyColumnNameInternal(ColumnRefExpression &col_ref, ErrorData &error);
	//! The dot call and lambda argument of the call as written, looked up once per function name and binder
	static WrittenCallArguments GetWrittenCallArguments(Binder &binder, const FunctionExpression &function);
	static unique_ptr<ColumnRefExpression> CreateDotCallReceiver(const QualifiedName &name);
	//! The function entry of the call; dot_call says it is written on a column, as in "x.lower()"
	static optional_ptr<CatalogEntry> FindFunction(Binder &binder, const FunctionExpression &function,
	                                               const QualifiedName &name, bool &dot_call);
};

} // namespace duckdb
