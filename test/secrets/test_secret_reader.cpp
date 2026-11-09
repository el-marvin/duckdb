#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_file_opener.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_file_opener.hpp"
#include "duckdb/main/secret/secret.hpp"

#include <functional>

using namespace duckdb;

namespace {

constexpr const char *PROXY_SETTING = "secret_reader_test_proxy";
constexpr const char *TOKEN_SETTING = "secret_reader_test_token";
constexpr const char *FLAG_SETTING = "secret_reader_test_flag";
constexpr const char *UNREGISTERED_SETTING = "secret_reader_test_unregistered";
//! Covered by the http secret `with_proxy`, which sets http_proxy and no bearer_token
constexpr const char *SECRET_PATH = "http://with-proxy/file.csv";
//! Covered only by `connection_proxy`, a secret of the fixture's connection
constexpr const char *CONNECTION_PATH = "http://connection-only/file.csv";
//! Covered by `shared_database` and by `shared_connection`, a secret of the fixture's connection, with one scope
constexpr const char *SHARED_PATH = "http://shared/file.csv";
constexpr const char *NO_SECRET_PATH = "http://no-secret/file.csv";
constexpr const char *NOT_FOUND = "<not found>";

//! A database with http secrets and three extension options, and the four ways to build a secret reader for a path
struct ReaderFixture {
	ReaderFixture() : db(nullptr), con(db), database_opener(*db.instance) {
		auto &config = DBConfig::GetConfig(*db.instance);
		config.AddExtensionOption(PROXY_SETTING, "proxy a read uses when its secret sets none", LogicalType::VARCHAR);
		config.AddExtensionOption(TOKEN_SETTING, "token a read uses when its secret sets none", LogicalType::VARCHAR,
		                          Value("default-token"));
		config.AddExtensionOption(FLAG_SETTING, "flag a read uses when its secret sets none", LogicalType::BOOLEAN);
		REQUIRE_NO_FAIL(
		    con.Query("CREATE SECRET with_proxy (TYPE http, SCOPE 'http://with-proxy/', HTTP_PROXY 'secret-proxy')"));
		REQUIRE_NO_FAIL(con.Query("CREATE SECRET connection_proxy IN connection (TYPE http, SCOPE "
		                          "'http://connection-only/', HTTP_PROXY 'connection-proxy')"));
		REQUIRE_NO_FAIL(con.Query(
		    "CREATE SECRET shared_database (TYPE http, SCOPE 'http://shared/', HTTP_PROXY 'database-proxy')"));
		REQUIRE_NO_FAIL(con.Query("CREATE SECRET shared_connection IN connection (TYPE http, SCOPE 'http://shared/', "
		                          "HTTP_PROXY 'shared-proxy')"));
	}

	//! Calls `read` with a reader for `path` built from the database, its file opener, the client and its file opener
	void ForEachReader(const string &path, const std::function<void(const string &, KeyValueSecretReader &)> &read,
	                   bool in_transaction = true) {
		if (in_transaction) {
			con.BeginTransaction();
		}
		ClientContextFileOpener client_opener(con.context->GetContext());
		FileOpenerInfo info(path);
		{
			KeyValueSecretReader reader(*db.instance, "http", path);
			read("database", reader);
		}
		{
			KeyValueSecretReader reader(database_opener, &info, "http");
			read("database opener", reader);
		}
		{
			KeyValueSecretReader reader(*con.context, "http", path);
			read("client", reader);
		}
		{
			KeyValueSecretReader reader(client_opener, &info, "http");
			read("client opener", reader);
		}
		if (in_transaction) {
			con.Commit();
		}
	}

	//! One "<reader>=<scope>:<value>" per reader, so a failure shows every reader's answer
	string Lookup(const string &path, const Identifier &secret_key, const Identifier &setting,
	              bool in_transaction = true) {
		string result;
		ForEachReader(
		    path,
		    [&](const string &route, KeyValueSecretReader &reader) {
			    Value value;
			    auto lookup = reader.TryGetSecretKeyOrSetting(secret_key, setting, value);
			    auto found = lookup ? string(EnumUtil::ToChars(lookup.GetScope())) + ":" + value.ToString() : NOT_FOUND;
			    result += (result.empty() ? "" : ", ") + route + "=" + found;
		    },
		    in_transaction);
		return result;
	}

	static string Expect(const string &database, const string &client) {
		return "database=" + database + ", database opener=" + database + ", client=" + client +
		       ", client opener=" + client;
	}
	static string Everywhere(const string &found) {
		return Expect(found, found);
	}

	DuckDB db;
	Connection con;
	DatabaseFileOpener database_opener;
};

} // namespace

