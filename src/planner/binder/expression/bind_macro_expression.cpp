#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/planner/column_qualifier.hpp"

namespace duckdb {

void ExpressionBinder::ReplaceMacroParametersInLambda(FunctionExpression &function,
                                                      vector<identifier_set_t> &lambda_params) {
	for (auto &child : function.GetArgumentsMutable()) {
		if (child.GetExpression().GetExpressionClass() != ExpressionClass::LAMBDA) {
			ReplaceMacroParameters(child.GetExpressionMutable(), lambda_params);
			continue;
		}

		// Special-handling for LHS lambda parameters.
		// We do not replace them, and we add them to the lambda_params vector.
		auto &lambda_expr = child.GetExpressionMutable()->Cast<LambdaExpression>();
		string error_message;
		auto column_ref_expressions = lambda_expr.ExtractColumnRefExpressions(error_message);

		if (!error_message.empty()) {
			// Possibly a JSON function, replace both LHS and RHS.
			ReplaceMacroParameters(lambda_expr.LeftMutable(), lambda_params);
			ReplaceMacroParameters(lambda_expr.RightMutable(), lambda_params);
			continue;
		}

		// Push the lambda parameter names of this level.
		lambda_params.emplace_back();
		for (const auto &column_ref_expr : column_ref_expressions) {
			const auto &column_ref = column_ref_expr.get().Cast<ColumnRefExpression>();
			lambda_params.back().emplace(column_ref.GetName());
		}

		// Only replace in the RHS of the expression.
		ReplaceMacroParameters(lambda_expr.RightMutable(), lambda_params);

		lambda_params.pop_back();
	}
}

void ExpressionBinder::ReplaceMacroParameters(unique_ptr<ParsedExpression> &expr,
                                              vector<identifier_set_t> &lambda_params) {
	switch (expr->GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
		// If the expression is a column reference, we replace it with its argument.
		auto &col_ref = expr->Cast<ColumnRefExpression>();
		if (LambdaExpression::IsLambdaParameter(lambda_params, col_ref.GetColumnName())) {
			return;
		}

		bool bind_macro_parameter = false;
		if (col_ref.IsQualified()) {
			// the table qualifier is the component directly before the column name
			auto &names = col_ref.ColumnNames();
			if (names[names.size() - 2].StartsWith(DummyBinding::DUMMY_NAME)) {
				bind_macro_parameter = true;
			}
		} else {
			bind_macro_parameter = macro_binding->HasMatchingBinding(col_ref.GetColumnName());
		}

		if (bind_macro_parameter) {
			D_ASSERT(macro_binding->HasMatchingBinding(col_ref.GetColumnName()));
			expr = macro_binding->ParamToArg(col_ref);
		}
		return;
	}
	case ExpressionClass::FUNCTION: {
		// Special-handling for lambdas, which are inside function expressions.
		auto &function = expr->Cast<FunctionExpression>();
		if (function.IsLambdaFunction()) {
			return ReplaceMacroParametersInLambda(function, lambda_params);
		}
		break;
	}
	case ExpressionClass::SUBQUERY: {
		auto &sq = (expr->Cast<SubqueryExpression>()).Subquery();
		ParsedExpressionIterator::EnumerateQueryNodeChildren(
		    *sq->node, [&](unique_ptr<ParsedExpression> &child) { ReplaceMacroParameters(child, lambda_params); });
		break;
	}
	default:
		break;
	}

	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { ReplaceMacroParameters(child, lambda_params); });
}

