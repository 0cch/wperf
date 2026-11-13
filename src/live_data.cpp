#include "live_data.h"

#include "common.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace wperf {
namespace {

constexpr wchar_t kWindowVersion[] = L"2";
constexpr size_t kMaxEncodedPayloadChars = 16 * 1024;
constexpr size_t kMaxEncodedStringBytes = kMaxEncodedPayloadChars / 2;
constexpr size_t kMaxWindowEncodedChars = 8 * 1024 * 1024;

std::wstring Utf8Prefix(std::string_view value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), required);
    return result;
}

std::wstring EncodeString(const std::wstring& value, bool* truncated = nullptr) {
    if (truncated != nullptr) *truncated = false;
    if (value.empty()) return L"-";
    const std::string bytes = ToUtf8(value);
    if (bytes.size() <= kMaxEncodedStringBytes) {
        return ToUtf16(HexText(value));
    }
    size_t length = std::min(bytes.size(), kMaxEncodedStringBytes);
    std::wstring prefix;
    while (length != 0) {
        prefix = Utf8Prefix(std::string_view(bytes.data(), length));
        if (!prefix.empty()) break;
        --length;
    }
    if (truncated != nullptr) *truncated = true;
    return prefix.empty() ? L"-" : ToUtf16(HexText(prefix));
}

bool DecodeString(const std::wstring& value, std::wstring& result) {
    if (value == L"-") {
        result.clear();
        return true;
    }
    return UnhexText(ToUtf8(value), result);
}

std::wstring EncodeU64(const std::vector<uint64_t>& values,
                       bool* truncated = nullptr) {
    if (truncated != nullptr) *truncated = false;
    if (values.empty()) return L"-";
    std::wostringstream output;
    for (size_t index = 0; index < values.size(); ++index) {
        std::wostringstream item;
        if (index != 0) item << L',';
        item << std::hex << values[index];
        if (output.tellp() >= 0 &&
            static_cast<size_t>(output.tellp()) + item.str().size() >
                kMaxEncodedPayloadChars) {
            if (truncated != nullptr) *truncated = true;
            break;
        }
        output << item.str();
    }
    if (output.str().empty()) return L"-";
    return ToUtf16(HexText(output.str()));
}

bool DecodeU64(const std::wstring& value, std::vector<uint64_t>& result) {
    result.clear();
    if (value == L"-") {
        return true;
    }
    std::wstring text;
    if (!UnhexText(ToUtf8(value), text)) {
        return false;
    }
    std::wistringstream input(text);
    std::wstring item;
    while (std::getline(input, item, L',')) {
        if (item.empty()) {
            return false;
        }
        wchar_t* end = nullptr;
        const uint64_t parsed = wcstoull(item.c_str(), &end, 16);
        if (end == nullptr || *end != L'\0') {
            return false;
        }
        result.push_back(parsed);
    }
    return true;
}

bool ReadUint64(std::wistringstream& input, uint64_t& value) {
    std::wstring text;
    if (!(input >> text)) {
        return false;
    }
    wchar_t* end = nullptr;
    const uint64_t parsed = wcstoull(text.c_str(), &end, 10);
    if (end == nullptr || *end != L'\0') {
        return false;
    }
    value = parsed;
    return true;
}

bool ReadUint32(std::wistringstream& input, uint32_t& value) {
    uint64_t parsed = 0;
    if (!ReadUint64(input, parsed) || parsed > UINT32_MAX) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

void SetError(std::wstring* error, const wchar_t* message) {
    if (error != nullptr) {
        *error = message;
    }
}

}  // namespace

