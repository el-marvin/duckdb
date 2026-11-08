//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/group_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {
class BoundSelectNode;
class ConstantExpression;
class ColumnRefExpression;
struct SelectBindState;

//! The GROUP binder is responsible for binding expressions in the GROUP BY clause
class GroupBinder : public ExpressionBinder {
public:
	GroupBinder(Binder &binder, ClientContext &context, BoundSelectNode &node, SelectBindState &bind_state);

public:
	//! Qualifies the GROUP BY entries and gives each one group: equal plain expressions share a group, a reference
	//! to a select entry shares the group of a plain expression equal to the entry and otherwise names the entry's
	//! own group; the grouping sets are remapped to those groups
	static void ReplaceSelectRefs(Binder &binder, SelectNode &node, SelectBindState &bind_state);

protected:
	BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) override;
	BindResult BindUnnest(FunctionExpression &function, idx_t depth, bool root_expression) override;
	void ThrowIfUnnestInLambda(const ColumnBinding &column_binding) override;

	string UnsupportedAggregateMessage() override;

	bool TryResolveAliasReference(ColumnRefExpression &colref, idx_t depth, bool root_expression, BindResult &result,
	                              unique_ptr<ParsedExpression> &expr_ptr) override;

private:
	static optional_idx GetSelectReference(SelectNode &node, SelectBindState &bind_state, ParsedExpression &expr,
	                                       Identifier &alias_name);

	BoundSelectNode &node;
	SelectBindState &bind_state;
	idx_t unnest_level = 0;
};

} // namespace duckdb
