#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/json_document.hpp"
#include "catch.hpp"

#include "duckdb/common/vector.hpp"
#include <string>
#include <cstring>

using namespace duckdb;

TEST_CASE("Test strcmp() to ensure platform sanity", "[comparison]") {
	int res;
	res = strcmp("ZZZ", "ZZZ");
	REQUIRE(res == 0);

	res = strcmp("ZZZ", "HXR");
	REQUIRE(res > 0);

	res = strcmp("ZZZ", "NUT");
	REQUIRE(res > 0);

	res = strcmp("HXR", "ZZZ");
	REQUIRE(res < 0);

	res = strcmp("HXR", "HXR");
	REQUIRE(res == 0);

	res = strcmp("HXR", "NUT");
	REQUIRE(res < 0);

	res = strcmp("NUT", "ZZZ");
	REQUIRE(res < 0);

	res = strcmp("NUT", "HXR");
	REQUIRE(res > 0);

	res = strcmp("NUT", "NUT");
	REQUIRE(res == 0);

	Value zzz("ZZZ");
	Value hxr("HXR");
	Value nut("NUT");

	REQUIRE_FALSE(zzz > zzz);
	REQUIRE(zzz > hxr);
	REQUIRE(zzz > nut);

	REQUIRE(zzz >= zzz);
	REQUIRE(zzz >= hxr);
	REQUIRE(zzz >= nut);

	REQUIRE(zzz <= zzz);
	REQUIRE_FALSE(zzz <= hxr);
	REQUIRE_FALSE(zzz <= nut);

	REQUIRE(zzz == zzz);
	REQUIRE_FALSE(zzz == hxr);
	REQUIRE_FALSE(zzz == nut);

	REQUIRE_FALSE(zzz != zzz);
	REQUIRE(zzz != hxr);
	REQUIRE(zzz != nut);

	REQUIRE_FALSE(hxr > zzz);
	REQUIRE_FALSE(hxr > hxr);
	REQUIRE_FALSE(hxr > nut);

	REQUIRE_FALSE(hxr >= zzz);
	REQUIRE(hxr >= hxr);
	REQUIRE_FALSE(hxr >= nut);

	REQUIRE(hxr <= zzz);
	REQUIRE(hxr <= hxr);
	REQUIRE(hxr <= nut);

	REQUIRE_FALSE(hxr == zzz);
	REQUIRE(hxr == hxr);
	REQUIRE_FALSE(hxr == nut);

	REQUIRE(hxr != zzz);
	REQUIRE_FALSE(hxr != hxr);
	REQUIRE(hxr != nut);

	REQUIRE_FALSE(nut > zzz);
	REQUIRE(nut > hxr);
	REQUIRE_FALSE(nut > nut);

	REQUIRE_FALSE(nut >= zzz);
	REQUIRE(nut >= hxr);
	REQUIRE(nut >= nut);

	REQUIRE(nut <= zzz);
	REQUIRE_FALSE(nut <= hxr);
	REQUIRE(nut <= nut);

	REQUIRE_FALSE(nut == zzz);
	REQUIRE_FALSE(nut == hxr);
	REQUIRE(nut == nut);

	REQUIRE(nut != zzz);
	REQUIRE(nut != hxr);
	REQUIRE_FALSE(nut != nut);
}

TEST_CASE("Test common prefix size", "[string_util]") {
	REQUIRE(StringUtil::GetCommonPrefixSize("", "") == 0);
	REQUIRE(StringUtil::GetCommonPrefixSize("abc", "") == 0);
	REQUIRE(StringUtil::GetCommonPrefixSize("abc", "xyz") == 0);
	REQUIRE(StringUtil::GetCommonPrefixSize("abc", "abc") == 3);
	REQUIRE(StringUtil::GetCommonPrefixSize("abc", "abcdef") == 3);
	REQUIRE(StringUtil::GetCommonPrefixSize("abcdef", "abcxyz") == 3);
	REQUIRE(StringUtil::GetCommonPrefixSize("🦆ab", "🦆ac") == 5);
}

TEST_CASE("Test next prefix", "[string_util]") {
	string prefix = "abc";
	REQUIRE(StringUtil::FindNextPrefix(prefix));
	REQUIRE(prefix == "abd");

	prefix = "a";
	prefix.push_back(static_cast<char>(0xFF));
	prefix.push_back(static_cast<char>(0xFF));
	REQUIRE(StringUtil::FindNextPrefix(prefix));
	REQUIRE(prefix == "b");

	prefix.assign(2, static_cast<char>(0xFF));
	REQUIRE_FALSE(StringUtil::FindNextPrefix(prefix));
	REQUIRE(prefix == string(2, static_cast<char>(0xFF)));

	prefix.clear();
	REQUIRE_FALSE(StringUtil::FindNextPrefix(prefix));
}