TEST_CASE("A secret reader built from a database instance reads a global setting, as one built from a client does",
          "[secret]") {
	ReaderFixture f;
	REQUIRE_NO_FAIL(f.con.Query(string("SET GLOBAL ") + PROXY_SETTING + " = 'global-proxy'"));
	REQUIRE(f.Lookup(NO_SECRET_PATH, "http_proxy", PROXY_SETTING) == f.Everywhere("GLOBAL:global-proxy"));
	// a secret that matches the path but lacks the key leaves the setting to fill it
	REQUIRE(f.Lookup(SECRET_PATH, "bearer_token", PROXY_SETTING) == f.Everywhere("GLOBAL:global-proxy"));
}

TEST_CASE("A secret reader of a client reads the client's own value of a setting, one of the database its global value",
          "[secret]") {
	ReaderFixture f;
	REQUIRE_NO_FAIL(f.con.Query(string("SET GLOBAL ") + PROXY_SETTING + " = 'global-proxy'"));
	REQUIRE_NO_FAIL(f.con.Query(string("SET SESSION ") + PROXY_SETTING + " = 'local-proxy'"));
	REQUIRE(f.Lookup(NO_SECRET_PATH, "http_proxy", PROXY_SETTING) ==
	        f.Expect("GLOBAL:global-proxy", "LOCAL:local-proxy"));
}

TEST_CASE("A secret reader reads the default of a setting nobody set, and finds no setting that does not exist",
          "[secret]") {
	ReaderFixture f;
	REQUIRE(f.Lookup(NO_SECRET_PATH, "bearer_token", TOKEN_SETTING) == f.Everywhere("GLOBAL:default-token"));
	REQUIRE(f.Lookup(NO_SECRET_PATH, "bearer_token", "allow_extension_repositories") ==
	        f.Everywhere("GLOBAL:undecided"));
	REQUIRE(f.Lookup(NO_SECRET_PATH, "bearer_token", UNREGISTERED_SETTING) == f.Everywhere(NOT_FOUND));
	REQUIRE(f.Lookup(SECRET_PATH, "bearer_token", UNREGISTERED_SETTING) == f.Everywhere(NOT_FOUND));
}

TEST_CASE("A secret reader takes the key of its secret over a setting, however it was built", "[secret]") {
	ReaderFixture f;
	REQUIRE_NO_FAIL(f.con.Query(string("SET GLOBAL ") + PROXY_SETTING + " = 'global-proxy'"));
	REQUIRE(f.Lookup(SECRET_PATH, "http_proxy", PROXY_SETTING) == f.Everywhere("SECRET:secret-proxy"));
	REQUIRE_NO_FAIL(f.con.Query(string("SET SESSION ") + PROXY_SETTING + " = 'local-proxy'"));
	REQUIRE(f.Lookup(SECRET_PATH, "http_proxy", PROXY_SETTING) == f.Everywhere("SECRET:secret-proxy"));
}

TEST_CASE("A secret reader of a client outside a transaction finds the database's secrets but not the connection's",
          "[secret]") {
	ReaderFixture f;
	REQUIRE(f.Lookup(CONNECTION_PATH, "http_proxy", UNREGISTERED_SETTING) ==
	        f.Expect(NOT_FOUND, "SECRET:connection-proxy"));
	REQUIRE(f.Lookup(CONNECTION_PATH, "http_proxy", UNREGISTERED_SETTING, false) == f.Everywhere(NOT_FOUND));
	REQUIRE(f.Lookup(SECRET_PATH, "http_proxy", UNREGISTERED_SETTING, false) == f.Everywhere("SECRET:secret-proxy"));
	// settings need no transaction, so the client's own value still wins
	REQUIRE_NO_FAIL(f.con.Query(string("SET GLOBAL ") + PROXY_SETTING + " = 'global-proxy'"));
	REQUIRE_NO_FAIL(f.con.Query(string("SET SESSION ") + PROXY_SETTING + " = 'local-proxy'"));
	REQUIRE(f.Lookup(CONNECTION_PATH, "http_proxy", PROXY_SETTING, false) ==
	        f.Expect("GLOBAL:global-proxy", "LOCAL:local-proxy"));
}

TEST_CASE("A connection secret and a database secret of one scope give the connection's inside a transaction, the "
          "database's outside",
          "[secret]") {
	ReaderFixture f;
	REQUIRE(f.Lookup(SHARED_PATH, "http_proxy", UNREGISTERED_SETTING) ==
	        f.Expect("SECRET:database-proxy", "SECRET:shared-proxy"));
	REQUIRE(f.Lookup(SHARED_PATH, "http_proxy", UNREGISTERED_SETTING, false) == f.Everywhere("SECRET:database-proxy"));
}