static bool GetLambdaParameters(LambdaExpression &lambda_expr, vector<reference<ColumnRefExpression>> &parameters) {
	auto &lhs = *lambda_expr.LeftMutable();
	if (lhs.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &column_ref = lhs.Cast<ColumnRefExpression>();
		if (column_ref.IsQualified()) {
			return false;
		}
		parameters.push_back(column_ref);
		return true;
	}
	if (lhs.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &row = lhs.Cast<FunctionExpression>();
	if (row.FunctionName() != "row" || row.GetArguments().empty()) {
		return false;
	}
	for (auto &argument : row.GetArgumentsMutable()) {
		auto &child = *argument.GetExpressionMutable();
		if (child.GetExpressionClass() != ExpressionClass::COLUMN_REF ||
		    child.Cast<ColumnRefExpression>().IsQualified()) {
			parameters.clear();
			return false;
		}
		parameters.push_back(child.Cast<ColumnRefExpression>());
	}
	return true;
}

static void CollectFreeNames(ParsedExpression &expr, vector<identifier_set_t> &bound, identifier_set_t &names) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
		auto &leading_name = expr.Cast<ColumnRefExpression>().ColumnNames()[0];
		if (!LambdaExpression::IsLambdaParameter(bound, leading_name)) {
			names.insert(leading_name);
		}
		return;
	}
	case ExpressionClass::LAMBDA: {
		// an arrow may be the JSON operator, whose left side names a column
		auto &lambda_expr = expr.Cast<LambdaExpression>();
		vector<reference<ColumnRefExpression>> parameters;
		if (lambda_expr.GetLambdaSyntaxType() != LambdaSyntaxType::LAMBDA_KEYWORD ||
		    !GetLambdaParameters(lambda_expr, parameters)) {
			break;
		}
		bound.emplace_back();
		for (auto &parameter : parameters) {
			bound.back().insert(parameter.get().GetColumnName());
		}
		CollectFreeNames(*lambda_expr.RightMutable(), bound, names);
		bound.pop_back();
		return;
	}
	case ExpressionClass::SUBQUERY:
		// the binder rejects a subquery inside a lambda, so no name of it can be captured by a lambda parameter
		return;
	default:
		break;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](ParsedExpression &child) { CollectFreeNames(child, bound, names); });
}

static void CollectWrittenNames(ParsedExpression &expr, identifier_set_t &names) {
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &column_names = expr.Cast<ColumnRefExpression>().ColumnNames();
		names.insert(column_names.begin(), column_names.end());
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &path = expr.Cast<FunctionExpression>().GetQualifiedName().Path();
		names.insert(path.begin(), path.end());
	}
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &sq = expr.Cast<SubqueryExpression>().Subquery();
		ParsedExpressionIterator::EnumerateQueryNodeChildren(
		    *sq->node, [&](unique_ptr<ParsedExpression> &child) { CollectWrittenNames(*child, names); });
	}
	ParsedExpressionIterator::EnumerateChildren(expr,
	                                            [&](ParsedExpression &child) { CollectWrittenNames(child, names); });
}

namespace {

struct LambdaScope {
	explicit LambdaScope(LambdaExpression &lambda_expr) : lambda_expr(lambda_expr) {
	}
	reference<LambdaExpression> lambda_expr;
	vector<reference<ColumnRefExpression>> parameters;
	identifier_set_t captured;
};

class MacroLambdaRenamer {
public:
	MacroLambdaRenamer(const vector<reference<unique_ptr<ParsedExpression>>> &body, Binder &binder,
	                   DummyBinding &macro_binding, optional_ptr<Binder> dummy_binder,
	                   optional_ptr<ParsedExpression> window_aggregate, optional_ptr<ParsedExpression> caller_window)
	    : body(body), binder(binder), macro_binding(macro_binding), dummy_binder(dummy_binder),
	      body_is_qualified(dummy_binder), window_aggregate(window_aggregate), caller_window(caller_window) {
	}