TEST_CASE("Test join vector items", "[string_util]") {
	SECTION("Three string items") {
		duckdb::vector<std::string> str_items = {"abc", "def", "ghi"};
		std::string result = StringUtil::Join(str_items, ",");
		REQUIRE(result == "abc,def,ghi");
	}

	SECTION("One string item") {
		duckdb::vector<std::string> str_items = {"abc"};
		std::string result = StringUtil::Join(str_items, ",");
		REQUIRE(result == "abc");
	}

	SECTION("No string items") {
		duckdb::vector<std::string> str_items;
		std::string result = StringUtil::Join(str_items, ",");
		REQUIRE(result == "");
	}

	SECTION("Three int items") {
		duckdb::vector<int> int_items = {1, 2, 3};
		std::string result =
		    StringUtil::Join(int_items, int_items.size(), ", ", [](const int &item) { return to_string(item); });
		REQUIRE(result == "1, 2, 3");
	}

	SECTION("One int item") {
		duckdb::vector<int> int_items = {1};
		std::string result =
		    StringUtil::Join(int_items, int_items.size(), ", ", [](const int &item) { return to_string(item); });
		REQUIRE(result == "1");
	}

	SECTION("No int items") {
		duckdb::vector<int> int_items;
		std::string result =
		    StringUtil::Join(int_items, int_items.size(), ", ", [](const int &item) { return to_string(item); });
		REQUIRE(result == "");
	}
}

TEST_CASE("Test replace strings", "[string_util]") {
	REQUIRE(StringUtil::Replace("abcabc", "ab", "x") == "xcxc");
	REQUIRE(StringUtil::Replace("aaaa", "aa", "b") == "bb");
	REQUIRE(StringUtil::Replace("xx", "x", "yx") == "yxyx");
	REQUIRE(StringUtil::Replace("aaa{SNAPSHOT_ID}bbb{SNAPSHOT_ID}", "{SNAPSHOT_ID}", "1") == "aaa1bbb1");
	REQUIRE(StringUtil::Replace("", "x", "y") == "");

// Replace throws an InternalException if the search string is empty.
// In CI, when DUCKDB_CRASH_ON_ASSERT is set this fails the test even when
// surrounded by REQUIRE_THROWS
#ifndef DUCKDB_CRASH_ON_ASSERT
	REQUIRE_THROWS(StringUtil::Replace("abc", "", "x"));
#endif
}

