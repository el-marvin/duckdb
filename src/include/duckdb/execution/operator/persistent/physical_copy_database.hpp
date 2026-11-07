//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/persistent/physical_copy_database.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/planner/operator/logical_copy_database.hpp"

namespace duckdb {

class PhysicalCopyDatabase : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::COPY_DATABASE;

public:
	PhysicalCopyDatabase(PhysicalPlan &physical_plan, vector<LogicalType> types, idx_t estimated_cardinality,
	                     unique_ptr<CopyDatabaseInfo> info_p);
	~PhysicalCopyDatabase() override;

	unique_ptr<CopyDatabaseInfo> info;

public:
	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

public:
	// Sink interface
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	bool IsSink() const override {
		return !children.empty();
	}

	bool ParallelSink() const override {
		return false;
	}

private:
	void CreateEntries(ClientContext &client) const;
};

} // namespace duckdb