	void Rename(unique_ptr<ParsedExpression> &expr) {
		if (window_aggregate && expr.get() == window_aggregate.get()) {
			Capture(CallerWindowNames());
		}
		switch (expr->GetExpressionClass()) {
		case ExpressionClass::COLUMN_REF:
			CaptureColumnReference(expr->Cast<ColumnRefExpression>());
			return;
		case ExpressionClass::FUNCTION: {
			auto &function = expr->Cast<FunctionExpression>();
			auto &path = function.GetQualifiedName().Path();
			// the dummy qualification turns a dot call on a macro parameter, a.upper(), into a parameter reference
			if (body_is_qualified && !scopes.empty() && path.size() >= 2 && !IsLambdaParameter(path[0]) &&
			    macro_binding.HasMatchingBinding(path[0]) && IsDotCall(function)) {
				CaptureMacroParameter(path[0]);
			}
			if (body_is_qualified && path.size() >= 3 && IsLambdaParameter(path[0]) && IsDotCall(function)) {
				auto &qualified_name = function.GetQualifiedName();
				CaptureByCallerScope(ColumnRefExpression(qualified_name.Schema(), qualified_name.Catalog()));
			}
			if (!function.IsLambdaFunction()) {
				break;
			}
			for (auto &child : function.GetArgumentsMutable()) {
				auto &child_expr = child.GetExpressionMutable();
				if (child_expr->GetExpressionClass() != ExpressionClass::LAMBDA) {
					Rename(child_expr);
					continue;
				}
				auto &lambda_expr = child_expr->Cast<LambdaExpression>();
				LambdaScope scope(lambda_expr);
				if (!GetLambdaParameters(lambda_expr, scope.parameters)) {
					Rename(lambda_expr.LeftMutable());
					Rename(lambda_expr.RightMutable());
					continue;
				}
				scopes.push_back(std::move(scope));
				Rename(lambda_expr.RightMutable());
				RenameCaptured(scopes.back());
				scopes.pop_back();
			}
			return;
		}
		case ExpressionClass::SUBQUERY: {
			// the dummy qualification does not enter a subquery, so its references stay as written
			auto &sq = expr->Cast<SubqueryExpression>().Subquery();
			auto outer_body_is_qualified = body_is_qualified;
			body_is_qualified = false;
			ParsedExpressionIterator::EnumerateQueryNodeChildren(
			    *sq->node, [&](unique_ptr<ParsedExpression> &child) { Rename(child); });
			body_is_qualified = outer_body_is_qualified;
			break;
		}
		default:
			break;
		}
		ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) { Rename(child); });
	}

