#include "live_data.h"

#include "common.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace wperf {
namespace {

constexpr wchar_t kWindowVersion[] = L"1";

std::wstring EncodeString(const std::wstring& value) {
    return value.empty() ? L"-" : ToUtf16(HexText(value));
}

bool DecodeString(const std::wstring& value, std::wstring& result) {
    if (value == L"-") {
        result.clear();
        return true;
    }
    return UnhexText(ToUtf8(value), result);
}

std::wstring EncodeU64(const std::vector<uint64_t>& values) {
    if (values.empty()) {
        return L"-";
    }
    std::wostringstream output;
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output << L',';
        }
        output << std::hex << values[index];
    }
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

std::wstring PrefixVersion(const wchar_t* kind) {
    return std::wstring(kind) + L" " + kWindowVersion;
}

void SetError(std::wstring* error, const wchar_t* message) {
    if (error != nullptr) {
        *error = message;
    }
}

}  // namespace

std::vector<std::wstring> EncodeLiveWindow(const TraceData& trace,
                                           uint64_t interval_ms) {
    std::vector<std::wstring> lines;
    std::wostringstream begin;
    begin << L"TOP2_BEGIN " << kWindowVersion << L' ' << interval_ms << L' '
          << trace.quality.samples_total << L' ' << trace.quality.events_total << L' '
          << trace.quality.unknown_events << L' ' << trace.quality.metadata_dropped
          << L' ' << trace.quality.pmc_samples << L' ' << trace.quality.lost_events
          << L' ' << trace.quality.lost_buffers << L' ' << trace.quality.ring_dropped
          << L' ' << trace.quality.stack_exact_total << L' '
          << trace.quality.stack_approximate_total << L' '
          << trace.quality.samples_without_stack << L' '
          << trace.quality.stack_ambiguous << L' ' << trace.quality.decode_errors
          << L' ' << (trace.quality.incomplete ? 1 : 0) << L' '
          << trace.profile_interval_ns;
    lines.push_back(begin.str());

    for (const ImageRecord& image : trace.images) {
        std::wostringstream line;
        line << L"TOP2_IMAGE " << image.pid << L' ' << image.base << L' '
             << image.size << L' ' << image.timestamp << L' ' << image.checksum
             << L' ' << image.load_ns << L' ' << image.unload_ns << L' '
             << EncodeString(image.path) << L' ' << EncodeString(image.resolved_path);
        lines.push_back(line.str());
    }
    for (const ProcessRecord& process : trace.processes) {
        std::wostringstream line;
        line << L"TOP2_PROCESS " << process.pid << L' ' << process.parent_pid << L' '
             << process.start_ns << L' ' << process.end_ns << L' '
             << EncodeString(process.image_name) << L' '
             << EncodeString(process.command_line);
        lines.push_back(line.str());
    }
    for (const ThreadRecord& thread : trace.threads) {
        std::wostringstream line;
        line << L"TOP2_THREAD " << thread.pid << L' ' << thread.tid << L' '
             << thread.start_ns << L' ' << thread.end_ns;
        lines.push_back(line.str());
    }
    for (const SampleRecord& sample : trace.samples) {
        std::wostringstream line;
        line << L"TOP2_SAMPLE " << sample.timestamp_ns << L' ' << sample.cpu << L' '
             << sample.pid << L' ' << sample.tid << L' '
             << sample.instruction_pointer << L' ' << sample.period << L' '
             << sample.profile_source << L' '
             << (sample.kind == SampleKind::Pmc ? 1 : 0) << L' '
             << static_cast<uint32_t>(sample.stack_quality) << L' '
             << (sample.stack_missing ? 1 : 0) << L' '
             << EncodeU64(sample.stack.View()) << L' '
             << EncodeU64(sample.pmc_counters);
        lines.push_back(line.str());
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
    if (!(input >> version) || version != kWindowVersion ||
        !ReadUint64(input, interval) || !ReadUint64(input, samples) ||
        !ReadUint64(input, events) || !ReadUint64(input, unknown) ||
        !ReadUint64(input, metadata_dropped) || !ReadUint64(input, pmc_samples) ||
        !ReadUint64(input, lost) || !ReadUint64(input, lost_buffers) ||
        !ReadUint64(input, ring_dropped) || !ReadUint64(input, exact) ||
        !ReadUint64(input, approximate) || !ReadUint64(input, missing) ||
        !ReadUint64(input, ambiguous) || !ReadUint64(input, decode_errors) ||
        !ReadUint64(input, incomplete) || !ReadUint64(input, profile_interval)) {
        SetError(error, L"unsupported or malformed live window header");
        return false;
    }
    trace_ = TraceData{};
    interval_ms_ = interval;
    expected_samples_ = samples;
    trace_.quality.samples_total = samples;
    trace_.quality.events_total = events;
    trace_.quality.unknown_events = unknown;
    trace_.quality.metadata_dropped = metadata_dropped;
    trace_.quality.pmc_samples = pmc_samples;
    trace_.quality.lost_events = lost;
    trace_.quality.lost_buffers = lost_buffers;
    trace_.quality.ring_dropped = ring_dropped;
    trace_.quality.stack_exact_total = exact;
    trace_.quality.stack_approximate_total = approximate;
    trace_.quality.samples_without_stack = missing;
    trace_.quality.stack_ambiguous = ambiguous;
    trace_.quality.decode_errors = decode_errors;
    trace_.quality.incomplete = incomplete != 0;
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
        std::wstring stack, pmc;
        if (!ReadUint64(input, sample.timestamp_ns) || !ReadUint32(input, sample.cpu) ||
            !ReadUint32(input, sample.pid) || !ReadUint32(input, sample.tid) ||
            !ReadUint64(input, sample.instruction_pointer) || !ReadUint64(input, sample.period) ||
            !ReadUint32(input, sample.profile_source) || !ReadUint32(input, kind_value) ||
            !ReadUint32(input, quality) || !ReadUint32(input, missing) ||
            !(input >> stack) || !(input >> pmc)) {
            SetError(error, L"invalid TOP2_SAMPLE message");
            return false;
        }
        std::vector<uint64_t> frames;
        if (!DecodeU64(stack, frames) || !DecodeU64(pmc, sample.pmc_counters) ||
            kind_value > 1 || quality > static_cast<uint32_t>(StackQuality::Approximate)) {
            SetError(error, L"invalid TOP2_SAMPLE payload");
            return false;
        }
        sample.kind = kind_value == 1 ? SampleKind::Pmc : SampleKind::Cpu;
        sample.stack_quality = static_cast<StackQuality>(quality);
        sample.stack_missing = missing != 0;
        sample.stack = frames;
        trace_.samples.push_back(std::move(sample));
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