TEST_CASE("Test SplitWithParentheses", "[string_util]") {
	SECTION("Standard split") {
		REQUIRE(StringUtil::SplitWithParentheses("") == duckdb::vector<string> {});
		REQUIRE(StringUtil::SplitWithParentheses("x") == duckdb::vector<string> {"x"});
		REQUIRE(StringUtil::SplitWithParentheses("hello") == duckdb::vector<string> {"hello"});
		REQUIRE(StringUtil::SplitWithParentheses("hello,world") == duckdb::vector<string> {"hello", "world"});
	}

	SECTION("Single item with parentheses") {
		REQUIRE(StringUtil::SplitWithParentheses("STRUCT(year BIGINT, month BIGINT, day BIGINT)") ==
		        duckdb::vector<string> {"STRUCT(year BIGINT, month BIGINT, day BIGINT)"});
		REQUIRE(StringUtil::SplitWithParentheses("(apple)") == duckdb::vector<string> {"(apple)"});
		REQUIRE(StringUtil::SplitWithParentheses("(apple, pear)") == duckdb::vector<string> {"(apple, pear)"});
		REQUIRE(StringUtil::SplitWithParentheses("(apple, pear) banana") ==
		        duckdb::vector<string> {"(apple, pear) banana"});
		REQUIRE(StringUtil::SplitWithParentheses("banana (apple, pear)") ==
		        duckdb::vector<string> {"banana (apple, pear)"});
		REQUIRE(StringUtil::SplitWithParentheses("banana (apple, pear) banana") ==
		        duckdb::vector<string> {"banana (apple, pear) banana"});
	}

	SECTION("Multiple items with parentheses") {
		REQUIRE(StringUtil::SplitWithParentheses("map::MAP(ANY,ANY),key::ANY") ==
		        duckdb::vector<string> {"map::MAP(ANY,ANY)", "key::ANY"});
		REQUIRE(StringUtil::SplitWithParentheses("extra,STRUCT(year BIGINT, month BIGINT, day BIGINT)") ==
		        duckdb::vector<string> {"extra", "STRUCT(year BIGINT, month BIGINT, day BIGINT)"});
		REQUIRE(StringUtil::SplitWithParentheses("extra,STRUCT(year BIGINT, month BIGINT, day BIGINT),extra") ==
		        duckdb::vector<string> {"extra", "STRUCT(year BIGINT, month BIGINT, day BIGINT)", "extra"});
		REQUIRE(StringUtil::SplitWithParentheses("aa(bb)cc,dd(ee)ff") ==
		        duckdb::vector<string> {"aa(bb)cc", "dd(ee)ff"});
		REQUIRE(StringUtil::SplitWithParentheses("aa(bb cc,dd),ee(f,,f)gg") ==
		        duckdb::vector<string> {"aa(bb cc,dd)", "ee(f,,f)gg"});
	}

	SECTION("Leading and trailing separators") {
		REQUIRE(StringUtil::SplitWithParentheses(",") == duckdb::vector<string> {""});
		REQUIRE(StringUtil::SplitWithParentheses(",,") == duckdb::vector<string> {"", ""});
		REQUIRE(StringUtil::SplitWithParentheses("aa,") == duckdb::vector<string> {"aa"});
		REQUIRE(StringUtil::SplitWithParentheses(",aa") == duckdb::vector<string> {"", "aa"});
		REQUIRE(StringUtil::SplitWithParentheses(",(aa,),") == duckdb::vector<string> {"", "(aa,)"});
	}

	SECTION("Leading and trailing spaces") {
		REQUIRE(StringUtil::SplitWithParentheses(" ") == duckdb::vector<string> {" "});
		REQUIRE(StringUtil::SplitWithParentheses("   ") == duckdb::vector<string> {"   "});
		REQUIRE(StringUtil::SplitWithParentheses("   , ") == duckdb::vector<string> {"   ", " "});
		REQUIRE(StringUtil::SplitWithParentheses("aa, bb") == duckdb::vector<string> {"aa", " bb"});
		REQUIRE(StringUtil::SplitWithParentheses(" aa,(bb, cc) ") == duckdb::vector<string> {" aa", "(bb, cc) "});
	}

	SECTION("Nested parentheses") {
		REQUIRE(StringUtil::SplitWithParentheses("STRUCT(aa BIGINT, bb STRUCT(cc BIGINT, dd BIGINT, BIGINT))") ==
		        duckdb::vector<string> {"STRUCT(aa BIGINT, bb STRUCT(cc BIGINT, dd BIGINT, BIGINT))"});
		REQUIRE(StringUtil::SplitWithParentheses("(((aa)))") == duckdb::vector<string> {"(((aa)))"});
		REQUIRE(StringUtil::SplitWithParentheses("((aa, bb))") == duckdb::vector<string> {"((aa, bb))"});
		REQUIRE(StringUtil::SplitWithParentheses("aa,(bb,(cc,dd)),ee") ==
		        duckdb::vector<string> {"aa", "(bb,(cc,dd))", "ee"});
	}

	SECTION("other parentheses") {
		REQUIRE(StringUtil::SplitWithParentheses(" aa,[bb, cc] )", ',', '[', ']') ==
		        duckdb::vector<string> {" aa", "[bb, cc] )"});
	}

	SECTION("other separators") {
		REQUIRE(StringUtil::SplitWithParentheses(" aa|(bb| cc),dd", '|') ==
		        duckdb::vector<string> {" aa", "(bb| cc),dd"});
	}

	SECTION("incongruent parentheses") {
		REQUIRE_THROWS(StringUtil::SplitWithParentheses("("));
		REQUIRE_THROWS(StringUtil::SplitWithParentheses(")"));
		REQUIRE_THROWS(StringUtil::SplitWithParentheses("aa(bb"));
		REQUIRE_THROWS(StringUtil::SplitWithParentheses("aa)bb"));
		REQUIRE_THROWS(StringUtil::SplitWithParentheses("(aa)bb)"));
		REQUIRE_THROWS(StringUtil::SplitWithParentheses("(aa(bb)"));
	}
}