private:
	bool IsLambdaParameter(const Identifier &name) const {
		for (auto &scope : scopes) {
			for (auto &parameter : scope.parameters) {
				if (parameter.get().GetColumnName() == name) {
					return true;
				}
			}
		}
		return false;
	}

	optional_ptr<LambdaScope> DeclaringScope(const Identifier &name) {
		for (idx_t i = scopes.size(); i > 0; i--) {
			for (auto &parameter : scopes[i - 1].parameters) {
				if (parameter.get().GetColumnName() == name) {
					return scopes[i - 1];
				}
			}
		}
		return nullptr;
	}

	// The top level of a scalar macro body binds in the caller's scope, which resolves a dotted name against its
	// own tables before the lambda parameters, as it must for a lambda written there, so a caller table named like
	// a body parameter would read the parameter's fields.
	void CaptureByCallerScope(const ColumnRefExpression &column_ref) {
		if (!column_ref.IsQualified()) {
			return;
		}
		auto &parameter = column_ref.ColumnNames()[0];
		auto scope = DeclaringScope(parameter);
		if (!scope || scope->captured.find(parameter) != scope->captured.end()) {
			return;
		}
		if (CallerScopeBindsElsewhere(column_ref)) {
			// a single arrow may be the JSON operator, whose left side names the parameter of an enclosing lambda
			Capture(identifier_set_t {parameter});
		}
	}

	bool CallerScopeBindsElsewhere(const ColumnRefExpression &column_ref) {
		auto &parameter = column_ref.ColumnNames()[0];
		vector<DummyBinding> parameter_binding;
		parameter_binding.emplace_back(vector<LogicalType> {LogicalType::ANY}, vector<Identifier> {parameter},
		                               parameter.GetIdentifierName());
		ColumnQualifier qualifier(binder, &parameter_binding);
		ColumnRefExpression probe(column_ref.ColumnNames());
		unique_ptr<ParsedExpression> qualified;
		try {
			ErrorData error;
			qualified = qualifier.QualifyColumnName(probe, error);
		} catch (std::exception &ex) {
			if (ErrorData(ex).Type() != ExceptionType::BINDER) {
				throw;
			}
			// an ambiguous name in the caller's scope is no reading of the parameter either
			return true;
		}
		D_ASSERT(qualified);
		reference<ParsedExpression> base = *qualified;
		while (base.get().GetExpressionType() == ExpressionType::STRUCT_EXTRACT) {
			base = *base.get().Cast<OperatorExpression>().GetChildren()[0];
		}
		return base.get().GetExpressionClass() != ExpressionClass::LAMBDA_REF;
	}

	// A rename can end ReplaceMacroParameters' last name test, so a qualified form counts whatever its last name.
	void CaptureColumnReference(ColumnRefExpression &column_ref) {
		auto &names = column_ref.ColumnNames();
		if (body_is_qualified) {
			CaptureByCallerScope(column_ref);
		}
		if (!column_ref.IsQualified()) {
			if (!IsLambdaParameter(names[0])) {
				CaptureMacroParameter(names[0]);
			}
		} else if (names[names.size() - 2].StartsWith(DummyBinding::DUMMY_NAME)) {
			CaptureMacroParameter(names.back());
		} else if (body_is_qualified && !IsLambdaParameter(names[0])) {
			// the dummy qualification reads a.d on a macro parameter a as a field of the argument
			CaptureMacroParameter(names[0]);
		}
	}

	void CaptureMacroParameter(const Identifier &parameter) {
		column_t index;
		if (scopes.empty() || !macro_binding.TryGetBindingIndex(parameter, index)) {
			return;
		}
		auto entry = argument_names.find(index);
		if (entry == argument_names.end()) {
			vector<identifier_set_t> bound;
			entry = argument_names.emplace(index, identifier_set_t()).first;
			CollectFreeNames(*(*macro_binding.arguments)[index], bound, entry->second);
		}
		Capture(entry->second);
	}

	const identifier_set_t &CallerWindowNames() {
		if (!caller_window_names) {
			caller_window_names = make_uniq<identifier_set_t>();
			vector<identifier_set_t> bound;
			CollectFreeNames(*caller_window, bound, *caller_window_names);
		}
		return *caller_window_names;
	}

	void Capture(const identifier_set_t &names) {
		for (auto &scope : scopes) {
			for (auto &parameter : scope.parameters) {
				auto &name = parameter.get().GetColumnName();
				if (names.find(name) != names.end()) {
					scope.captured.insert(name);
				}
			}
		}
	}

	// A leading digit needs quotes, so no table or column at the call site has the name; one name per original name
	// keeps identical lambdas, such as a select list item and its GROUP BY, equal.
	Identifier GetUnusedName(const Identifier &name) {
		auto known = new_names.find(name);
		if (known != new_names.end()) {
			return known->second;
		}
		if (!used_names) {
			used_names = make_uniq<identifier_set_t>();
			for (auto &expr : body) {
				CollectWrittenNames(*expr.get(), *used_names);
			}
			for (auto &argument : *macro_binding.arguments) {
				CollectWrittenNames(*argument, *used_names);
			}
			if (caller_window) {
				CollectWrittenNames(*caller_window, *used_names);
			}
		}
		for (idx_t suffix = 1;; suffix++) {
			Identifier candidate("0_" + to_string(suffix) + "_" + name.GetIdentifierName());
			if (used_names->insert(candidate).second) {
				new_names.emplace(name, candidate);
				return candidate;
			}
		}
	}

	// A child of the expanding binder has its search path; without the catalog callback it records no dependency.
	Binder &ProbeBinder() {
		if (!probe_binder) {
			probe_binder = Binder::CreateBinder(binder.context, &binder);
			probe_binder->EntryRetriever().SetCallback(nullptr);
		}
		return *probe_binder;
	}

	// Only the search path of the binder that will qualify the call decides whether it is a dot call.
	bool IsDotCall(FunctionExpression &function) {
		if (ExpressionBinder::IsUnnestFunction(function.FunctionName())) {
			return false;
		}
		FunctionExpression probe(function.GetQualifiedName(), vector<unique_ptr<ParsedExpression>>());
		ColumnQualifier(body_is_qualified ? *dummy_binder : ProbeBinder()).QualifyFunction(probe);
		return probe.GetQualifiedName().Path().size() == 1;
	}

	// An inner lambda declaring the name is renamed with it, which also renames the column of a JSON arrow.
	void RenameReferences(unique_ptr<ParsedExpression> &expr, const identifier_map_t<Identifier> &renames) {
		switch (expr->GetExpressionClass()) {
		case ExpressionClass::COLUMN_REF: {
			auto &column_ref = expr->Cast<ColumnRefExpression>();
			auto &leading_name = column_ref.ColumnNamesMutable()[0];
			auto entry = renames.find(leading_name);
			if (entry == renames.end()) {
				return;
			}
			// struct_pack(x) names its field after the reference
			if (!column_ref.IsQualified() && column_ref.GetAlias().empty()) {
				column_ref.SetAlias(leading_name);
			}
			leading_name = entry->second;
			return;
		}
		case ExpressionClass::LAMBDA: {
			// a declaration is named by its alias when it has one, so it takes the new name without an alias
			auto &lambda_expr = expr->Cast<LambdaExpression>();
			vector<reference<ColumnRefExpression>> parameters;
			if (!GetLambdaParameters(lambda_expr, parameters)) {
				break;
			}
			for (auto &parameter : parameters) {
				auto &name = parameter.get().ColumnNamesMutable()[0];
				auto entry = renames.find(name);
				if (entry != renames.end()) {
					name = entry->second;
				}
			}
			RenameReferences(lambda_expr.RightMutable(), renames);
			return;
		}
		case ExpressionClass::FUNCTION: {
			auto &function = expr->Cast<FunctionExpression>();
			auto &path = function.GetQualifiedName().Path();
			if (path.size() >= 2 && renames.find(path[0]) != renames.end() && IsDotCall(function)) {
				// rewritten as the qualifier would, so that the receiver is a reference renamed below with its alias
				auto qualified_name = function.GetQualifiedName();
				auto receiver = qualified_name.Catalog().empty()
				                    ? make_uniq<ColumnRefExpression>(qualified_name.Schema())
				                    : make_uniq<ColumnRefExpression>(qualified_name.Schema(), qualified_name.Catalog());
				auto &arguments = function.GetArgumentsMutable();
				arguments.insert(arguments.begin(), FunctionArgument(std::move(receiver)));
				function.SetQualifiedName(QualifiedName(qualified_name.Name()));
			}
			break;
		}
		case ExpressionClass::SUBQUERY: {
			auto &sq = expr->Cast<SubqueryExpression>().Subquery();
			auto outer_body_is_qualified = body_is_qualified;
			body_is_qualified = false;
			ParsedExpressionIterator::EnumerateQueryNodeChildren(
			    *sq->node, [&](unique_ptr<ParsedExpression> &child) { RenameReferences(child, renames); });
			body_is_qualified = outer_body_is_qualified;
			break;
		}
		default:
			break;
		}
		ParsedExpressionIterator::EnumerateChildren(
		    *expr, [&](unique_ptr<ParsedExpression> &child) { RenameReferences(child, renames); });
	}

	void RenameCaptured(LambdaScope &scope) {
		if (scope.captured.empty()) {
			return;
		}
		identifier_map_t<Identifier> renames;
		for (auto &parameter : scope.parameters) {
			auto &name = parameter.get().ColumnNamesMutable()[0];
			if (scope.captured.find(name) == scope.captured.end()) {
				continue;
			}
			// a repeated parameter keeps one name, so the binder still rejects it as a duplicate
			auto entry = renames.find(name);
			if (entry == renames.end()) {
				entry = renames.emplace(name, GetUnusedName(name)).first;
			}
			name = entry->second;
		}
		RenameReferences(scope.lambda_expr.get().RightMutable(), renames);
	}

	const vector<reference<unique_ptr<ParsedExpression>>> &body;
	Binder &binder;
	DummyBinding &macro_binding;
	optional_ptr<Binder> dummy_binder;
	bool body_is_qualified;
	optional_ptr<ParsedExpression> window_aggregate;
	optional_ptr<ParsedExpression> caller_window;
	vector<LambdaScope> scopes;
	unordered_map<column_t, identifier_set_t> argument_names;
	unique_ptr<identifier_set_t> caller_window_names;
	unique_ptr<identifier_set_t> used_names;
	identifier_map_t<Identifier> new_names;
	shared_ptr<Binder> probe_binder;
};

} // namespace

