#include "state.h"

#include "common.h"

#include <fstream>
#include <sstream>

namespace wperf {
namespace {

std::string EscapeJson(std::wstring_view value) {
    const std::string utf8 = ToUtf8(value);
    std::string result;
    for (unsigned char ch : utf8) {
        switch (ch) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    char buffer[8]{};
                    sprintf_s(buffer, "\\u%04x", ch);
                    result += buffer;
                } else {
                    result.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return result;
}

std::wstring ExtractJsonString(const std::wstring& text, const std::wstring& key) {
    const std::wstring marker = L"\"" + key + L"\":\"";
    size_t position = text.find(marker);
    if (position == std::wstring::npos) {
        return {};
    }
    position += marker.size();
    std::wstring result;
    bool escaped = false;
    for (; position < text.size(); ++position) {
        const wchar_t ch = text[position];
        if (escaped) {
            switch (ch) {
                case L'n':
                    result.push_back(L'\n');
                    break;
                case L'r':
                    result.push_back(L'\r');
                    break;
                case L't':
                    result.push_back(L'\t');
                    break;
                default:
                    result.push_back(ch);
                    break;
            }
            escaped = false;
            continue;
        }
        if (ch == L'\\') {
            escaped = true;
            continue;
        }
        if (ch == L'"') {
            break;
        }
        result.push_back(ch);
    }
    return result;
}

uint32_t ExtractJsonNumber(const std::wstring& text, const std::wstring& key) {
    const std::wstring marker = L"\"" + key + L"\":";
    size_t position = text.find(marker);
    if (position == std::wstring::npos) {
        return 0;
    }
    position += marker.size();
    while (position < text.size() && (text[position] == L' ' || text[position] == L'\t')) {
        ++position;
    }
    uint64_t value = 0;
    while (position < text.size() && text[position] >= L'0' && text[position] <= L'9') {
        value = value * 10 + static_cast<uint64_t>(text[position] - L'0');
        ++position;
    }
    return static_cast<uint32_t>(value);
}

}  // namespace

StateStore::StateStore(std::filesystem::path path) : path_(std::move(path)) {}

const std::filesystem::path& StateStore::Path() const {
    return path_;
}

std::filesystem::path StateStore::PathFor(const std::wstring& correlation_id) {
    return GetSessionsDirectory() / (correlation_id + L".json");
}

bool StateStore::Write(const RunState& state, std::wstring* error) const {
    if (!EnsureDirectory(path_.parent_path(), error)) {
        return false;
    }

    RunState copy = state;
    if (copy.correlation_id.empty()) {
        copy.correlation_id = path_.stem().wstring();
    }
    if (copy.updated_utc.empty()) {
        copy.updated_utc = UtcTimestamp();
    }

    const std::filesystem::path temporary = path_.wstring() + L".tmp." + NewId();
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        if (error != nullptr) {
            *error = L"cannot open state file: " + temporary.wstring();
        }
        return false;
    }

    stream << "{\n"
           << "  \"correlation_id\":\"" << EscapeJson(copy.correlation_id) << "\",\n"
           << "  \"phase\":\"" << EscapeJson(copy.phase) << "\",\n"
           << "  \"session_name\":\"" << EscapeJson(copy.session_name) << "\",\n"
           << "  \"trace_path\":\"" << EscapeJson(copy.trace_path) << "\",\n"
           << "  \"controller_role\":\"" << EscapeJson(copy.controller_role) << "\",\n"
           << "  \"controller_pid\":" << copy.controller_pid << ",\n"
           << "  \"worker_pid\":" << copy.worker_pid << ",\n"
           << "  \"target_pid\":" << copy.target_pid << ",\n"
           << "  \"guard_pid\":" << copy.guard_pid << ",\n"
           << "  \"error_code\":" << copy.error_code << ",\n"
           << "  \"updated_utc\":\"" << EscapeJson(copy.updated_utc) << "\"\n"
           << "}\n";
    stream.flush();
    stream.close();

    if (!MoveFileExW(temporary.c_str(), path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        if (error != nullptr) {
            *error = L"MoveFileEx(state) failed: " + FormatWindowsError(code);
        }
        return false;
    }
    return true;
}

std::vector<RunState> StateStore::ReadAll(
    const std::filesystem::path& sessions_directory) {
    std::vector<RunState> result;
    std::error_code ec;
    if (!std::filesystem::exists(sessions_directory, ec)) {
        return result;
    }

    for (const auto& entry : std::filesystem::directory_iterator(sessions_directory, ec)) {
        if (ec || !entry.is_regular_file() || entry.path().extension() != L".json") {
            continue;
        }
        std::ifstream stream(entry.path(), std::ios::binary);
        if (!stream.is_open()) {
            continue;
        }
        std::ostringstream bytes;
        bytes << stream.rdbuf();
        const std::wstring text = ToUtf16(bytes.str());

        RunState state;
        state.correlation_id = ExtractJsonString(text, L"correlation_id");
        state.phase = ExtractJsonString(text, L"phase");
        state.session_name = ExtractJsonString(text, L"session_name");
        state.trace_path = ExtractJsonString(text, L"trace_path");
        state.controller_role = ExtractJsonString(text, L"controller_role");
        state.updated_utc = ExtractJsonString(text, L"updated_utc");
        state.controller_pid = ExtractJsonNumber(text, L"controller_pid");
        state.worker_pid = ExtractJsonNumber(text, L"worker_pid");
        state.target_pid = ExtractJsonNumber(text, L"target_pid");
        state.guard_pid = ExtractJsonNumber(text, L"guard_pid");
        state.error_code = ExtractJsonNumber(text, L"error_code");
        if (!state.correlation_id.empty()) {
            result.push_back(std::move(state));
        }
    }
    return result;
}

}  // namespace wperf