TEST_CASE("Test split quoted strings", "[string_util]") {
	SECTION("Empty string") {
		REQUIRE(StringUtil::SplitWithQuote("") == duckdb::vector<string> {});
	}

	SECTION("Empty string with space") {
		REQUIRE(StringUtil::SplitWithQuote(" ") == duckdb::vector<string> {});
	}

	SECTION("One item") {
		REQUIRE(StringUtil::SplitWithQuote("x") == duckdb::vector<string> {"x"});
	}

	SECTION("One item with space") {
		REQUIRE(StringUtil::SplitWithQuote(" x ") == duckdb::vector<string> {"x"});
	}

	SECTION("One item with quote") {
		REQUIRE(StringUtil::SplitWithQuote("\"x\"") == duckdb::vector<string> {"x"});
	}

	SECTION("One empty item with quote") {
		REQUIRE(StringUtil::SplitWithQuote("\"\"") == duckdb::vector<string> {""});
	}

	SECTION("One empty item, followed by non-empty one - Or vise versa") {
		REQUIRE(StringUtil::SplitWithQuote("\"\",hello") == duckdb::vector<string> {"", "hello"});
		REQUIRE(StringUtil::SplitWithQuote(",\"hello\"") == duckdb::vector<string> {"", "hello"});
		REQUIRE(StringUtil::SplitWithQuote(",hello") == duckdb::vector<string> {"", "hello"});
		REQUIRE(StringUtil::SplitWithQuote("\"\",\"hello\"") == duckdb::vector<string> {"", "hello"});

		REQUIRE(StringUtil::SplitWithQuote("\"hello\",") == duckdb::vector<string> {"hello", ""});
		REQUIRE(StringUtil::SplitWithQuote("hello,\"\"") == duckdb::vector<string> {"hello", ""});
		REQUIRE(StringUtil::SplitWithQuote("hello,") == duckdb::vector<string> {"hello", ""});
		REQUIRE(StringUtil::SplitWithQuote("\"hello\",\"\"") == duckdb::vector<string> {"hello", ""});
	}

	SECTION("One quoted item with spaces") {
		REQUIRE(StringUtil::SplitWithQuote(" \" x y \" ") == duckdb::vector<string> {" x y "});
	}

	SECTION("One quoted item with a delimiter") {
		REQUIRE(StringUtil::SplitWithQuote("\"x,y\"") == duckdb::vector<string> {"x,y"});
	}

	SECTION("Three items") {
		REQUIRE(StringUtil::SplitWithQuote("x,y,z") == duckdb::vector<string> {"x", "y", "z"});
	}

	SECTION("Three items, with and without quote") {
		REQUIRE(StringUtil::SplitWithQuote("x,\"y\",z") == duckdb::vector<string> {"x", "y", "z"});
	}

	SECTION("Even more items, with and without quote") {
		REQUIRE(StringUtil::SplitWithQuote("a,b,c,d,e,f,g") ==
		        duckdb::vector<string> {"a", "b", "c", "d", "e", "f", "g"});
	}

	SECTION("Three empty items") {
		REQUIRE(StringUtil::SplitWithQuote(",,") == duckdb::vector<string> {"", "", ""});
	}

	SECTION("Three empty quoted items") {
		REQUIRE(StringUtil::SplitWithQuote("\"\",\"\",\"\"") == duckdb::vector<string> {"", "", ""});
	}

	SECTION("Unclosed quote") {
		REQUIRE_THROWS_AS(StringUtil::SplitWithQuote("\""), ParserException);
		REQUIRE_THROWS_AS(StringUtil::SplitWithQuote("\"x"), ParserException);
		REQUIRE_THROWS_AS(StringUtil::SplitWithQuote("\"x "), ParserException);
		REQUIRE_THROWS_AS(StringUtil::SplitWithQuote("\","), ParserException);
		REQUIRE_THROWS_AS(StringUtil::SplitWithQuote("\"x,"), ParserException);
	}

	SECTION("Unexpected quote") {
		REQUIRE_THROWS_AS(StringUtil::SplitWithQuote("abc\"def"), ParserException);
	}

	SECTION("Missing delimiter") {
		REQUIRE_THROWS_AS(StringUtil::SplitWithQuote("\"x\"\"y\""), ParserException);
		REQUIRE_THROWS_AS(StringUtil::SplitWithQuote("\"x\" \"y\""), ParserException);
		REQUIRE_THROWS_AS(StringUtil::SplitWithQuote("x y"), ParserException);
	}
}

TEST_CASE("Test RTrim preserves trailing UTF-8", "[string_util]") {
	string value = "abcé";
	StringUtil::RTrim(value);
	REQUIRE(value == "abcé");

	value = "abcé   ";
	StringUtil::RTrim(value);
	REQUIRE(value == "abcé");
}

TEST_CASE("Test custom RTrim preserves UTF-8 paths", "[string_util]") {
	string path = "/tmp/café";
	StringUtil::RTrim(path, "/");
	REQUIRE(path == "/tmp/café");

	path = "/tmp/café///";
	StringUtil::RTrim(path, "/");
	REQUIRE(path == "/tmp/café");
}

