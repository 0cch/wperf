#include "json_writer.h"

#include "common.h"
#include <stdexcept>

namespace wperf {

JsonStreamWriter::JsonStreamWriter(std::wostream& output) : output_(output) {}

void JsonStreamWriter::BeforeValue() {
    if (levels_.empty()) return;
    Level& level = levels_.back();
    if (level.object) {
        if (!level.pending_value) {
            throw std::logic_error("JSON object value without key");
        }
        level.pending_value = false;
        return;
    }
    if (!level.first) output_ << L',';
    level.first = false;
}

void JsonStreamWriter::WriteDumped(const nlohmann::ordered_json& value) {
    output_ << ToUtf16(value.dump());
}

void JsonStreamWriter::BeginObject() {
    BeforeValue();
    output_ << L'{';
    levels_.push_back(Level{true, true, false});
}

void JsonStreamWriter::EndObject() {
    if (levels_.empty() || !levels_.back().object || levels_.back().pending_value) {
        throw std::logic_error("invalid JSON object close");
    }
    levels_.pop_back();
    output_ << L'}';
}

void JsonStreamWriter::BeginArray() {
    BeforeValue();
    output_ << L'[';
    levels_.push_back(Level{false, true, false});
}

void JsonStreamWriter::EndArray() {
    if (levels_.empty() || levels_.back().object) {
        throw std::logic_error("invalid JSON array close");
    }
    levels_.pop_back();
    output_ << L']';
}

void JsonStreamWriter::Key(std::wstring_view key) {
    if (levels_.empty() || !levels_.back().object || levels_.back().pending_value) {
        throw std::logic_error("JSON key outside object");
    }
    Level& level = levels_.back();
    if (!level.first) output_ << L',';
    level.first = false;
    level.pending_value = true;
    WriteDumped(nlohmann::ordered_json(ToUtf8(key)));
    output_ << L':';
}

void JsonStreamWriter::String(std::wstring_view value) {
    BeforeValue();
    WriteDumped(nlohmann::ordered_json(ToUtf8(value)));
}

void JsonStreamWriter::Unsigned(uint64_t value) {
    BeforeValue();
    WriteDumped(nlohmann::ordered_json(value));
}

void JsonStreamWriter::Signed(int64_t value) {
    BeforeValue();
    WriteDumped(nlohmann::ordered_json(value));
}

void JsonStreamWriter::Double(double value) {
    BeforeValue();
    WriteDumped(nlohmann::ordered_json(value));
}

void JsonStreamWriter::Bool(bool value) {
    BeforeValue();
    WriteDumped(nlohmann::ordered_json(value));
}

void JsonStreamWriter::Null() {
    BeforeValue();
    output_ << L"null";
}

void JsonStreamWriter::Item(const nlohmann::ordered_json& value) {
    if (levels_.empty()) {
        throw std::logic_error("JSON item outside container");
    }
    BeforeValue();
    WriteDumped(value);
}

}  // namespace wperf
