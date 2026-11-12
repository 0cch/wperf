#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wperf {

struct ProcessRecord {
    uint32_t pid = 0;
    uint32_t parent_pid = 0;
    std::wstring image_name;
    std::wstring command_line;
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
};

struct ThreadRecord {
    uint32_t pid = 0;
    uint32_t tid = 0;
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
};

struct ImageRecord {
    uint32_t pid = 0;
    uint64_t base = 0;
    uint64_t size = 0;
    uint32_t timestamp = 0;
    uint32_t checksum = 0;
    uint64_t load_ns = 0;
    uint64_t unload_ns = 0;
    std::wstring path;
};

struct SampleRecord {
    uint64_t timestamp_ns = 0;
    uint32_t cpu = 0;
    uint32_t pid = 0;
    uint32_t tid = 0;
    uint64_t instruction_pointer = 0;
    std::vector<uint64_t> stack;
    bool stack_missing = false;
};

enum class TraceEventKind {
    ContextSwitch,
    ReadyThread,
    SyscallEnter,
    SyscallExit,
    PageFault,
    DiskIo,
    FileIo,
    Unknown,
};

struct TraceEvent {
    uint64_t timestamp_ns = 0;
    uint32_t cpu = 0;
    uint32_t pid = 0;
    uint32_t tid = 0;
    TraceEventKind kind = TraceEventKind::Unknown;
    uint64_t value1 = 0;
    uint64_t value2 = 0;
    uint32_t event_id = 0;
};

struct TraceQuality {
    uint64_t events_total = 0;
    uint64_t samples_total = 0;
    uint64_t stack_events_total = 0;
    uint64_t stack_ref_events_total = 0;
    uint64_t stack_def_events_total = 0;
    uint64_t stack_exact_total = 0;
    uint64_t stack_approximate_total = 0;
    uint64_t stack_key_mismatch = 0;
    uint64_t samples_without_stack = 0;
    uint64_t stack_ambiguous = 0;
    uint64_t unknown_events = 0;
    uint64_t lost_events = 0;
    bool incomplete = false;
};

struct TraceData {
    std::vector<ProcessRecord> processes;
    std::vector<ThreadRecord> threads;
    std::vector<ImageRecord> images;
    std::vector<SampleRecord> samples;
    std::vector<TraceEvent> events;
    TraceQuality quality;
    uint32_t pointer_size = sizeof(void*);
    uint64_t qpc_frequency = 10000000;
    uint64_t profile_interval_ns = 1000000;
    std::wstring source_path;
};

const wchar_t* TraceEventKindName(TraceEventKind kind);
const ProcessRecord* FindProcess(const TraceData& trace, uint32_t pid,
                                 uint64_t timestamp_ns);
const ThreadRecord* FindThread(const TraceData& trace, uint32_t tid,
                                 uint64_t timestamp_ns);
const ImageRecord* FindImage(const TraceData& trace, uint32_t pid, uint64_t address,
                             uint64_t timestamp_ns);
std::wstring ProcessDisplayName(const TraceData& trace, uint32_t pid);

}  // namespace wperf