TEST_CASE("Test path utilities", "[string_util]") {
	SECTION("File name") {
		REQUIRE("bin" == StringUtil::GetFileName("/usr/bin/"));
		REQUIRE("foo.txt" == StringUtil::GetFileName("foo.txt"));
		REQUIRE("foo.txt" == StringUtil::GetFileName("tmp/foo.txt"));
		REQUIRE("foo.txt" == StringUtil::GetFileName("tmp\\foo.txt"));
		REQUIRE("foo.txt" == StringUtil::GetFileName("/tmp/foo.txt"));
		REQUIRE("foo.txt" == StringUtil::GetFileName("\\tmp\\foo.txt"));
		REQUIRE("foo.txt" == StringUtil::GetFileName("foo.txt/."));
		REQUIRE("foo.txt" == StringUtil::GetFileName("foo.txt/./"));
		REQUIRE("foo.txt" == StringUtil::GetFileName("foo.txt/.//"));
		REQUIRE("foo.txt" == StringUtil::GetFileName("foo.txt\\."));
		REQUIRE("foo.txt" == StringUtil::GetFileName("foo.txt\\.\\"));
		REQUIRE("foo.txt" == StringUtil::GetFileName("foo.txt\\.\\\\"));
		REQUIRE(".." == StringUtil::GetFileName(".."));
		REQUIRE("" == StringUtil::GetFileName("/"));
	}

	SECTION("File extension") {
		REQUIRE("cpp" == StringUtil::GetFileExtension("test.cpp"));
		REQUIRE("gz" == StringUtil::GetFileExtension("test.cpp.gz"));
		REQUIRE("" == StringUtil::GetFileExtension("test"));
		REQUIRE("" == StringUtil::GetFileExtension(".gitignore"));
	}

	SECTION("File stem (base name)") {
		REQUIRE("test" == StringUtil::GetFileStem("test.cpp"));
		REQUIRE("test.cpp" == StringUtil::GetFileStem("test.cpp.gz"));
		REQUIRE("test" == StringUtil::GetFileStem("test"));
		REQUIRE(".gitignore" == StringUtil::GetFileStem(".gitignore"));

		REQUIRE("test" == StringUtil::GetFileStem("test.cpp/"));
		REQUIRE("test" == StringUtil::GetFileStem("test.cpp/."));
		REQUIRE("test" == StringUtil::GetFileStem("test.cpp/./"));
		REQUIRE("test" == StringUtil::GetFileStem("test.cpp/.//"));
		REQUIRE("test" == StringUtil::GetFileStem("test.cpp\\"));
		REQUIRE("test" == StringUtil::GetFileStem("test.cpp\\."));
		REQUIRE("test" == StringUtil::GetFileStem("test.cpp\\.\\"));
		REQUIRE("test" == StringUtil::GetFileStem("test.cpp\\.\\\\"));
		REQUIRE(".." == StringUtil::GetFileStem(".."));
		REQUIRE("" == StringUtil::GetFileStem("/"));
		REQUIRE("test" == StringUtil::GetFileStem("tmp/test.txt"));
		REQUIRE("test" == StringUtil::GetFileStem("tmp\\test.txt"));
		REQUIRE("test" == StringUtil::GetFileStem("/tmp/test.txt"));
		REQUIRE("test" == StringUtil::GetFileStem("\\tmp\\test.txt"));
	}

	SECTION("File path") {
		REQUIRE("/usr/local/bin" == StringUtil::GetFilePath("/usr/local/bin/test.cpp"));
		REQUIRE("\\usr\\local\\bin" == StringUtil::GetFilePath("\\usr\\local\\bin\\test.cpp"));
		REQUIRE("tmp" == StringUtil::GetFilePath("tmp/test.txt"));
		REQUIRE("tmp" == StringUtil::GetFilePath("tmp\\test.txt"));
		REQUIRE("/tmp" == StringUtil::GetFilePath("/tmp/test.txt"));
		REQUIRE("\\tmp" == StringUtil::GetFilePath("\\tmp\\test.txt"));
		REQUIRE("/tmp" == StringUtil::GetFilePath("/tmp/test.txt/"));
		REQUIRE("\\tmp" == StringUtil::GetFilePath("\\tmp\\test.txt\\"));
		REQUIRE("/tmp" == StringUtil::GetFilePath("/tmp//test.txt"));
		REQUIRE("\\tmp" == StringUtil::GetFilePath("\\tmp\\\\test.txt"));
	}

	SECTION("Replace file name") {
		REQUIRE("/usr/local/bin/other.cpp" == StringUtil::ReplaceFileName("/usr/local/bin/test.cpp", "other.cpp"));
		REQUIRE("tmp\\other.txt" == StringUtil::ReplaceFileName("tmp\\test.txt", "other.txt"));
		REQUIRE("other.txt" == StringUtil::ReplaceFileName("test.txt", "other.txt"));
		REQUIRE("s3://bucket/other.csv" == StringUtil::ReplaceFileName("s3://bucket/test.csv", "other.csv"));
		REQUIRE("opfs://other.csv" == StringUtil::ReplaceFileName("opfs://test.csv", "other.csv"));
	}
}

