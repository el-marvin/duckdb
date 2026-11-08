//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/check_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/column_definition.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/common/index_map.hpp"
#include "duckdb/parser/column_list.hpp"

namespace duckdb {
class SchemaCatalogEntry;

//! The CHECK binder is responsible for binding an expression within a CHECK constraint
class CheckBinder : public ExpressionBinder {
public:
	CheckBinder(Binder &binder, ClientContext &context, Identifier table, const ColumnList &columns,
	            physical_index_set_t &bound_columns, optional_ptr<const SchemaCatalogEntry> table_schema = nullptr);

	Identifier table;
	const ColumnList &columns;
	physical_index_set_t &bound_columns;
	//! Where a referenced generated column's expression resolves its names; a CHECK constraint binds without one, as it
	//! binds its own names with the binder's path
	optional_ptr<const SchemaCatalogEntry> table_schema;

protected:
	BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
	                          bool root_expression = false) override;

	BindResult BindCheckColumn(ColumnRefExpression &expr);

	string UnsupportedAggregateMessage() override;
};

} // namespace duckdb
