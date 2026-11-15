#include "common.h"
#include "json_writer.h"

#include <cstdint>
#include <iostream>
#include <sstream>

int main() {
    std::wostringstream out;
    wperf::JsonStreamWriter writer(out);
    writer.BeginObject();
    writer.Key(L"text");
    writer.String(L"quote=\" slash=\\ newline=\n unicode=中文");
    writer.Key(L"u64");
    writer.Unsigned(UINT64_MAX);
    writer.Key(L"null_value");
    writer.Null();
    writer.Key(L"items");
    writer.BeginArray();
    writer.Item(nlohmann::ordered_json{{"name", "first"}, {"value", 1}});
    writer.Item(nlohmann::ordered_json::array({1, 2, 3}));
    writer.EndArray();
    writer.EndObject();

    const auto parsed = nlohmann::json::parse(wperf::ToUtf8(out.str()));
    if (parsed.at("text").get<std::string>() != "quote=\" slash=\\ newline=\n unicode=中文") {
        std::cerr << "string escaping mismatch\n";
        return 1;
    }
    if (!parsed.at("u64").is_number_unsigned() ||
        parsed.at("u64").get<uint64_t>() != UINT64_MAX) {
        std::cerr << "uint64 round trip mismatch\n";
        return 1;
    }
    if (!parsed.at("null_value").is_null() ||
        parsed.at("items").size() != 2) {
        std::cerr << "container state mismatch\n";
        return 1;
    }
    std::cout << "json writer: PASS\n";
    return 0;
}