TEST_CASE("Test JSON Parsing", "[string_util]") {
	auto json_map = StringUtil::ParseJSONMap(R"JSON_LITERAL(
	{
    "crs": {
        "$schema": "https://proj.org/schemas/v0.7/projjson.schema.json",
        "type": "GeographicCRS",
        "name": "WGS 84",
        "datum_ensemble": {
            "name": "World Geodetic System 1984 ensemble",
            "members": [
                {
                    "name": "World Geodetic System 1984 (Transit)",
                    "id": {
                        "authority": "EPSG",
                        "code": 1166
                    }
                },
                {
                    "name": "World Geodetic System 1984 (G730)",
                    "id": {
                        "authority": "EPSG",
                        "code": 1152
                    }
                },
                {
                    "name": "World Geodetic System 1984 (G873)",
                    "id": {
                        "authority": "EPSG",
                        "code": 1153
                    }
                },
                {
                    "name": "World Geodetic System 1984 (G1150)",
                    "id": {
                        "authority": "EPSG",
                        "code": 1154
                    }
                },
                {
                    "name": "World Geodetic System 1984 (G1674)",
                    "id": {
                        "authority": "EPSG",
                        "code": 1155
                    }
                },
                {
                    "name": "World Geodetic System 1984 (G1762)",
                    "id": {
                        "authority": "EPSG",
                        "code": 1156
                    }
                },
                {
                    "name": "World Geodetic System 1984 (G2139)",
                    "id": {
                        "authority": "EPSG",
                        "code": 1309
                    }
                },
                {
                    "name": "World Geodetic System 1984 (G2296)",
                    "id": {
                        "authority": "EPSG",
                        "code": 1383
                    }
                }
            ],
            "ellipsoid": {
                "name": "WGS 84",
                "semi_major_axis": 6378137,
                "inverse_flattening": 298.257223563
            },
            "accuracy": "2.0",
            "id": {
                "authority": "EPSG",
                "code": 6326
            }
        },
        "coordinate_system": {
            "subtype": "ellipsoidal",
            "axis": [
                {
                    "name": "Geodetic latitude",
                    "abbreviation": "Lat",
                    "direction": "north",
                    "unit": "degree"
                },
                {
                    "name": "Geodetic longitude",
                    "abbreviation": "Lon",
                    "direction": "east",
                    "unit": "degree"
                }
            ]
        },
        "scope": "Horizontal component of 3D system.",
        "area": "World.",
        "bbox": {
            "south_latitude": -90,
            "west_longitude": -180,
            "north_latitude": 90,
            "east_longitude": 180
        },
        "id": {
            "authority": "EPSG",
            "code": 4326
        }
    },
    "crs_type": "projjson"
}	)JSON_LITERAL");

	json_map = StringUtil::ParseJSONMap(R"JSON_LITERAL(
	{
		"int": 42,
		"signed_int": -42,
		"real": 1.5,
		"null_val": null,
		"arr": [1, 2, 3],
		"obj": {
			"str_val": "val"
		},
		"empty_arr": [],
		"bool_t": true,
		"bool_f": false
	}
	)JSON_LITERAL");
	REQUIRE(json_map["real"] == "1.500000");
}

TEST_CASE("Test ToString of doubles", "[string_util]") {
	REQUIRE(StringUtil::ToString(10.0) == "10.000000");
	REQUIRE(StringUtil::ToString(-2.25) == "-2.250000");
	REQUIRE(StringUtil::ToString(1.0 / 3.0) == "0.333333");
	REQUIRE(StringUtil::ToString(1e20) == "100000000000000000000.000000");
}

TEST_CASE("Test CIHash is independent of char signedness", "[string_util]") {
	// bytes >= 0x80 must be zero-extended before hashing so the result is identical
	// on platforms where char is signed (x86) and where it is unsigned (ARM)
	const char high_byte[] = {(char)0xC3, 0};
	REQUIRE(StringUtil::CIHash(high_byte, 1) == 2242087697ULL);

	const char cafe_utf8[] = "Caf\xC3\xA9";
	REQUIRE(StringUtil::CIHash(cafe_utf8, strlen(cafe_utf8)) == 2425794034ULL);

	// ASCII is unaffected
	REQUIRE(StringUtil::CIHash("hello") == StringUtil::CIHash("HeLLo"));
}

TEST_CASE("Test Damerau-Levenshtein distance", "[string_util]") {
	REQUIRE(StringUtil::DamerauLevenshteinDistance("", "") == 0);
	REQUIRE(StringUtil::DamerauLevenshteinDistance("", "abc") == 3);
	REQUIRE(StringUtil::DamerauLevenshteinDistance("from", "FROM") == 0);
	// a swap of adjacent characters is a single edit, unlike plain Levenshtein
	REQUIRE(StringUtil::DamerauLevenshteinDistance("form", "from") == 1);
	REQUIRE(StringUtil::LevenshteinDistance("form", "from") == 2);
	REQUIRE(StringUtil::DamerauLevenshteinDistance("gruop", "group") == 1);
	REQUIRE(StringUtil::DamerauLevenshteinDistance("whre", "where") == 1);
	REQUIRE(StringUtil::DamerauLevenshteinDistance("tabel", "table") == 1);
	REQUIRE(StringUtil::DamerauLevenshteinDistance("alias", "all") == 3);
	REQUIRE(StringUtil::DamerauLevenshteinDistance("int8_tbl", "int") == 5);
}

