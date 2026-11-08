#include "duckdb/planner/expression_binder/group_binder.hpp"

#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression_map.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_binder/select_bind_state.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

GroupBinder::GroupBinder(Binder &binder, ClientContext &context, BoundSelectNode &node, SelectBindState &bind_state)
    : ExpressionBinder(binder, context), node(node), bind_state(bind_state) {
}

BindResult GroupBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	auto &expr = *expr_ptr;
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::DEFAULT:
		return BindUnsupportedExpression(expr, depth, "GROUP BY clause cannot contain DEFAULT clause");
	case ExpressionClass::WINDOW:
		return BindUnsupportedExpression(expr, depth, "GROUP BY clause cannot contain window functions!");
	default:
		return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
	}
}

string GroupBinder::UnsupportedAggregateMessage() {
	return "GROUP BY clause cannot contain aggregates!";
}

void GroupBinder::ThrowIfUnnestInLambda(const ColumnBinding &column_binding) {
	for (auto &node_pair : node.unnests.GroupBy()) {
		auto &unnest_node = node_pair.second;
		if (unnest_node.index == column_binding.table_index &&
		    column_binding.column_index < unnest_node.expressions.size()) {
			throw BinderException("UNNEST in lambda expressions is not supported");
		}
	}
}

optional_idx GroupBinder::GetSelectReference(SelectNode &node, SelectBindState &bind_state, ParsedExpression &expr,
                                             Identifier &alias_name) {
	idx_t select_list_idx;
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
		// root is a column - check if it refers to an alias in the select list
		auto &colref = expr.Cast<ColumnRefExpression>();
		if (!IsPotentialAlias(colref)) {
			return optional_idx();
		}
		auto entry = bind_state.alias_map.find(colref.GetColumnName());
		if (entry == bind_state.alias_map.end()) {
			// no matching alias found
			return optional_idx();
		}
		alias_name = colref.GetColumnName();
		select_list_idx = entry->second;
		break;
	}
	case ExpressionClass::CONSTANT: {
		// root is a constant
		auto &constant = expr.Cast<ConstantExpression>();
		int64_t index;
		if (!constant.GetLiteral().TryGetInt64(index)) {
			// non-integral expression, we just leave the constant here.
			return optional_idx();
		}
		// INTEGER constant: we use the integer as an index into the select list (e.g. GROUP BY 1)
		select_list_idx = (idx_t)index - 1;
		break;
	}
	case ExpressionClass::PARAMETER:
		throw ParameterNotAllowedException("Parameter not supported in GROUP BY clause");
	default:
		return optional_idx();
	}
	if (select_list_idx >= node.select_list.size()) {
		throw BinderException("GROUP BY term out of range - should be between 1 and %d", (int)node.select_list.size());
	}
	return select_list_idx;
}

void GroupBinder::ReplaceSelectRefs(Binder &binder, SelectNode &node, SelectBindState &bind_state) {
	auto &group_expressions = node.groups.group_expressions;
	vector<optional_idx> select_references;
	vector<Identifier> aliases(group_expressions.size());
	parsed_expression_set_t plain_expressions;
	for (idx_t i = 0; i < group_expressions.size(); i++) {
		ExpressionBinder::QualifyColumnNames(binder, group_expressions[i]);
		select_references.push_back(GetSelectReference(node, bind_state, *group_expressions[i], aliases[i]));
		if (!select_references[i].IsValid()) {
			plain_expressions.insert(*group_expressions[i]);
		}
	}

	// a GROUP BY entry names the group of its class: a plain expression is the class of that expression, and so is a
	// select reference whose entry equals a plain expression, as the select list reads such an entry as that group;
	// any other select reference is the class of its entry
	vector<unique_ptr<ParsedExpression>> groups;
	vector<ProjectionIndex> group_of_entry;
	parsed_expression_map_t<ProjectionIndex> expression_groups;
	auto &grouped_entries = bind_state.grouped_select_entries;
	for (idx_t i = 0; i < group_expressions.size(); i++) {
		ProjectionIndex new_group(groups.size());
		if (!select_references[i].IsValid()) {
			auto entry = expression_groups.find(*group_expressions[i]);
			if (entry != expression_groups.end()) {
				group_of_entry.push_back(entry->second);
				continue;
			}
			expression_groups.emplace(*group_expressions[i], new_group);
			groups.push_back(std::move(group_expressions[i]));
			group_of_entry.push_back(new_group);
			continue;
		}
		auto select_list_idx = select_references[i].GetIndex();
		auto grouped_entry = grouped_entries.find(select_list_idx);
		ProjectionIndex entry_group;
		if (grouped_entry != grouped_entries.end()) {
			entry_group = grouped_entry->second;
		} else {
			auto &select_expr = *node.select_list[select_list_idx];
			bool has_plain_class = plain_expressions.find(select_expr) != plain_expressions.end();
			auto class_group = has_plain_class ? expression_groups.find(select_expr) : expression_groups.end();
			if (class_group != expression_groups.end()) {
				entry_group = class_group->second;
				bind_state.replaced_select_expressions.push_back(std::move(node.select_list[select_list_idx]));
			} else {
				if (has_plain_class) {
					expression_groups.emplace(select_expr, new_group);
				}
				entry_group = new_group;
				groups.push_back(std::move(node.select_list[select_list_idx]));
			}
			grouped_entries[select_list_idx] = entry_group;
			Identifier select_ref(to_string(select_list_idx));
			bind_state.group_alias_map[select_ref] = entry_group;
			node.select_list[select_list_idx] = make_uniq<ColumnRefExpression>(select_ref);
		}
		group_of_entry.push_back(entry_group);
		if (!aliases[i].empty()) {
			bind_state.group_alias_map[aliases[i]] = entry_group;
		}
	}

	for (auto &grouping_set : node.groups.grouping_sets) {
		GroupingSet remapped_set;
		for (auto &entry : grouping_set) {
			remapped_set.insert(group_of_entry[entry.GetIndex()]);
		}
		grouping_set = std::move(remapped_set);
	}
	group_expressions = std::move(groups);
}

bool GroupBinder::TryResolveAliasReference(ColumnRefExpression &colref, idx_t depth, bool root_expression,
                                           BindResult &result, unique_ptr<ParsedExpression> &expr_ptr) {
	// try to resolve alias references in GROUP
	// failed to bind the column and the node is the root expression with depth = 0
	// check if refers to an alias in the select clause

	auto &alias_name = colref.GetColumnName();
	auto entry = bind_state.alias_map.find(alias_name);
	if (entry == bind_state.alias_map.end()) {
		// no matching alias found
		return false;
	}
	result = BindResult(BinderException(
	    colref, "Alias with name %s exists, but aliases cannot be used as part of an expression in the GROUP BY",
	    alias_name));
	return true;
}

} // namespace duckdb