void ExpressionBinder::RenameLambdaParametersCapturingArguments(
    const vector<reference<unique_ptr<ParsedExpression>>> &body, Binder &binder, DummyBinding &macro_binding,
    optional_ptr<Binder> dummy_binder, optional_ptr<ParsedExpression> window_aggregate,
    optional_ptr<ParsedExpression> caller_window) {
	MacroLambdaRenamer renamer(body, binder, macro_binding, dummy_binder, window_aggregate, caller_window);
	for (auto &expr : body) {
		renamer.Rename(expr.get());
	}
}

static unique_ptr<ParsedExpression> &FindExpressionSlot(unique_ptr<ParsedExpression> &root,
                                                        const ParsedExpression &target) {
	optional_ptr<unique_ptr<ParsedExpression>> slot;
	std::function<void(unique_ptr<ParsedExpression> &)> find = [&](unique_ptr<ParsedExpression> &child) {
		if (slot) {
			return;
		}
		if (child.get() == &target) {
			slot = &child;
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(*child, find);
	};
	find(root);
	if (!slot) {
		throw InternalException("Window macro aggregate not found in the macro body");
	}
	return *slot;
}

// Find aggregate expression children
void ExpressionBinder::FindAggregateExprs(unique_ptr<ParsedExpression> &expr,
                                          vector<reference<unique_ptr<ParsedExpression>>> &exprs) {
	if (expr->GetExpressionType() == ExpressionType::FUNCTION) {
		auto &fn_expr = expr->Cast<FunctionExpression>();

		// Look up the function in the catalog, check to see if it is actually an aggregate function
		EntryLookupInfo fn_entry(CatalogType::AGGREGATE_FUNCTION_ENTRY, fn_expr.GetQualifiedName());
		auto entry = GetCatalogEntry(fn_entry, OnEntryNotFound::RETURN_NULL);

		if (entry && entry->type == CatalogType::AGGREGATE_FUNCTION_ENTRY) {
			exprs.push_back(expr);
			return;
		}
	}

	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child_expr) { FindAggregateExprs(child_expr, exprs); });
}

