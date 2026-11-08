//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry_retriever.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <functional>
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/parser/query_error_context.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"

namespace duckdb {

class ClientContext;
class Catalog;
class CatalogEntry;
class MacroFunction;

using catalog_entry_callback_t = std::function<void(CatalogEntry &)>;

//! An entry the statement being bound creates, found where a lookup will find it once the statement has run
struct PendingCatalogEntry {
	PendingCatalogEntry(SchemaCatalogEntry &schema, Identifier name, CatalogType type)
	    : schema(schema), name(std::move(name)), type(type) {
	}

	SchemaCatalogEntry &schema;
	Identifier name;
	CatalogType type;
	//! The entry as it will be stored; while unset, a lookup only records that it would have found it
	optional_ptr<CatalogEntry> entry;
	//! Whether a lookup reached the schema before finding another entry of the name
	bool reached = false;
	//! The member of the entry being bound, whose expansion inside that binding is a cycle
	optional_ptr<const MacroFunction> bound_member;
	//! Whether binding expanded the bound member
	bool bound_member_expanded = false;
};

// Wraps the Catalog::GetEntry method
class CatalogEntryRetriever {
public:
	explicit CatalogEntryRetriever(ClientContext &context) : context(context) {
	}
	CatalogEntryRetriever(const CatalogEntryRetriever &other) : callback(other.callback), context(other.context) {
	}

public:
	void Inherit(const CatalogEntryRetriever &parent);
	ClientContext &GetContext() {
		return context;
	}

	//! Look up an entry described by the (catalog/schema-qualified) EntryLookupInfo
	optional_ptr<CatalogEntry> GetEntry(const EntryLookupInfo &lookup_info,
	                                    OnEntryNotFound on_entry_not_found = OnEntryNotFound::THROW_EXCEPTION);
	//! Look up a (catalog/schema-qualified) type
	LogicalType GetType(const QualifiedName &name, OnEntryNotFound on_entry_not_found = OnEntryNotFound::RETURN_NULL);
	//! Look up the schema described by the (catalog-qualified) EntryLookupInfo
	optional_ptr<SchemaCatalogEntry> GetSchema(const EntryLookupInfo &schema_lookup,
	                                           OnEntryNotFound on_entry_not_found = OnEntryNotFound::THROW_EXCEPTION);

	//! Deprecated: the catalog/schema qualification is now carried inside the EntryLookupInfo / QualifiedName - fold it
	//! in there instead of passing it separately.
	[[deprecated(
	    "Fold catalog/schema into the EntryLookupInfo and use GetEntry(EntryLookupInfo)")]] optional_ptr<CatalogEntry>
	GetEntry(const Identifier &catalog, const Identifier &schema, const EntryLookupInfo &lookup_info,
	         OnEntryNotFound on_entry_not_found = OnEntryNotFound::THROW_EXCEPTION);
	[[deprecated(
	    "Fold the schema into the EntryLookupInfo and use GetEntry(EntryLookupInfo)")]] optional_ptr<CatalogEntry>
	GetEntry(Catalog &catalog, const Identifier &schema, const EntryLookupInfo &lookup_info,
	         OnEntryNotFound on_entry_not_found = OnEntryNotFound::THROW_EXCEPTION);
	[[deprecated("Fold catalog/schema into the QualifiedName and use GetType(QualifiedName)")]] LogicalType
	GetType(const Identifier &catalog, const Identifier &schema, const Identifier &name,
	        OnEntryNotFound on_entry_not_found = OnEntryNotFound::RETURN_NULL);
	[[deprecated("Fold the schema into the QualifiedName and use GetType(QualifiedName)")]] LogicalType
	GetType(Catalog &catalog, const Identifier &schema, const Identifier &name,
	        OnEntryNotFound on_entry_not_found = OnEntryNotFound::RETURN_NULL);
	[[deprecated("Fold the catalog into the EntryLookupInfo and use GetSchema(EntryLookupInfo)")]] optional_ptr<
	    SchemaCatalogEntry>
	GetSchema(const Identifier &catalog, const EntryLookupInfo &schema_lookup,
	          OnEntryNotFound on_entry_not_found = OnEntryNotFound::THROW_EXCEPTION);

	const CatalogSearchPath &GetSearchPath() const;
	void SetSearchPath(vector<CatalogSearchEntry> entries);

	void SetCallback(catalog_entry_callback_t callback);
	catalog_entry_callback_t GetCallback();

	optional_ptr<BoundAtClause> GetAtClause() const;
	void SetAtClause(optional_ptr<BoundAtClause> at_clause);

	optional_ptr<PendingCatalogEntry> GetPendingEntry() const;
	void SetPendingEntry(optional_ptr<PendingCatalogEntry> pending_entry);
	//! Applies the pending entry to the result of a lookup in one schema of the search
	void FindPendingEntry(const EntryLookupInfo &lookup_info, CatalogEntryLookup &result);

private:
	optional_ptr<CatalogEntry> ReturnAndCallback(optional_ptr<CatalogEntry> result);

private:
	//! (optional) callback, called on every successful entry retrieval
	catalog_entry_callback_t callback = nullptr;
	ClientContext &context;
	shared_ptr<CatalogSearchPath> search_path;
	optional_ptr<BoundAtClause> at_clause;
	optional_ptr<PendingCatalogEntry> pending_entry;
};

} // namespace duckdb
