#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_copy_database.hpp"
#include "duckdb/execution/operator/persistent/physical_copy_database.hpp"

namespace duckdb {

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalCopyDatabase &op) {
	auto &copy_database = Make<PhysicalCopyDatabase>(op.types, op.estimated_cardinality, std::move(op.info));
	if (!op.children.empty()) {
		D_ASSERT(op.children.size() == 1);
		auto &plan = CreatePlan(*op.children[0]);
		copy_database.children.push_back(plan);
	}
	return copy_database;
}

} // namespace duckdb