std::vector<std::wstring> EncodeLiveWindow(const TraceData& trace,
                                           uint64_t interval_ms) {
    std::vector<std::wstring> body;
    uint64_t truncated_count = 0;
    const auto encode = [&](const std::wstring& value) {
        bool truncated = false;
        std::wstring encoded = EncodeString(value, &truncated);
        if (truncated) ++truncated_count;
        return encoded;
    };
    size_t window_chars = 0;
    uint64_t dropped_records = 0;
    uint64_t dropped_samples = 0;
    uint64_t encoded_samples = 0;
    const auto append = [&](std::wstring line) {
        if (line.size() + 1 > kMaxWindowEncodedChars - window_chars) {
            return false;
        }
        window_chars += line.size() + 1;
        body.push_back(std::move(line));
        return true;
    };
    for (const ImageRecord& image : trace.images) {
        std::wostringstream line;
        line << L"TOP2_IMAGE " << image.pid << L' ' << image.base << L' '
             << image.size << L' ' << image.timestamp << L' ' << image.checksum
             << L' ' << image.load_ns << L' ' << image.unload_ns << L' '
             << encode(image.path) << L' ' << encode(image.resolved_path);
        if (!append(line.str())) ++dropped_records;
    }
    for (const ProcessRecord& process : trace.processes) {
        std::wostringstream line;
        line << L"TOP2_PROCESS " << process.pid << L' ' << process.parent_pid << L' '
             << process.start_ns << L' ' << process.end_ns << L' '
             << encode(process.image_name) << L' '
             << encode(process.command_line);
        if (!append(line.str())) ++dropped_records;
    }
    for (const ThreadRecord& thread : trace.threads) {
        std::wostringstream line;
        line << L"TOP2_THREAD " << thread.pid << L' ' << thread.tid << L' '
             << thread.start_ns << L' ' << thread.end_ns;
        if (!append(line.str())) ++dropped_records;
    }
    for (size_t sample_index = 0; sample_index < trace.samples.size(); ++sample_index) {
        const SampleRecord& sample = trace.samples[sample_index];
        bool stack_truncated = false;
        bool pmc_truncated = false;
        const std::wstring stack = EncodeU64(sample.stack.View(), &stack_truncated);
        const std::wstring pmc = EncodeU64(sample.pmc_counters, &pmc_truncated);
        if (stack_truncated) ++truncated_count;
        if (pmc_truncated) ++truncated_count;
        std::wostringstream line;
        line << L"TOP2_SAMPLE " << sample.timestamp_ns << L' ' << sample.cpu << L' '
             << sample.pid << L' ' << sample.tid << L' '
             << sample.instruction_pointer << L' ' << sample.period << L' '
             << sample.profile_source << L' '
             << (sample.kind == SampleKind::Pmc ? 1 : 0) << L' '
             << static_cast<uint32_t>(sample.stack_quality) << L' '
             << (sample.stack_missing ? 1 : 0) << L' '
             << stack << L' ' << pmc << L' '
             << (stack_truncated ? 1 : 0) << L' '
             << (pmc_truncated ? 1 : 0);
        if (!append(line.str())) {
            dropped_samples = trace.samples.size() - sample_index;
            break;
        }
        ++encoded_samples;
    }

    std::vector<std::wstring> lines;
    lines.reserve(body.size() + 3);
    std::wostringstream begin;
    begin << L"TOP2_BEGIN " << kWindowVersion << L' ' << interval_ms << L' '
          << encoded_samples << L' ' << trace.quality.events_total << L' '
          << trace.quality.unknown_events << L' ' << trace.quality.metadata_dropped
          << L' ' << trace.quality.pmc_samples << L' ' << trace.quality.lost_events
          << L' ' << trace.quality.lost_buffers << L' ' << trace.quality.ring_dropped
          << L' ' << trace.quality.stack_exact_total << L' '
          << trace.quality.stack_approximate_total << L' '
          << trace.quality.samples_without_stack << L' '
          << trace.quality.stack_ambiguous << L' ' << trace.quality.decode_errors
          << L' ' << (trace.quality.incomplete ? 1 : 0) << L' '
          << trace.profile_interval_ns << L' ' << trace.quality.samples_total
          << L' ' << dropped_samples << L' ' << dropped_records;
    lines.push_back(begin.str());
    lines.insert(lines.end(), body.begin(), body.end());
    if (truncated_count != 0) {
        lines.push_back(L"TOP2_TRUNCATED " + std::to_wstring(truncated_count));
    }
    lines.push_back(L"TOP2_END");
    return lines;
}