void ExpressionBinder::UnfoldWindowMacroExpression(unique_ptr<ParsedExpression> &expr, ScalarMacroFunction &macro_def,
                                                   Binder &dummy_binder) {
	auto macro_copy = macro_def.expression->Copy();
	vector<reference<unique_ptr<ParsedExpression>>> aggregate_exprs;
	FindAggregateExprs(macro_copy, aggregate_exprs);

	// Only allowed if the macro body has a single aggregate expression
	if (aggregate_exprs.size() != 1) {
		throw BinderException("Window function macro bodies must contain exactly one aggregate function");
	}

	// The window spec is pushed down to the aggregate function target within the macro body
	auto &agg_fn_expr = aggregate_exprs[0].get()->Cast<FunctionExpression>();

	// Transfer the macro function attributes
	auto &window_expr = expr->Cast<WindowExpression>();
	window_expr.SetQualifiedName(agg_fn_expr.GetQualifiedName());
	window_expr.GetArgumentsMutable().clear();
	// renamed after the call's arguments are cleared and before the caller's window clause moves into the body
	RenameLambdaParametersCapturingArguments({macro_copy}, binder, *macro_binding, dummy_binder, agg_fn_expr,
	                                         expr.get());
	// renaming can insert a dot call's receiver before the aggregate, which moves the slot holding it
	auto &agg_expr_ref = FindExpressionSlot(macro_copy, agg_fn_expr);
	for (auto &arg : agg_fn_expr.GetArgumentsMutable()) {
		window_expr.GetArgumentsMutable().push_back(std::move(arg));
	}
	if (!window_expr.Distinct()) {
		window_expr.DistinctMutable() = agg_fn_expr.Distinct();
	}
	if (window_expr.Filter() && agg_fn_expr.Filter()) {
		// Two FILTER clauses: combine
		window_expr.FilterMutable() =
		    make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(window_expr.FilterMutable()),
		                                     std::move(agg_fn_expr.FilterMutable()));
	} else if (agg_fn_expr.Filter()) {
		//	One FILTER from the MACRO
		window_expr.FilterMutable() = std::move(agg_fn_expr.FilterMutable());
	}
	// Transfer argument ORDER BYs
	if (agg_fn_expr.OrderBy()) {
		auto clone = agg_fn_expr.OrderBy()->Copy();
		window_expr.ArgOrdersMutable() = std::move(clone->Cast<OrderModifier>().orders);
	}

	// Replace the aggregate expression with the new window expression
	agg_expr_ref = std::move(expr);
	expr = std::move(macro_copy);
}