TEST_CASE("A file read outside a transaction may name a database secret but not a connection secret", "[secret]") {
	ReaderFixture f;
	ClientContextFileOpener client_opener(f.con.context->GetContext());
	for (auto opener : {static_cast<FileOpener *>(&client_opener), static_cast<FileOpener *>(&f.database_opener)}) {
		FileOpenerInfo named_database {OpenFileInfo(SECRET_PATH)};
		named_database.file.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
		named_database.file.extended_info->options[ExtendedOpenFileInfo::SECRET_OPTION] = Value("with_proxy");
		KeyValueSecretReader reader(*opener, &named_database, "http");
		REQUIRE(reader.GetSecretKey("http_proxy").ToString() == "secret-proxy");

		FileOpenerInfo named_connection {OpenFileInfo(CONNECTION_PATH)};
		named_connection.file.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
		named_connection.file.extended_info->options[ExtendedOpenFileInfo::SECRET_OPTION] = Value("connection_proxy");
		REQUIRE_THROWS_WITH(KeyValueSecretReader(*opener, &named_connection, "http"),
		                    Catch::Contains("Secret 'connection_proxy' named by 'http://connection-only/file.csv' not "
		                                    "found"));
	}
}

TEST_CASE("A secret reader returns the fallback only when neither its secret nor a setting has the value", "[secret]") {
	ReaderFixture f;
	REQUIRE_NO_FAIL(f.con.Query(string("SET GLOBAL ") + PROXY_SETTING + " = 'global-proxy'"));
	for (auto path : {NO_SECRET_PATH, SECRET_PATH}) {
		f.ForEachReader(path, [&](const string &route, KeyValueSecretReader &reader) {
			INFO(route << " reading " << path);
			REQUIRE(reader.GetSecretKeyOrSettingOrDefault<string>("bearer_token", UNREGISTERED_SETTING, "fallback") ==
			        "fallback");
			REQUIRE(reader.GetSecretKeyOrSettingOrDefault<string>("bearer_token", PROXY_SETTING, "fallback") ==
			        "global-proxy");
			REQUIRE(reader.GetSecretKeyOrSettingOrDefault<string>("bearer_token", TOKEN_SETTING, "fallback") ==
			        "default-token");
		});
	}
}

TEST_CASE("A secret reader returns the fallback for a setting set to NULL", "[secret]") {
	ReaderFixture f;
	REQUIRE_NO_FAIL(f.con.Query(string("SET GLOBAL ") + PROXY_SETTING + " = NULL"));
	REQUIRE_NO_FAIL(f.con.Query(string("SET GLOBAL ") + FLAG_SETTING + " = NULL"));
	REQUIRE(f.Lookup(NO_SECRET_PATH, "http_proxy", PROXY_SETTING) == f.Everywhere("GLOBAL:NULL"));
	f.ForEachReader(NO_SECRET_PATH, [&](const string &route, KeyValueSecretReader &reader) {
		INFO(route);
		REQUIRE(reader.GetSecretKeyOrSettingOrDefault<string>("http_proxy", PROXY_SETTING, "fallback") == "fallback");
		REQUIRE(reader.GetSecretKeyOrSettingOrDefault<bool>("verify_ssl", FLAG_SETTING, true));
		REQUIRE(!reader.GetSecretKeyOrSettingOrDefault<bool>("verify_ssl", FLAG_SETTING, false));
		string untouched = "untouched";
		auto found = reader.TryGetSecretKeyOrSetting<string>("http_proxy", PROXY_SETTING, untouched);
		REQUIRE(found.GetScope() == SettingScope::GLOBAL);
		REQUIRE(untouched == "untouched");
	});
}

TEST_CASE("A secret reader asked for a value that neither its secret nor a setting has throws", "[secret]") {
	ReaderFixture f;
	REQUIRE_NO_FAIL(f.con.Query(string("SET GLOBAL ") + PROXY_SETTING + " = 'global-proxy'"));
	f.ForEachReader(NO_SECRET_PATH, [&](const string &route, KeyValueSecretReader &reader) {
		INFO(route);
		REQUIRE(reader.GetSecretKeyOrSetting("bearer_token", PROXY_SETTING).ToString() == "global-proxy");
		REQUIRE_THROWS_AS(reader.GetSecretKeyOrSetting("bearer_token", UNREGISTERED_SETTING),
		                  InvalidConfigurationException);
		REQUIRE_THROWS_WITH(reader.GetSecretKeyOrSetting("bearer_token", UNREGISTERED_SETTING),
		                    Catch::Contains("no secret was found for 'http://no-secret/file.csv'"));
	});
	f.ForEachReader(SECRET_PATH, [&](const string &route, KeyValueSecretReader &reader) {
		INFO(route);
		REQUIRE_THROWS_WITH(reader.GetSecretKeyOrSetting("bearer_token", UNREGISTERED_SETTING),
		                    Catch::Contains("did not contain the key"));
	});
}