bool LiveWindowDecoder::Begin(const std::wstring& line, std::wstring* error) {
    if (line.rfind(L"TOP2_BEGIN ", 0) != 0) {
        SetError(error, L"invalid live window header");
        return false;
    }
    std::wistringstream input(line.substr(11));
    std::wstring version;
    uint64_t interval = 0, samples = 0, events = 0, unknown = 0, metadata_dropped = 0;
    uint64_t pmc_samples = 0, lost = 0, lost_buffers = 0, ring_dropped = 0;
    uint64_t exact = 0, approximate = 0, missing = 0, ambiguous = 0, decode_errors = 0;
    uint64_t incomplete = 0, profile_interval = 0;
    uint64_t original_samples = 0, samples_dropped = 0, records_dropped = 0;
    if (!(input >> version) || version != kWindowVersion ||
        !ReadUint64(input, interval) || !ReadUint64(input, samples) ||
        !ReadUint64(input, events) || !ReadUint64(input, unknown) ||
        !ReadUint64(input, metadata_dropped) || !ReadUint64(input, pmc_samples) ||
        !ReadUint64(input, lost) || !ReadUint64(input, lost_buffers) ||
        !ReadUint64(input, ring_dropped) || !ReadUint64(input, exact) ||
        !ReadUint64(input, approximate) || !ReadUint64(input, missing) ||
        !ReadUint64(input, ambiguous) || !ReadUint64(input, decode_errors) ||
        !ReadUint64(input, incomplete) || !ReadUint64(input, profile_interval) ||
        !ReadUint64(input, original_samples) || !ReadUint64(input, samples_dropped) ||
        !ReadUint64(input, records_dropped)) {
        SetError(error, L"unsupported or malformed live window header");
        return false;
    }
    trace_ = TraceData{};
    interval_ms_ = interval;
    expected_samples_ = samples;
    trace_.quality.samples_total = original_samples;
    trace_.quality.samples_dropped = samples_dropped;
    trace_.quality.events_total = events;
    trace_.quality.unknown_events = unknown;
    trace_.quality.metadata_dropped = metadata_dropped + records_dropped;
    trace_.quality.pmc_samples = pmc_samples;
    trace_.quality.lost_events = lost;
    trace_.quality.lost_buffers = lost_buffers;
    trace_.quality.ring_dropped = ring_dropped;
    trace_.quality.stack_exact_total = exact;
    trace_.quality.stack_approximate_total = approximate;
    trace_.quality.samples_without_stack = missing;
    trace_.quality.stack_ambiguous = ambiguous;
    trace_.quality.decode_errors = decode_errors;
    trace_.quality.incomplete =
        incomplete != 0 || samples_dropped != 0 || records_dropped != 0;
    trace_.profile_interval_ns = profile_interval;
    active_ = true;
    return true;
}