MacroFunction &ExpressionBinder::UnfoldMacroExpression(FunctionExpression &function,
                                                       ScalarMacroCatalogEntry &macro_func,
                                                       unique_ptr<ParsedExpression> &expr, idx_t depth) {
	// validate the arguments and separate positional and default arguments
	vector<unique_ptr<ParsedExpression>> positional_arguments;
	InsertionOrderPreservingMap<unique_ptr<ParsedExpression>, Identifier, identifier_map_t<idx_t>> named_arguments;
	binder.lambda_bindings = lambda_bindings;
	auto bind_result = MacroFunction::BindMacroFunction(binder, macro_func.macros, macro_func.name, function,
	                                                    positional_arguments, named_arguments, depth);
	if (!bind_result.error.empty()) {
		throw BinderException(*expr, bind_result.error);
	}
	auto &macro_def = macro_func.macros[bind_result.function_idx.GetIndex()]->Cast<ScalarMacroFunction>();

	auto new_macro_binding =
	    MacroFunction::CreateDummyBinding(macro_def, macro_func.name, positional_arguments, named_arguments);
	macro_binding = new_macro_binding.get();

	// a new empty binder that only knows the macro binding qualifies the macro parameters
	auto dummy_binder = Binder::CreateBinder(context);
	dummy_binder->macro_binding = new_macro_binding.get();

	// replace current expression with stored macro expression
	// special case: If this is a window function, then we need to return a window expression
	if (expr->GetExpressionClass() == ExpressionClass::WINDOW) {
		UnfoldWindowMacroExpression(expr, macro_def, *dummy_binder);
	} else {
		expr = macro_def.expression->Copy();
		RenameLambdaParametersCapturingArguments({expr}, binder, *macro_binding, dummy_binder.get());
	}

	ExpressionBinder::QualifyColumnNames(*dummy_binder, expr);

	// now replace the parameters
	vector<identifier_set_t> lambda_params;
	ReplaceMacroParameters(expr, lambda_params);
	return macro_def;
}

BindResult ExpressionBinder::BindMacro(FunctionExpression &function, ScalarMacroCatalogEntry &macro_func, idx_t depth,
                                       unique_ptr<ParsedExpression> &expr) {
	auto stack_checker = StackCheck(*expr, 3);
	// expanding nested macros can take a long time, allow the query to be interrupted
	context.InterruptCheck();

	// unfold the macro expression
	auto &member = UnfoldMacroExpression(function, macro_func, expr, depth);

	// bind the unfolded macro
	auto result = BindExpression(expr, depth);
	if (result.HasError()) {
		return result;
	}
	// enforce a declared RETURNS type by casting the member's result to it
	if (member.return_type.id() != LogicalTypeId::UNKNOWN) {
		result.expression =
		    BoundCastExpression::AddCastToType(context, std::move(result.expression), member.return_type);
	}
	return result;
}

} // namespace duckdb
