#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <ostream>
#include <string_view>
#include <vector>

namespace wperf {

class JsonStreamWriter {
public:
    explicit JsonStreamWriter(std::wostream& output);

    JsonStreamWriter(const JsonStreamWriter&) = delete;
    JsonStreamWriter& operator=(const JsonStreamWriter&) = delete;

    void BeginObject();
    void EndObject();
    void BeginArray();
    void EndArray();
    void Key(std::wstring_view key);
    void String(std::wstring_view value);
    void Unsigned(uint64_t value);
    void Signed(int64_t value);
    void Double(double value);
    void Bool(bool value);
    void Null();
    void Item(const nlohmann::ordered_json& value);

private:
    struct Level {
        bool object = false;
        bool first = true;
        bool pending_value = false;
    };

    void BeforeValue();
    void WriteDumped(const nlohmann::ordered_json& value);

    std::wostream& output_;
    std::vector<Level> levels_;
};

}  // namespace wperf