bool LiveWindowDecoder::Consume(const std::wstring& line, std::wstring* error) {
    if (!active_) {
        SetError(error, L"live window data arrived before its header");
        return false;
    }
    if (line == L"TOP2_END") {
        return true;
    }
    std::wistringstream input(line);
    std::wstring kind;
    input >> kind;
    if (kind == L"TOP2_IMAGE") {
        ImageRecord image;
        std::wstring path, resolved;
        if (!ReadUint32(input, image.pid) || !ReadUint64(input, image.base) ||
            !ReadUint64(input, image.size) || !ReadUint32(input, image.timestamp) ||
            !ReadUint32(input, image.checksum) || !ReadUint64(input, image.load_ns) ||
            !ReadUint64(input, image.unload_ns) || !(input >> path) ||
            !(input >> resolved) || !DecodeString(path, image.path) ||
            !DecodeString(resolved, image.resolved_path)) {
            SetError(error, L"invalid TOP2_IMAGE message");
            return false;
        }
        trace_.images.push_back(std::move(image));
    } else if (kind == L"TOP2_PROCESS") {
        ProcessRecord process;
        std::wstring image_name, command_line;
        if (!ReadUint32(input, process.pid) || !ReadUint32(input, process.parent_pid) ||
            !ReadUint64(input, process.start_ns) || !ReadUint64(input, process.end_ns) ||
            !(input >> image_name) || !(input >> command_line) ||
            !DecodeString(image_name, process.image_name) ||
            !DecodeString(command_line, process.command_line)) {
            SetError(error, L"invalid TOP2_PROCESS message");
            return false;
        }
        trace_.processes.push_back(std::move(process));
    } else if (kind == L"TOP2_THREAD") {
        ThreadRecord thread;
        if (!ReadUint32(input, thread.pid) || !ReadUint32(input, thread.tid) ||
            !ReadUint64(input, thread.start_ns) || !ReadUint64(input, thread.end_ns)) {
            SetError(error, L"invalid TOP2_THREAD message");
            return false;
        }
        trace_.threads.push_back(thread);
    } else if (kind == L"TOP2_SAMPLE") {
        SampleRecord sample;
        uint32_t kind_value = 0, quality = 0, missing = 0;
        uint32_t stack_truncated = 0, pmc_truncated = 0;
        std::wstring stack, pmc;
        if (!ReadUint64(input, sample.timestamp_ns) || !ReadUint32(input, sample.cpu) ||
            !ReadUint32(input, sample.pid) || !ReadUint32(input, sample.tid) ||
            !ReadUint64(input, sample.instruction_pointer) || !ReadUint64(input, sample.period) ||
            !ReadUint32(input, sample.profile_source) || !ReadUint32(input, kind_value) ||
            !ReadUint32(input, quality) || !ReadUint32(input, missing) ||
            !(input >> stack) || !(input >> pmc) ||
            !ReadUint32(input, stack_truncated) || !ReadUint32(input, pmc_truncated)) {
            SetError(error, L"invalid TOP2_SAMPLE message");
            return false;
        }
        std::vector<uint64_t> frames;
        if (!DecodeU64(stack, frames) || !DecodeU64(pmc, sample.pmc_counters) ||
            kind_value > 1 || quality > static_cast<uint32_t>(StackQuality::Approximate) ||
            stack_truncated > 1 || pmc_truncated > 1) {
            SetError(error, L"invalid TOP2_SAMPLE payload");
            return false;
        }
        sample.kind = kind_value == 1 ? SampleKind::Pmc : SampleKind::Cpu;
        sample.stack_quality = static_cast<StackQuality>(quality);
        sample.stack_missing = missing != 0;
        if (stack_truncated != 0) {
            if (frames.empty()) {
                sample.stack_quality = StackQuality::Missing;
                sample.stack_missing = true;
            } else if (sample.stack_quality == StackQuality::Exact) {
                sample.stack_quality = StackQuality::Approximate;
            }
        }
        sample.stack = frames;
        trace_.samples.push_back(std::move(sample));
    } else if (kind == L"TOP2_TRUNCATED") {
        uint64_t count = 0;
        if (!ReadUint64(input, count)) {
            SetError(error, L"invalid TOP2_TRUNCATED message");
            return false;
        }
        trace_.quality.metadata_dropped += count;
        if (count != 0) trace_.quality.incomplete = true;
    } else {
        SetError(error, L"unknown live window message");
        return false;
    }
    return true;
}

bool LiveWindowDecoder::Finish(TraceData& trace, uint64_t& interval_ms,
                               std::wstring* error) {
    if (!active_) {
        SetError(error, L"live window was not started");
        return false;
    }
    if (trace_.samples.size() != expected_samples_) {
        trace_.quality.incomplete = true;
        ++trace_.quality.decode_errors;
    }
    trace = std::move(trace_);
    interval_ms = interval_ms_;
    active_ = false;
    return true;
}

}  // namespace wperf
