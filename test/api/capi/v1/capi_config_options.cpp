#include "capi_tester.hpp"

using namespace duckdb;

TEST_CASE("Test Custom Configuration Options in C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	// Create a connection and get client context
	REQUIRE(tester.OpenDatabase(nullptr));
	duckdb_client_context context;
	duckdb_connection_get_client_context(tester.connection, &context);
	REQUIRE(context != nullptr);

	// Setup some types for config options
	auto str_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	auto int_type = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);

	auto default_str_value = duckdb_create_varchar("default_string");
	auto default_int_value = duckdb_create_int32(42);

	// Test 1: Create a string config option
	auto str_opt = duckdb_create_config_option();
	REQUIRE(str_opt != nullptr);
	duckdb_config_option_set_name(str_opt, "test_string_option");
	duckdb_config_option_set_type(str_opt, str_type);
	duckdb_config_option_set_description(str_opt, "A test string configuration option");
	duckdb_config_option_set_default_value(str_opt, default_str_value);
	REQUIRE(duckdb_register_config_option(tester.connection, str_opt) == DuckDBSuccess);

	// Get and verify the string option
	result = tester.Query("SELECT current_setting('test_string_option')");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<string>(0, 0) == "default_string");

	// Set and verify the string option
	REQUIRE_NO_FAIL(tester.Query("SET test_string_option = 'new_value'"));
	result = tester.Query("SELECT current_setting('test_string_option')");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<string>(0, 0) == "new_value");

	// Also get it from the client context
	duckdb_config_option_scope scope;
	auto retrieved_str = duckdb_client_context_get_config_option(context, "test_string_option", &scope);
	REQUIRE(retrieved_str != nullptr);
	duckdb_destroy_value(&retrieved_str);

	// By default, the scope should be SESSION
	REQUIRE(scope == DUCKDB_CONFIG_OPTION_SCOPE_SESSION);

	duckdb_destroy_config_option(&str_opt);
	duckdb_destroy_config_option(&str_opt);

	// Fetch it from the duckdb_settings() function
	result =
	    tester.Query("SELECT name, value, input_type, scope FROM duckdb_settings() WHERE name = 'test_string_option'");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->ChunkCount() == 1);
	REQUIRE(result->Fetch<string>(0, 0) == "test_string_option");
	REQUIRE(result->Fetch<string>(1, 0) == "new_value");
	REQUIRE(result->Fetch<string>(2, 0) == "VARCHAR");
	REQUIRE(result->Fetch<string>(3, 0) == "LOCAL");

	// A session reset drops the session value, so the global value shows again
	REQUIRE_NO_FAIL(tester.Query("SET GLOBAL test_string_option = 'global_value'"));
	REQUIRE_NO_FAIL(tester.Query("RESET test_string_option"));
	result = tester.Query("SELECT current_setting('test_string_option')");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<string>(0, 0) == "global_value");
	retrieved_str = duckdb_client_context_get_config_option(context, "test_string_option", &scope);
	REQUIRE(retrieved_str != nullptr);
	auto global_str = duckdb_get_varchar(retrieved_str);
	REQUIRE(string(global_str) == "global_value");
	duckdb_free(global_str);
	duckdb_destroy_value(&retrieved_str);
	REQUIRE(scope == DUCKDB_CONFIG_OPTION_SCOPE_GLOBAL);

	// A global reset restores the default value
	REQUIRE_NO_FAIL(tester.Query("RESET GLOBAL test_string_option"));
	result = tester.Query("SELECT current_setting('test_string_option')");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<string>(0, 0) == "default_string");

	// Test 2: Create an integer config option, with the type inferred from the default value
	auto int_opt = duckdb_create_config_option();
	REQUIRE(int_opt != nullptr);
	duckdb_config_option_set_name(int_opt, "test_integer_option");
	duckdb_config_option_set_description(int_opt, "A test integer configuration option");
	duckdb_config_option_set_default_value(int_opt, default_int_value);
	duckdb_config_option_set_default_scope(int_opt, DUCKDB_CONFIG_OPTION_SCOPE_GLOBAL);
	REQUIRE(duckdb_register_config_option(tester.connection, int_opt) == DuckDBSuccess);

	// Get and verify the integer option
	result = tester.Query("SELECT current_setting('test_integer_option')");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int32_t>(0, 0) == 42);

	// Also get it from the client context. Try without specifying scope first
	auto retrieved_int = duckdb_client_context_get_config_option(context, "test_integer_option", nullptr);
	REQUIRE(retrieved_int != nullptr);
	REQUIRE(duckdb_get_int32(retrieved_int) == 42);
	duckdb_destroy_value(&retrieved_int);

	// By default, the scope should be GLOBAL, as that was what we defined this option with
	retrieved_int = duckdb_client_context_get_config_option(context, "test_integer_option", &scope);
	REQUIRE(retrieved_int != nullptr);
	duckdb_destroy_value(&retrieved_int);

	// Also check in duckdb_settings()
	result =
	    tester.Query("SELECT name, value, input_type, scope FROM duckdb_settings() WHERE name = 'test_integer_option'");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->ChunkCount() == 1);
	REQUIRE(result->Fetch<string>(0, 0) == "test_integer_option");
	REQUIRE(result->Fetch<string>(1, 0) == "42");
	REQUIRE(result->Fetch<string>(2, 0) == "INTEGER");
	REQUIRE(result->Fetch<string>(3, 0) == "GLOBAL");

	duckdb_destroy_config_option(&int_opt);
	duckdb_destroy_config_option(&int_opt);

	// Test 3: an option without a default value reads as a NULL of its type until it is set
	auto null_opt = duckdb_create_config_option();
	REQUIRE(null_opt != nullptr);
	duckdb_config_option_set_name(null_opt, "test_null_default_option");
	duckdb_config_option_set_type(null_opt, str_type);
	REQUIRE(duckdb_register_config_option(tester.connection, null_opt) == DuckDBSuccess);
	duckdb_destroy_config_option(&null_opt);

	// expected_value empty means NULL
	auto check_null_default = [&](const string &expected_value, duckdb_config_option_scope expected_scope) {
		auto expect_null = expected_value.empty();
		result = tester.Query("SELECT current_setting('test_null_default_option') IS NULL, "
		                      "typeof(current_setting('test_null_default_option')), "
		                      "coalesce(current_setting('test_null_default_option'), '')");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Fetch<bool>(0, 0) == expect_null);
		REQUIRE(result->Fetch<string>(1, 0) == "VARCHAR");
		REQUIRE(result->Fetch<string>(2, 0) == expected_value);
		auto value = duckdb_client_context_get_config_option(context, "test_null_default_option", &scope);
		REQUIRE(value != nullptr);
		REQUIRE(duckdb_is_null_value(value) == expect_null);
		if (!expect_null) {
			auto text = duckdb_get_varchar(value);
			REQUIRE(string(text) == expected_value);
			duckdb_free(text);
		}
		duckdb_destroy_value(&value);
		REQUIRE(scope == expected_scope);
	};
	check_null_default("", DUCKDB_CONFIG_OPTION_SCOPE_GLOBAL);
	REQUIRE_NO_FAIL(tester.Query("SET test_null_default_option = 'session_value'"));
	check_null_default("session_value", DUCKDB_CONFIG_OPTION_SCOPE_SESSION);
	REQUIRE_NO_FAIL(tester.Query("RESET test_null_default_option"));
	check_null_default("", DUCKDB_CONFIG_OPTION_SCOPE_GLOBAL);
	REQUIRE_NO_FAIL(tester.Query("SET GLOBAL test_null_default_option = 'global_value'"));
	check_null_default("global_value", DUCKDB_CONFIG_OPTION_SCOPE_GLOBAL);
	REQUIRE_NO_FAIL(tester.Query("RESET GLOBAL test_null_default_option"));
	check_null_default("", DUCKDB_CONFIG_OPTION_SCOPE_GLOBAL);

	// Cleanup
	duckdb_destroy_client_context(&context);
	duckdb_destroy_logical_type(&str_type);
	duckdb_destroy_logical_type(&int_type);

	duckdb_destroy_value(&default_str_value);
	duckdb_destroy_value(&default_int_value);
}

TEST_CASE("Issue #25056: duckdb_set_config crashes on a boolean option set to false", "[capi]") {
	// debug_force_mbedtls_unsafe consults the attached databases when it is disabled, but there is no
	// database instance yet when the option is set on a config
	for (const auto &alias : {"force_mbedtls_unsafe", "debug_force_mbedtls_unsafe"}) {
		for (const auto &value : {"true", "false", "1", "0"}) {
			duckdb_config config;
			REQUIRE(duckdb_create_config(&config) == DuckDBSuccess);
			REQUIRE(duckdb_set_config(config, alias, value) == DuckDBSuccess);
			duckdb_destroy_config(&config);
		}
	}
}
