#include "duckdb/execution/operator/helper/physical_reset.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/execution/operator/helper/physical_set.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

//! The value the scope reads once the reset is done
static Value GetValueAfterReset(const DBConfig &config, SetScope scope, idx_t setting_index, Value default_value) {
	Value global_value;
	if (scope != SetScope::GLOBAL && config.user_settings.TryGetSetting(setting_index, global_value)) {
		return global_value;
	}
	return default_value;
}

void PhysicalReset::ResetExtensionVariable(ExecutionContext &context, DBConfig &config,
                                           ExtensionOption &extension_option) const {
	auto effective_scope = extension_option.ResolveScope(Identifier(name.ToStdString()), scope);
	auto setting_index = extension_option.setting_index.GetIndex();
	auto reset_value = GetValueAfterReset(config, effective_scope, setting_index, extension_option.default_value);
	if (extension_option.set_function) {
		extension_option.set_function(context.client, effective_scope, reset_value);
	}
	if (extension_option.info) {
		extension_option.info->OnSet(context.client, effective_scope, reset_value, true);
	}
	if (effective_scope == SetScope::GLOBAL) {
		// a value the callback put in place of the default is stored, as on a SET
		config.SetOption(setting_index, std::move(reset_value));
	} else {
		auto &client_config = ClientConfig::GetConfig(context.client);
		client_config.user_settings.ClearSetting(setting_index);
	}
}

SourceResultType PhysicalReset::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                OperatorSourceInput &input) const {
	if (scope == SetScope::VARIABLE) {
		auto &client_config = ClientConfig::GetConfig(context.client);
		client_config.ResetUserVariable(name);
		return SourceResultType::FINISHED;
	}
	auto &config = DBConfig::GetConfig(context.client);
	Identifier option_name(name.ToStdString());
	config.CheckLock(option_name);
	auto option = DBConfig::GetOptionByName(option_name);
	if (!option) {
		// check if this is an extra extension variable
		ExtensionOption extension_option;
		if (!config.TryGetExtensionOption(option_name, extension_option)) {
			auto extension_name = Catalog::AutoloadExtensionByConfigName(context.client, option_name);
			if (!config.TryGetExtensionOption(option_name, extension_option)) {
				throw InvalidInputException("Extension parameter %s was not found after autoloading", name);
			}
		}
		ResetExtensionVariable(context, config, extension_option);
		return SourceResultType::FINISHED;
	}

	// Transform scope
	SetScope variable_scope = PhysicalSet::GetSettingScope(*option, scope);

	if (option->default_value) {
		auto setting_index = option->setting_idx.GetIndex();
		if (option->set_callback) {
			SettingCallbackInfo info(context.client, variable_scope);
			info.is_reset = true;
			auto parameter_type = DBConfig::ParseLogicalType(option->parameter_type);
			auto default_value = Value(option->default_value).CastAs(context.client, parameter_type);
			auto reset_value = GetValueAfterReset(config, variable_scope, setting_index, std::move(default_value));
			option->set_callback(info, reset_value);
		}
		if (variable_scope == SetScope::SESSION) {
			auto &client_config = ClientConfig::GetConfig(context.client);
			client_config.user_settings.ClearSetting(setting_index);
		} else {
			config.ResetGenericOption(setting_index);
		}
		return SourceResultType::FINISHED;
	}
	switch (variable_scope) {
	case SetScope::GLOBAL: {
		if (!option->set_global) {
			throw CatalogException("option \"%s\" cannot be reset globally", name.ToStdString());
		}
		auto &db = DatabaseInstance::GetDatabase(context.client);
		config.ResetOption(&db, *option);
		break;
	}
	case SetScope::SESSION:
		if (!option->reset_local) {
			throw CatalogException("option \"%s\" cannot be reset locally", name.ToStdString());
		}
		option->reset_local(context.client);
		break;
	default:
		throw InternalException("Unsupported SetScope for variable");
	}

	return SourceResultType::FINISHED;
}

} // namespace duckdb