TEST_CASE("Test CILessThan handles embedded NUL bytes", "[string_util]") {
	const string secret("secret");
	const string secret_nul("secret\0other", 12);
	const string secret_nul2("secret\0zzz", 10);

	// a string with content after an embedded NUL is NOT equal to its prefix: exactly one of (a<b, b<a) holds
	REQUIRE(StringUtil::CILessThan(secret, secret_nul) != StringUtil::CILessThan(secret_nul, secret));
	// the shorter prefix sorts first
	REQUIRE(StringUtil::CILessThan(secret, secret_nul));
	REQUIRE(!StringUtil::CILessThan(secret_nul, secret));

	// equality (neither less than the other) only for genuinely equal strings
	REQUIRE((!StringUtil::CILessThan(secret, secret) && !StringUtil::CILessThan(secret, secret)));
	REQUIRE((!StringUtil::CILessThan(secret_nul, secret_nul) && !StringUtil::CILessThan(secret_nul, secret_nul)));

	// strings that differ only after the NUL byte are ordered by that byte (strict weak ordering, no collision)
	REQUIRE(StringUtil::CILessThan(secret_nul, secret_nul2)); // 'o' < 'z'
	REQUIRE(!StringUtil::CILessThan(secret_nul2, secret_nul));

	// case-insensitivity is preserved
	REQUIRE(!StringUtil::CILessThan("Hello", "hello"));
	REQUIRE(!StringUtil::CILessThan("hello", "Hello"));
	REQUIRE(StringUtil::CILessThan("abc", "abd"));
}

static string JSONWriterExceptionMap(ExceptionType type, const string &message,
                                     const unordered_map<string, string> &map) {
	JSONWriter writer;
	auto obj = writer.CreateObject();
	obj.AddString("exception_type", Exception::ExceptionTypeToString(type));
	obj.AddString("exception_message", message);
	for (auto &entry : map) {
		obj.AddString(entry.first, entry.second);
	}
	writer.SetRoot(obj);
	return writer.ToString(JSONWriteFlags::ALLOW_INVALID_UNICODE);
}

//! The map a JSON document reads from an object whose values are all strings
static bool JSONDocumentStringMap(const string &json, unordered_map<string, string> &result) {
	JSONParseError error;
	auto doc = JSONDocument::TryParse(json.c_str(), json.size(), error, JSONReadFlags::ALLOW_INVALID_UNICODE);
	if (!doc || !doc->GetRoot().IsObject()) {
		return false;
	}
	doc->GetRoot().IterateObject([&](const string &key, JSONValue value) {
		REQUIRE(value.IsString());
		result[key] = value.GetString();
	});
	return true;
}

TEST_CASE("Exception JSON is written as the JSON writer writes it", "[string_util]") {
	string every_byte;
	for (idx_t i = 0; i < 256; i++) {
		every_byte += static_cast<char>(i);
	}
	duckdb::vector<string> messages = {"",
	                                   "failed to allocate data of size 16.0 MiB (15.9 MiB/19.0 MiB used)",
	                                   "say \"hi\" \\ back\\slash / slash",
	                                   "line\nbreak\ttab\rreturn\bbackspace\fform feed\x01\x0b\x1f\x7f",
	                                   string("embedded\0nul", 12),
	                                   "h\xc3\xa9llo \xe2\x82\xac \xf0\x9d\x84\x9e",
	                                   "invalid \xff\xfe lone \x80 cut \xe2\x82",
	                                   every_byte};
	duckdb::vector<unordered_map<string, string>> extra_infos = {
	    {},
	    {{"error_subtype", "MISSING_ENTRY"}},
	    {{"position", "7"}, {"name", "t\"bl\n"}, {"candidates", "a,b"}, {"key \\ \x02", every_byte}}};
	duckdb::vector<ExceptionType> types = {ExceptionType::OUT_OF_MEMORY, ExceptionType::INVALID_INPUT,
	                                       ExceptionType::INTERNAL};
	for (auto type : types) {
		for (auto &message : messages) {
			for (auto &extra_info : extra_infos) {
				auto json = StringUtil::ExceptionToJSONMap(type, message, extra_info);
				REQUIRE(json == JSONWriterExceptionMap(type, message, extra_info));

				auto parsed = StringUtil::ParseJSONMap(json);
				unordered_map<string, string> expected;
				REQUIRE(JSONDocumentStringMap(json, expected));
				REQUIRE(parsed == expected);
				unordered_map<string, string> flat;
				REQUIRE(StringUtil::TryParseFlatJSONMap(json, flat));
				REQUIRE(flat == expected);
				REQUIRE(StringUtil::TryParseFlatJSONMap(StringUtil::ToJSONMap(extra_info), flat));
				REQUIRE(flat == extra_info);
				REQUIRE(parsed.size() == extra_info.size() + 2);
				REQUIRE(parsed["exception_type"] == Exception::ExceptionTypeToString(type));
				REQUIRE(parsed["exception_message"] == message);
				for (auto &entry : extra_info) {
					REQUIRE(parsed[entry.first] == entry.second);
				}
			}
		}
	}
}

