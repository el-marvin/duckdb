#include "duckdb/execution/operator/persistent/physical_copy_database.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_trigger_info.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/execution/index/unbound_index.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/column/column_data_scan_states.hpp"

namespace duckdb {

PhysicalCopyDatabase::PhysicalCopyDatabase(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                           idx_t estimated_cardinality, unique_ptr<CopyDatabaseInfo> info_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::COPY_DATABASE, std::move(types), estimated_cardinality),
      info(std::move(info_p)) {
}

PhysicalCopyDatabase::~PhysicalCopyDatabase() {
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class CopyDatabaseGlobalSinkState : public GlobalSinkState {
public:
	CopyDatabaseGlobalSinkState(ClientContext &context, const vector<LogicalType> &types) : rows(context, types) {
	}

	ColumnDataCollection rows;
};

unique_ptr<GlobalSinkState> PhysicalCopyDatabase::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<CopyDatabaseGlobalSinkState>(context, types);
}

SinkResultType PhysicalCopyDatabase::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<CopyDatabaseGlobalSinkState>();
	gstate.rows.Append(chunk);
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class CopyDatabaseSourceState : public GlobalSourceState {
public:
	bool entries_created = false;
	ColumnDataScanState scan_state;
};

unique_ptr<GlobalSourceState> PhysicalCopyDatabase::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<CopyDatabaseSourceState>();
}

SourceResultType PhysicalCopyDatabase::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                       OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<CopyDatabaseSourceState>();
	if (!state.entries_created) {
		CreateEntries(context.client);
		state.entries_created = true;
		if (!IsSink()) {
			return SourceResultType::FINISHED;
		}
		sink_state->Cast<CopyDatabaseGlobalSinkState>().rows.InitializeScan(state.scan_state);
	}
	// the data step returns the same rows whether or not it creates triggers after its inserts
	sink_state->Cast<CopyDatabaseGlobalSinkState>().rows.Scan(state.scan_state, chunk);
	return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

void PhysicalCopyDatabase::CreateEntries(ClientContext &client) const {
	auto &catalog = Catalog::GetCatalog(client, info->target_database);
	for (auto &create_info : info->entries) {
		switch (create_info->type) {
		case CatalogType::SCHEMA_ENTRY:
			catalog.CreateSchema(client, create_info->Cast<CreateSchemaInfo>());
			break;
		case CatalogType::VIEW_ENTRY:
			catalog.CreateView(client, create_info->Cast<CreateViewInfo>());
			break;
		case CatalogType::SEQUENCE_ENTRY:
			catalog.CreateSequence(client, create_info->Cast<CreateSequenceInfo>());
			break;
		case CatalogType::TYPE_ENTRY:
			catalog.CreateType(client, create_info->Cast<CreateTypeInfo>());
			break;
		case CatalogType::MACRO_ENTRY:
		case CatalogType::TABLE_MACRO_ENTRY:
			catalog.CreateFunction(client, create_info->Cast<CreateMacroInfo>());
			break;
		case CatalogType::TABLE_ENTRY: {
			auto binder = Binder::CreateBinder(client);
			auto bound_info = binder->BindCreateTableInfo(std::move(create_info));
			catalog.CreateTable(client, *bound_info);
			break;
		}
		case CatalogType::INDEX_ENTRY: {
			// Skip for now.
			break;
		}
		case CatalogType::TRIGGER_ENTRY: {
			auto &trigger_info = create_info->Cast<CreateTriggerInfo>();
			auto binder = Binder::CreateBinder(client);
			// the body binds as CREATE TRIGGER binds it in a session on the target, as IMPORT recreates it
			binder->SetSearchPath(catalog, Identifier(DEFAULT_SCHEMA));
			binder->BindCreateTriggerInfo(trigger_info);
			// the stored ON clause names the table within its database, not under the alias the copy used
			auto on_table = trigger_info.base_table->GetQualifiedName();
			on_table.StripCatalog();
			trigger_info.base_table->SetQualifiedName(on_table);
			auto &table = catalog.GetEntry<TableCatalogEntry>(
			    client, trigger_info.GetQualifiedName().WithName(trigger_info.base_table->Table()));
			table.CreateTrigger(catalog.GetCatalogTransaction(client), trigger_info);
			break;
		}
		default:
			throw NotImplementedException("Entry type %s not supported in PhysicalCopyDatabase",
			                              CatalogTypeToString(create_info->type));
		}
	}

	// Create the indexes after table creation.
	for (auto &create_info : info->entries) {
		if (!create_info || create_info->type != CatalogType::INDEX_ENTRY) {
			continue;
		}
		auto index_entry = catalog.CreateIndex(client, create_info->Cast<CreateIndexInfo>());
		D_ASSERT(index_entry);

		auto &create_index_info = create_info->Cast<CreateIndexInfo>();
		auto &table_entry = catalog.GetEntry<TableCatalogEntry>(
		    client,
		    QualifiedName(catalog.GetName(), create_index_info.GetQualifiedName().Schema(), create_index_info.table));
		auto &data_table = table_entry.GetStorage();

		IndexStorageInfo storage_info(create_index_info.GetIndexName());
		storage_info.options.emplace("v1_0_0_storage", false);
		auto unbound_index = make_uniq<UnboundIndex>(create_index_info.Copy(), std::move(storage_info),
		                                             data_table.GetTableIOManager(), catalog.GetAttached());
		data_table.AddIndex(std::move(unbound_index), index_entry->oid);

		// We add unbound indexes, so we immediately bind them.
		// Otherwise, WAL serialization fails due to unbound indexes.
		auto &data_table_info = *data_table.GetDataTableInfo();
		data_table_info.BindIndexes(client);
	}
}

} // namespace duckdb