TEST_CASE("A flat JSON map is read as the JSON document reads it", "[string_util]") {
	// what the exception and map writers can write: read without a JSON document
	duckdb::vector<string> flat = {"{}",
	                               R"({"":""})",
	                               R"({"a":"b"})",
	                               R"({"a":"b","a":"c"})",
	                               R"({"a\"b":"c\\d"})",
	                               R"({"k":"\/ \b\f\n\r\t"})",
	                               R"({"k":"A\u007f\u001F\u007F\u0000"})",
	                               "{\"k\":\"h\xc3\xa9llo \xf0\x9d\x84\x9e\"}",
	                               "{\"k\":\"invalid \xff\xfe \x80\"}"};
	// valid JSON the writers do not write: read as a JSON document
	duckdb::vector<string> document = {R"({"k":"\u00e9 \u20AC \uD834\uDD1E"})",
	                                   R"({"k":"\u0080"})",
	                                   R"({ "a" : "b" })",
	                                   "{\"a\":\"b\"}\n",
	                                   "{\"a\":\"x\ty\"}",
	                                   string("{\"a\":\"x\0y\"}", 11)};
	for (auto &json : flat) {
		INFO(json);
		unordered_map<string, string> expected;
		REQUIRE(JSONDocumentStringMap(json, expected));
		unordered_map<string, string> result;
		REQUIRE(StringUtil::TryParseFlatJSONMap(json, result));
		REQUIRE(result == expected);
		REQUIRE(StringUtil::ParseJSONMap(json) == expected);
	}
	for (auto &json : document) {
		INFO(json);
		unordered_map<string, string> expected;
		REQUIRE(JSONDocumentStringMap(json, expected));
		unordered_map<string, string> result;
		REQUIRE(!StringUtil::TryParseFlatJSONMap(json, result));
		REQUIRE(result.empty());
		REQUIRE(StringUtil::ParseJSONMap(json) == expected);
	}
	duckdb::vector<string> unreadable = {"{",
	                                     R"({"a")",
	                                     R"({"a":)",
	                                     R"({"a":"b")",
	                                     R"({"a":"b)",
	                                     R"({"a":"b",})",
	                                     R"({"a":"b"}x)",
	                                     R"({"a":"b"}{"c":"d"})",
	                                     R"({"a"})",
	                                     R"({"a":"\x"})",
	                                     R"({"a":"\u00"})",
	                                     R"({"a":"\u00G0"})",
	                                     R"(["a","b"])",
	                                     R"("a")"};
	for (auto &json : unreadable) {
		INFO(json);
		unordered_map<string, string> expected;
		REQUIRE(!JSONDocumentStringMap(json, expected));
		unordered_map<string, string> result;
		REQUIRE(!StringUtil::TryParseFlatJSONMap(json, result));
		REQUIRE(result.empty());
		REQUIRE_THROWS_AS(StringUtil::ParseJSONMap(json), SerializationException);
		REQUIRE(StringUtil::ParseJSONMap(json, true).empty());
	}
	auto decoded = StringUtil::ParseJSONMap(R"({"k":"A\u007f\u001F\u0000\/"})");
	REQUIRE(decoded["k"] == string("A\x7f\x1f\0/", 5));
}

TEST_CASE("A JSON map that TryParseFlatJSONMap refuses late keeps the key order of a JSON document read",
          "[string_util]") {
	string json = "{";
	for (idx_t i = 0; i < 200; i++) {
		json += (i ? ",\"k" : "\"k") + to_string(i) + "\":\"v\"";
	}
	// the trailing newline makes TryParseFlatJSONMap refuse the object after it has read every key
	json += "}\n";
	unordered_map<string, string> expected;
	REQUIRE(JSONDocumentStringMap(json, expected));
	auto result = StringUtil::ParseJSONMap(json);
	duckdb::vector<string> result_keys;
	for (auto &entry : result) {
		result_keys.push_back(entry.first);
	}
	duckdb::vector<string> expected_keys;
	for (auto &entry : expected) {
		expected_keys.push_back(entry.first);
	}
	REQUIRE(result_keys == expected_keys);

	// a map that held entries before gets the key order of a fresh read too
	unordered_map<string, string> reused;
	REQUIRE(!StringUtil::TryParseFlatJSONMap(json, reused));
	string small = R"({"a":"1","b":"2","c":"3"})";
	REQUIRE(StringUtil::TryParseFlatJSONMap(json.substr(0, json.size() - 1), reused));
	REQUIRE(StringUtil::TryParseFlatJSONMap(small, reused));
	auto fresh = StringUtil::ParseJSONMap(small);
	duckdb::vector<string> reused_keys;
	for (auto &entry : reused) {
		reused_keys.push_back(entry.first);
	}
	duckdb::vector<string> fresh_keys;
	for (auto &entry : fresh) {
		fresh_keys.push_back(entry.first);
	}
	REQUIRE(reused_keys == fresh_keys);
}
