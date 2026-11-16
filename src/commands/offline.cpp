#include "commands/offline.h"

#include "commands/help.h"
#include "commands/live_control.h"
#include "commands/util.h"
#include "common.h"
#include "archive.h"
#include "analysis.h"
#include "output.h"
#include "json_writer.h"
#include "cpu_report.h"
#include "elevation.h"
#include "etl_reader.h"
#include "symbolizer.h"
#include "trace_events.h"
#include "off_cpu_report.h"
#include "trace_model.h"
#include "diagnostic_report.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

namespace wperf {
using namespace commands;

bool WriteReportFile(const std::filesystem::path&, const std::wstring&, std::wstring*);

int RunScript(const ScriptRequest& request) {
    std::filesystem::path input = request.input;
    std::optional<uint32_t> pid = request.pid;
    std::wstring fields = request.fields;
    std::wstring symbol_path = request.symbol_path;
    std::filesystem::path archive_directory = request.archive;
    uint64_t limit = request.limit;
    bool csv = request.csv;
    if (input.empty()) {
        PrintError(L"script requires -i FILE");
        return 2;
    }

    std::vector<std::wstring> field_list;
    size_t field_start = 0;
    while (field_start <= fields.size()) {
        const size_t comma = fields.find(L',', field_start);
        const std::wstring field = fields.substr(
            field_start, comma == std::wstring::npos ? std::wstring::npos
                                                     : comma - field_start);
        if (field.empty()) {
            PrintError(L"--fields contains an empty field");
            return 2;
        }
        if (field != L"timestamp" && field != L"cpu" && field != L"pid" &&
            field != L"tid" && field != L"ip" && field != L"comm" &&
            field != L"dso" && field != L"symbol" && field != L"stack" &&
            field != L"source" && field != L"period" && field != L"stack_quality") {
            PrintError(L"unsupported script field: " + field);
            return 2;
        }
        field_list.push_back(field);
        if (comma == std::wstring::npos) {
            break;
        }
        field_start = comma + 1;
    }

    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error, false)) {
        PrintError(error);
        return 6;
    }
    if(!archive_directory.empty()) {
        if(!LoadArchiveModules(archive_directory,trace,&error)) { PrintError(error); return 6; }
        symbol_path+=L";"+ArchiveSymbolPath(archive_directory,trace);
    }
    Symbolizer symbolizer;
    if (!symbolizer.Initialize(symbol_path, &error)) { PrintError(error); return 6; }
    symbolizer.PrepareTrace(trace);
    for (const auto& image : trace.images) {
        symbolizer.AddModule(image);
    }

    std::wostringstream file_output;
    std::wostream& output = request.output.empty() ? std::wcout : file_output;

    for (size_t field_index = 0; field_index < field_list.size(); ++field_index) {
        if (field_index != 0) {
            output << (csv ? L"," : L" ");
        }
        output << field_list[field_index];
    }
    output << L"\n";

    ProcessNameIndex process_names(trace);
    uint64_t emitted = 0;
    for (const auto& sample : trace.samples) {
        if (pid.has_value() && sample.pid != *pid) {
            continue;
        }
        if (limit != 0 && emitted >= limit) {
            break;
        }
        const ResolvedSymbol leaf = symbolizer.Resolve(
            trace, sample.pid, sample.instruction_pointer, sample.timestamp_ns);
        const std::wstring comm = process_names.Name(sample.pid, sample.timestamp_ns);
        for (size_t field_index = 0; field_index < field_list.size(); ++field_index) {
            if (field_index != 0) {
                output << (csv ? L"," : L" ");
            }
            const std::wstring& field = field_list[field_index];
            if (field == L"timestamp") {
                output << sample.timestamp_ns;
            } else if (field == L"cpu") {
                output << sample.cpu;
            } else if (field == L"pid") {
                output << sample.pid;
            } else if (field == L"tid") {
                output << sample.tid;
            } else if (field == L"ip") {
                output << std::hex << sample.instruction_pointer << std::dec;
            } else if (field == L"comm") {
                output << (csv ? CsvEscape(comm) : comm);
            } else if (field == L"dso") {
                output << (csv ? CsvEscape(leaf.module) : leaf.module);
            } else if (field == L"symbol") {
                output << (csv ? CsvEscape(leaf.symbol) : leaf.symbol);
            } else if (field == L"source") {
                output << sample.profile_source;
            } else if (field == L"period") {
                output << sample.period;
            } else if (field == L"stack_quality") {
                output << (sample.stack_quality == StackQuality::Exact ? L"exact" :
                               sample.stack_quality == StackQuality::Approximate ? L"approximate" : L"missing");
            } else if (field == L"stack") {
                std::wostringstream stack;
                for (size_t frame = 0; frame < sample.stack.size(); ++frame) {
                    if (frame != 0) {
                        stack << L";";
                    }
                    stack << std::hex << sample.stack[frame] << std::dec;
                }
                output << (csv ? CsvEscape(stack.str()) : stack.str());
            }
        }
        output << L"\n";
        ++emitted;
    }
    if (!request.output.empty() && !WriteReportFile(request.output, file_output.str(), &error)) {
        PrintError(error); return 9;
    }
    return 0;
}
std::atomic_bool g_live_trace_interrupted{false};

BOOL WINAPI LiveTraceControlHandler(DWORD control_type) {
    if (control_type == CTRL_C_EVENT || control_type == CTRL_BREAK_EVENT ||
        control_type == CTRL_CLOSE_EVENT || control_type == CTRL_LOGOFF_EVENT ||
        control_type == CTRL_SHUTDOWN_EVENT) {
        g_live_trace_interrupted.store(true);
        return TRUE;
    }
    return FALSE;
}

int RunLiveTrace(const std::wstring& filter, const std::wstring& pid_text,
                 const std::wstring& max_text, const std::wstring& duration_text,
                 bool json, bool elevate) {
    bool elevated = false;
    std::wstring error;
    IsProcessElevated(elevated, &error);
    if (!elevated && !elevate) {
        PrintError(L"live trace requires administrator rights; rerun with --elevate");
        return 3;
    }

    LiveWorkerOptions options;
    options.mode = L"trace";
    options.session_name = L"wperf-trace-" + NewId();
    options.interval_ms = 250;
    options.event_filter = filter;
    options.pid_text = pid_text;
    options.max_text = max_text;
    options.duration_text = duration_text;
    options.json = json;

    LiveWorkerClient worker;
    if (!worker.Start(options, !elevated && elevate, &error)) {
        PrintError(error);
        return 4;
    }

    g_live_trace_interrupted.store(false);
    SetConsoleCtrlHandler(LiveTraceControlHandler, TRUE);
    int exit_code = 0;
    bool finished = false;
    bool stop_sent = false;
    std::unique_ptr<JsonStreamWriter> live_json;
    while (!finished) {
        if (g_live_trace_interrupted.load() && !stop_sent) {
            worker.SendStop(nullptr);
            stop_sent = true;
        }
        if (!worker.HasData()) {
            DWORD worker_exit = 0;
            std::wstring ignored;
            if (worker.WaitForExit(0, worker_exit, &ignored)) {
                if (!finished) {
                    PrintError(L"live worker exited unexpectedly with code " +
                               std::to_wstring(worker_exit));
                    exit_code = static_cast<int>(worker_exit);
                }
                break;
            }
            Sleep(20);
            continue;
        }

        std::wstring line;
        if (!worker.ReceiveLine(line, 5000, &error)) {
            PrintError(error);
            exit_code = 5;
            break;
        }
        if (line == L"JSON_BEGIN" && json) {
            live_json = std::make_unique<JsonStreamWriter>(std::wcout);
            live_json->BeginArray();
        } else if (line == L"JSON_END" && live_json) {
            live_json->EndArray();
            std::wcout << L"\n";
            live_json.reset();
        } else if (line.rfind(L"OUT ", 0) == 0) {
            const std::wstring payload = line.substr(4);
            if (live_json) {
                const auto item = nlohmann::ordered_json::parse(ToUtf8(payload), nullptr, false);
                if (item.is_discarded()) {
                    PrintError(L"live worker emitted invalid JSON");
                    exit_code = 5;
                    break;
                }
                live_json->Item(item);
            } else {
                std::wcout << payload << L"\n";
            }
            std::wcout.flush();
        } else if (line.rfind(L"WARN ", 0) == 0) {
            std::wcerr << L"wperf: warning: " << line.substr(5) << L"\n";
        } else if (line.rfind(L"ERROR ", 0) == 0) {
            PrintError(line.substr(6));
        } else if (line.rfind(L"FINISHED ", 0) == 0) {
            exit_code = _wtoi(line.c_str() + 9);
            finished = true;
        }
    }

    SetConsoleCtrlHandler(LiveTraceControlHandler, FALSE);
    worker.Close();
    return exit_code;
}
bool MatchesEventFilter(const TraceEvent& event, const std::wstring& filter) {
    if (filter.empty() || filter == L"all") {
        return true;
    }
    const std::wstring kind = TraceEventKindName(event.kind);
    if (filter == L"sched") {
        return event.kind == TraceEventKind::ContextSwitch ||
               event.kind == TraceEventKind::ReadyThread;
    }
    if (filter == L"syscall") {
        return event.kind == TraceEventKind::SyscallEnter ||
               event.kind == TraceEventKind::SyscallExit;
    }
    if (filter == L"pagefault" || filter == L"vm") {
        return event.kind == TraceEventKind::PageFault;
    }
    if (filter == L"io") {
        return event.kind == TraceEventKind::DiskIo ||
               event.kind == TraceEventKind::FileIo;
    }
    if (filter == L"lbr") {
        return event.kind == TraceEventKind::LastBranchRecord;
    }
    if (filter == L"context" || filter == L"context-register" ||
        filter == L"context-registers") {
        return event.kind == TraceEventKind::ContextRegister;
    }
    return kind == filter;
}

std::wstring EventDetail(const TraceData& trace, const TraceEvent& event) {
    std::wostringstream detail;
    if (event.event_id != 0) {
        detail << L"id=" << event.event_id << L" ";
    }
    switch (event.kind) {
        case TraceEventKind::ContextSwitch:
            detail << L"old_tid=" << event.related_tid
                   << L" wait=" << TraceWaitReasonName(event.wait_reason)
                   << L" state=" << TraceThreadStateName(event.previous_state);
            break;
        case TraceEventKind::ReadyThread:
            detail << L"awakened_tid=" << event.related_tid
                   << L" flags=0x" << std::hex << event.flags << std::dec;
            break;
        case TraceEventKind::SyscallEnter:
            detail << L"address=0x" << std::hex << event.value1 << std::dec;
            break;
        case TraceEventKind::SyscallExit:
            if (event.has_status) {
                detail << L"ntstatus=0x" << std::hex
                       << static_cast<uint32_t>(event.status) << std::dec;
            }
            break;
        case TraceEventKind::FileIo:
            detail << L"irp=0x" << std::hex << event.correlation_id << std::dec
                   << L" offset=" << event.offset << L" size=" << event.size
                   << L" completion=" << (event.is_completion ? L"true" : L"false");
            if (event.has_status) {
                detail << L" ntstatus=0x" << std::hex
                       << static_cast<uint32_t>(event.status) << std::dec;
            }
            break;
        case TraceEventKind::DiskIo:
            detail << L"irp=0x" << std::hex << event.correlation_id << std::dec
                   << L" offset=" << event.offset << L" size=" << event.size
                   << L" completion=" << (event.is_completion ? L"true" : L"false");
            break;
        case TraceEventKind::ContextRegister:
            if (event.context_register_index < trace.context_registers.size()) {
                const auto& registers = trace.context_registers[event.context_register_index];
                detail << L"rip=0x" << std::hex << registers.rip
                       << L" rsp=0x" << registers.rsp
                       << L" rax=0x" << registers.rax
                       << L" rcx=0x" << registers.rcx
                       << L" rdx=0x" << registers.rdx
                       << L" rbx=0x" << registers.rbx
                       << L" rbp=0x" << registers.rbp
                       << L" rsi=0x" << registers.rsi
                       << L" rdi=0x" << registers.rdi
                       << L" r8=0x" << registers.r8
                       << L" r9=0x" << registers.r9
                       << L" r10=0x" << registers.r10
                       << L" r11=0x" << registers.r11
                       << L" r12=0x" << registers.r12
                       << L" r13=0x" << registers.r13
                       << L" r14=0x" << registers.r14
                       << L" r15=0x" << registers.r15
                       << std::dec;
            }
            break;        case TraceEventKind::LastBranchRecord:
            detail << L"from=0x" << std::hex << event.value1
                   << L" to=0x" << event.value2 << std::dec
                   << L" filters=0x" << std::hex << event.flags << std::dec;
            break;
        default:
            break;
    }
    if (event.has_duration) {
        detail << L" duration_us=" << event.duration_ns / 1000;
    }
    return detail.str();
}

int RunTrace(const TraceRequest& request) {
    std::filesystem::path input = request.input;
    std::wstring filter = request.event;
    const bool json = request.json;
    const bool metadata = request.metadata;
    const bool live = request.live;
    const bool elevate = request.elevate;
    std::optional<uint32_t> pid = request.pid;
    std::wstring pid_text = pid ? std::to_wstring(*pid) : L"";
    const uint64_t max_events = request.max_events;
    const std::wstring max_text = std::to_wstring(max_events);
    const std::wstring duration_text = request.duration_ms ? std::to_wstring(*request.duration_ms) : L"0";
    const bool duration_explicit = request.duration_ms.has_value();
    if (filter != L"all" && filter != L"sched" && filter != L"syscall" &&
        filter != L"pagefault" && filter != L"io" && filter != L"lbr" &&
        filter != L"context" && filter != L"context-register" &&
        filter != L"context-registers") {
        PrintError(L"unsupported trace event: " + filter);
        return ExitStatus(ExitCode::InvalidArguments);
    }
    if (metadata && !json) {
        PrintError(L"--metadata requires --json");
        return ExitStatus(ExitCode::InvalidArguments);
    }
    if (live && metadata) {
        PrintError(L"--metadata is only supported for offline trace replay");
        return ExitStatus(ExitCode::InvalidArguments);
    }
    if (live) {
        if (!input.empty()) {
            PrintError(L"trace --live cannot be combined with -i FILE");
            return ExitStatus(ExitCode::InvalidArguments);
        }
        if (filter == L"context" || filter == L"context-register" ||
            filter == L"context-registers") {
            PrintError(L"context register tracing is currently supported for offline ETL traces");
            return ExitStatus(ExitCode::InvalidArguments);
        }
        return RunLiveTrace(filter, pid_text, max_text, duration_text, json, elevate);
    }
    if (duration_explicit) {
        PrintError(L"--duration is only valid with trace --live");
        return ExitStatus(ExitCode::InvalidArguments);
    }
    if (elevate) {
        PrintError(L"--elevate is only valid with trace --live");
        return ExitStatus(ExitCode::InvalidArguments);
    }
    if (input.empty()) {
        PrintError(L"trace requires -i FILE for offline mode or --live");
        return ExitStatus(ExitCode::InvalidArguments);
    }
    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error, false)) {
        PrintError(error);
        return 6;
    }

    uint64_t first_timestamp = UINT64_MAX;
    size_t emitted = 0;
    bool truncated = false;
    std::unique_ptr<JsonStreamWriter> json_writer;
    if (json) {
        json_writer = std::make_unique<JsonStreamWriter>(std::wcout);
        if (metadata) {
            json_writer->BeginObject();
            json_writer->Key(L"events");
        }
        json_writer->BeginArray();
    } else {
        std::wcout << L"timestamp_ns cpu pid tid event value1 value2 "
                      L"related_tid related_pid detail comm\n";
    }
    std::vector<TraceEvent> lbr_events;
    if (filter == L"lbr" || filter == L"all") {
        for (const auto& sample : trace.lbr_samples) {
            for (size_t index = 0; index < sample.branches.size(); ++index) {
                TraceEvent event;
                event.timestamp_ns = sample.timestamp_ns;
                event.cpu = sample.cpu;
                event.pid = sample.pid;
                event.tid = sample.tid;
                event.kind = TraceEventKind::LastBranchRecord;
                event.value1 = sample.branches[index].from_address;
                event.value2 = sample.branches[index].to_address;
                event.flags = sample.filters;
                event.event_id = sample.event_id;
                event.lbr_group_id = sample.group_id;
                event.lbr_branch_index = static_cast<uint32_t>(index);
                lbr_events.push_back(std::move(event));
            }
        }
    }
    std::vector<const TraceEvent*> ordered_events;
    ordered_events.reserve(trace.events.size() + lbr_events.size());
    for (const auto& event : trace.events) ordered_events.push_back(&event);
    for (const auto& event : lbr_events) ordered_events.push_back(&event);
    std::stable_sort(ordered_events.begin(), ordered_events.end(),
                     [](const TraceEvent* left, const TraceEvent* right) {
                         return left->timestamp_ns < right->timestamp_ns;
                     });
    for (const TraceEvent* event_ptr : ordered_events) {
        const TraceEvent& event = *event_ptr;
        uint32_t display_pid = event.pid;
        uint32_t display_tid = event.tid;
        if (event.kind == TraceEventKind::ReadyThread) {
            display_tid = event.related_tid;
            if (display_pid == 0 || display_pid == UINT32_MAX) {
                display_pid = event.related_pid;
            }
        }
        if (display_pid == 0 || display_pid == UINT32_MAX) {
            if (const auto* thread =
                    FindThread(trace, display_tid, event.timestamp_ns)) {
                display_pid = thread->pid;
            }
        }
        if (pid.has_value() && display_pid != *pid) {
            continue;
        }
        if (!MatchesEventFilter(event, filter)) {
            continue;
        }
        if (max_events != 0 && emitted >= max_events) {
            truncated = true;
            break;
        }
        if (first_timestamp == UINT64_MAX) {
            first_timestamp = event.timestamp_ns;
        }
        const uint64_t relative =
            event.timestamp_ns >= first_timestamp ? event.timestamp_ns - first_timestamp
                                                  : 0;
        const std::wstring comm = ProcessDisplayName(trace, display_pid, event.timestamp_ns);
        const std::wstring detail = EventDetail(trace, event);
        if (json_writer) {
            nlohmann::ordered_json item;
            item["ts"] = relative;
            item["cpu"] = event.cpu;
            item["pid"] = display_pid;
            item["tid"] = display_tid;
            item["event"] = ToUtf8(TraceEventKindName(event.kind));
            item["value1"] = event.value1;
            item["value2"] = event.value2;
            item["related_tid"] = event.related_tid;
            item["related_pid"] = event.related_pid;
            item["status"] = event.status;
            item["has_status"] = event.has_status;
            item["duration_ns"] = event.duration_ns;
            item["has_duration"] = event.has_duration;
            item["size"] = event.size;
            item["offset"] = event.offset;
            item["correlation_id"] = event.correlation_id;
            item["wait_reason"] = event.wait_reason;
            item["previous_state"] = event.previous_state;
            item["flags"] = event.flags;
            item["file_path"] = ToUtf8(event.file_path);
            item["event_id"] = event.event_id;
            item["operation_id"] = event.operation_id;
            item["file_key"] = event.file_key;
            item["file_object"] = event.file_object;
            item["lbr_group_id"] = event.lbr_group_id;
            item["lbr_branch_index"] = event.lbr_branch_index;
            item["stack"] = event.stack.View();
            item["stack_quality"] = event.stack_quality == StackQuality::Exact ? "exact" :
                (event.stack_quality == StackQuality::Approximate ? "approximate" : "missing");
            item["stack_missing"] = event.stack_missing;
            item["stack_ambiguous"] = event.stack_ambiguous;
            item["pmc_counters"] = event.pmc_counters;
            if (event.kind == TraceEventKind::ContextRegister &&
                event.context_register_index < trace.context_registers.size()) {
                const auto& registers = trace.context_registers[event.context_register_index];
                item["context_registers"] = nlohmann::ordered_json{
                    {"rip", registers.rip}, {"rsp", registers.rsp},
                    {"rax", registers.rax}, {"rcx", registers.rcx},
                    {"rdx", registers.rdx}, {"rbx", registers.rbx},
                    {"rbp", registers.rbp}, {"rsi", registers.rsi},
                    {"rdi", registers.rdi}, {"r8", registers.r8},
                    {"r9", registers.r9}, {"r10", registers.r10},
                    {"r11", registers.r11}, {"r12", registers.r12},
                    {"r13", registers.r13}, {"r14", registers.r14},
                    {"r15", registers.r15}};
            }
            item["detail"] = ToUtf8(detail);
            item["comm"] = ToUtf8(comm);
            json_writer->Item(item);
        } else {
            std::wcout << relative << L" " << event.cpu << L" " << display_pid << L" "
                       << display_tid << L" " << TraceEventKindName(event.kind) << L" "
                       << event.value1 << L" " << event.value2 << L" "
                       << event.related_tid << L" " << event.related_pid << L" "
                       << detail << L" " << comm << L"\n";
        }
        ++emitted;
    }
    if (json_writer) {
        json_writer->EndArray();
        if (metadata) {
            json_writer->Key(L"truncated");
            json_writer->Bool(truncated);
            json_writer->Key(L"quality");
            json_writer->Item(QualityJson(trace));
            json_writer->EndObject();
        }
        std::wcout << L"\n";
    }
    if (truncated) {
        std::wcerr << L"wperf: trace output truncated at " << max_events << L" events\n";
    }
    return ExitStatus(ExitCode::Success);
}

int RunSched(const SchedRequest& request) {
    std::filesystem::path input = request.input;
    bool json = request.json;
    bool csv = request.csv;
    if (input.empty()) {
        PrintError(L"sched requires -i FILE");
        return ExitStatus(ExitCode::InvalidArguments);
    }
    if (json && csv) {
        PrintError(L"--json and --csv are mutually exclusive");
        return ExitStatus(ExitCode::InvalidArguments);
    }
    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error, false)) {
        PrintError(error);
        return ExitStatus(ExitCode::ReplayOrQualityFailure);
    }

    const auto timing = AnalyzeScheduler(trace);
    struct Counts {
        uint64_t running = 0;
        uint64_t ready = 0;
        uint64_t blocked = 0;
        uint64_t wakeups = 0;
        uint64_t wakeup_total_ns = 0;
        uint64_t wakeup_max_ns = 0;
    };
    std::map<uint32_t, Counts> counts;
    std::map<uint32_t, std::map<uint32_t, uint64_t>> wait_reasons;
    for (const auto& event : trace.events) {
        if (event.kind == TraceEventKind::ContextSwitch) {
            uint32_t pid = event.pid;
            if (pid == 0 || pid == UINT32_MAX) pid = event.related_pid;
            ++counts[pid].running;
            if (event.related_pid != 0 && event.related_pid != UINT32_MAX) {
                if (event.previous_state == 5) {
                    ++counts[event.related_pid].blocked;
                    ++wait_reasons[event.related_pid][event.wait_reason];
                } else if (event.previous_state == 1) {
                    ++counts[event.related_pid].ready;
                }
            }
            if (event.has_duration) {
                Counts& target = counts[pid];
                ++target.wakeups;
                target.wakeup_total_ns += event.duration_ns;
                target.wakeup_max_ns = std::max(target.wakeup_max_ns, event.duration_ns);
            }
        } else if (event.kind == TraceEventKind::ReadyThread) {
            const uint32_t pid = event.related_pid;
            if (pid != 0 && pid != UINT32_MAX) ++counts[pid].ready;
        }
    }

    struct Row {
        uint32_t pid = 0;
        std::wstring comm;
        uint64_t running_ms = 0;
        uint64_t ready_ms = 0;
        uint64_t blocked_ms = 0;
        uint64_t wakeups = 0;
        uint64_t wakeup_avg_us = 0;
        uint64_t wakeup_max_us = 0;
        uint64_t p50_us = 0;
        uint64_t p95_us = 0;
        uint64_t p99_us = 0;
        std::wstring top_wait_reason;
    };
    std::vector<Row> rows;
    rows.reserve(counts.size());
    for (const auto& [pid, value] : counts) {
        uint32_t top_reason = 0;
        uint64_t top_reason_count = 0;
        const auto reasons = wait_reasons.find(pid);
        if (reasons != wait_reasons.end()) {
            for (const auto& [reason, count] : reasons->second) {
                if (count > top_reason_count) {
                    top_reason = reason;
                    top_reason_count = count;
                }
            }
        }
        const auto measured = timing.find(pid);
        const SchedulerRow empty{};
        const auto& durations = measured == timing.end() ? empty : measured->second;
        Row row;
        row.pid = pid;
        row.comm = ProcessDisplayName(trace, pid);
        row.running_ms = durations.running_ns / 1000000;
        row.ready_ms = durations.ready_ns / 1000000;
        row.blocked_ms = durations.blocked_ns / 1000000;
        row.wakeups = value.wakeups;
        row.wakeup_avg_us = (value.wakeups == 0 ? 0 : value.wakeup_total_ns / value.wakeups) / 1000;
        row.wakeup_max_us = value.wakeup_max_ns / 1000;
        row.p50_us = Percentile(durations.wakeup_ns, 0.5) / 1000;
        row.p95_us = Percentile(durations.wakeup_ns, 0.95) / 1000;
        row.p99_us = Percentile(durations.wakeup_ns, 0.99) / 1000;
        row.top_wait_reason = top_reason_count == 0 ? L"-" : TraceWaitReasonName(top_reason);
        rows.push_back(std::move(row));
    }

    if (json) {
        JsonStreamWriter writer(std::wcout);
        writer.BeginObject();
        writer.Key(L"quality");
        writer.Item(QualityJson(trace));
        writer.Key(L"rows");
        writer.BeginArray();
        for (const auto& row : rows) {
            nlohmann::ordered_json item;
            item["pid"] = row.pid;
            item["comm"] = ToUtf8(row.comm);
            item["running_ms"] = row.running_ms;
            item["ready_ms"] = row.ready_ms;
            item["blocked_ms"] = row.blocked_ms;
            item["wakeups"] = row.wakeups;
            item["wakeup_avg_us"] = row.wakeup_avg_us;
            item["wakeup_max_us"] = row.wakeup_max_us;
            item["p50_us"] = row.p50_us;
            item["p95_us"] = row.p95_us;
            item["p99_us"] = row.p99_us;
            item["top_wait_reason"] = ToUtf8(row.top_wait_reason);
            writer.Item(item);
        }
        writer.EndArray();
        writer.EndObject();
        std::wcout << L"\n";
        return ExitStatus(ExitCode::Success);
    }
    if (csv) {
        std::wcout << L"pid,comm,running_ms,ready_ms,blocked_ms,wakeups,wakeup_avg_us,wakeup_max_us,p50_us,p95_us,p99_us,top_wait_reason\n";
        for (const auto& row : rows) {
            std::wcout << row.pid << L',' << EscapeCsv(row.comm) << L','
                       << row.running_ms << L',' << row.ready_ms << L','
                       << row.blocked_ms << L',' << row.wakeups << L','
                       << row.wakeup_avg_us << L',' << row.wakeup_max_us << L','
                       << row.p50_us << L',' << row.p95_us << L',' << row.p99_us << L','
                       << EscapeCsv(row.top_wait_reason) << L"\n";
        }
        return ExitStatus(ExitCode::Success);
    }

    std::wcout << std::left << std::setw(8) << L"PID" << std::setw(26)
               << L"Command" << std::right << std::setw(9) << L"Running"
               << std::setw(9) << L"Ready" << std::setw(9) << L"Blocked"
               << std::setw(9) << L"Wakeups" << std::setw(13)
               << L"WakeupAvgUs" << std::setw(13) << L"WakeupMaxUs"
               << L"  TopWaitReason  RunningMs ReadyMs BlockedMs P50Us P95Us P99Us\n";
    for (const auto& row : rows) {
        const auto& counts_row = counts.at(row.pid);
        std::wcout << std::left << std::setw(8) << row.pid << std::setw(26)
                   << ClampText(row.comm, 24) << std::right
                   << std::setw(9) << counts_row.running << std::setw(9)
                   << counts_row.ready << std::setw(9) << counts_row.blocked
                   << std::setw(9) << row.wakeups << std::setw(13) << row.wakeup_avg_us
                   << std::setw(13) << row.wakeup_max_us << L"  " << row.top_wait_reason
                   << L" " << row.running_ms << L" " << row.ready_ms << L" "
                   << row.blocked_ms << L" " << row.p50_us << L" " << row.p95_us
                   << L" " << row.p99_us << L"\n";
    }
    return ExitStatus(ExitCode::Success);
}
bool MatchesEventSelection(const TraceData& trace, const TraceEvent& event,
                           const CpuReportOptions& options, const ProcessNameIndex& names,
                           bool outgoing = false) {
    SampleRecord point;
    point.pid = outgoing ? (event.related_tid == 0 ? 0 :
                           event.related_pid == 0 ? UINT32_MAX : event.related_pid) : event.pid;
    point.tid = outgoing ? event.related_tid : event.tid;
    point.cpu = event.cpu;
    point.timestamp_ns = event.timestamp_ns;
    auto selection = options;
    selection.profile_source.reset();
    if (!MatchesSampleSelection(trace, point, selection)) return false;
    if (options.comm_filter.empty()) return true;
    auto lower = [](std::wstring text) {
        std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) { return wchar_t(std::towlower(ch)); });
        return text;
    };
    return lower(names.Name(point.pid, point.timestamp_ns)).find(lower(options.comm_filter)) != std::wstring::npos;
}

uint64_t WriteIoSummary(const TraceData& trace, const CpuReportOptions& options,
                       JsonStreamWriter* shared_writer = nullptr) {
    const bool json = options.json;
    ProcessNameIndex names(trace);
    uint64_t file_io = 0;
    uint64_t disk_io = 0;
    uint64_t file_duration_total_ns = 0;
    uint64_t disk_duration_total_ns = 0;
    uint64_t file_duration_max_ns = 0;
    uint64_t disk_duration_max_ns = 0;
    uint64_t file_duration_count = 0;
    uint64_t disk_duration_count = 0;
    std::map<uint32_t, uint64_t> file_operations;
    struct FileStats { uint64_t operations=0,bytes=0; std::vector<uint64_t> durations; };
    std::map<std::pair<std::wstring,uint32_t>,FileStats> files;
    for (const TraceEvent& event : trace.events) {
        if (!MatchesEventSelection(trace, event, options, names)) continue;
        if (event.kind == TraceEventKind::FileIo) {
            ++file_io;
            ++file_operations[event.event_id];
            if(event.event_id>=64&&event.event_id<=77&&!event.is_completion) {
                auto& row=files[{event.file_path.empty()?L"[unknown]":event.file_path,event.operation_id}];
                ++row.operations; row.bytes+=event.size;
            }
            if(event.is_completion&&event.has_duration)files[{event.file_path.empty()?L"[unknown]":event.file_path,event.operation_id}].durations.push_back(event.duration_ns);
            if (event.has_duration) {
                ++file_duration_count;
                file_duration_total_ns += event.duration_ns;
                file_duration_max_ns = std::max(file_duration_max_ns, event.duration_ns);
            }
        } else if (event.kind == TraceEventKind::DiskIo) {
            ++disk_io;
            if (event.has_duration) {
                ++disk_duration_count;
                disk_duration_total_ns += event.duration_ns;
                disk_duration_max_ns = std::max(disk_duration_max_ns, event.duration_ns);
            }
        }
    }

    if (json) {
        JsonStreamWriter local_writer(std::wcout);
        JsonStreamWriter& writer = shared_writer == nullptr ? local_writer : *shared_writer;
        writer.BeginObject();
        writer.Key(L"quality_scope"); writer.String(L"capture");
        writer.Key(L"file_io"); writer.Unsigned(file_io);
        writer.Key(L"disk_io"); writer.Unsigned(disk_io);
        writer.Key(L"init"); writer.Unsigned(trace.quality.io_init_events);
        writer.Key(L"complete"); writer.Unsigned(trace.quality.io_complete_events);
        writer.Key(L"orphan"); writer.Unsigned(trace.quality.io_orphaned);
        writer.Key(L"file_duration_count"); writer.Unsigned(file_duration_count);
        writer.Key(L"file_duration_total_ns"); writer.Unsigned(file_duration_total_ns);
        writer.Key(L"file_duration_max_ns"); writer.Unsigned(file_duration_max_ns);
        writer.Key(L"disk_duration_count"); writer.Unsigned(disk_duration_count);
        writer.Key(L"disk_duration_total_ns"); writer.Unsigned(disk_duration_total_ns);
        writer.Key(L"disk_duration_max_ns"); writer.Unsigned(disk_duration_max_ns);
        writer.Key(L"files");
        writer.BeginArray();
        for (const auto& [key,row] : files) {
            nlohmann::ordered_json item;
            item["path"] = ToUtf8(key.first);
            item["operation"] = key.second;
            item["operations"] = row.operations;
            item["bytes"] = row.bytes;
            item["p50_ns"] = Percentile(row.durations,0.5);
            item["p95_ns"] = Percentile(row.durations,0.95);
            item["p99_ns"] = Percentile(row.durations,0.99);
            writer.Item(item);
        }
        writer.EndArray();
        writer.EndObject();
        std::wcout << L"\n";
        return file_io + disk_io;
    }

    std::wcout << L"Kind     Events  Timed  AvgUs  MaxUs\n";
    const uint64_t file_average =
        file_duration_count == 0 ? 0 : file_duration_total_ns / file_duration_count;
    const uint64_t disk_average =
        disk_duration_count == 0 ? 0 : disk_duration_total_ns / disk_duration_count;
    std::wcout << L"FileIo  " << file_io << L"  " << file_duration_count << L"  "
               << file_average / 1000 << L"  " << file_duration_max_ns / 1000
               << L"\n";
    std::wcout << L"DiskIo  " << disk_io << L"  " << disk_duration_count << L"  "
               << disk_average / 1000 << L"  " << disk_duration_max_ns / 1000
               << L"\n";
    std::wcout<<L"File path / operation / operations / bytes / P50Us / P95Us / P99Us\n";
    for(const auto& [key,row]:files)std::wcout<<key.first<<L" "<<key.second<<L" "<<row.operations<<L" "<<row.bytes
        <<L" "<<Percentile(row.durations,0.5)/1000<<L" "<<Percentile(row.durations,0.95)/1000<<L" "<<Percentile(row.durations,0.99)/1000<<L"\n";
    std::wcout << L"File operation event IDs\n";
    for (const auto& [id, count] : file_operations) {
        std::wcout << L"  " << id << L"  " << count << L"\n";
    }
    return file_io + disk_io;
}

bool WriteReportFile(const std::filesystem::path& path, const std::wstring& text,
                     std::wstring* error) {
    std::filesystem::path destination = path;
    if (destination.is_relative()) {
        destination = std::filesystem::absolute(destination);
    }
    if (!destination.parent_path().empty() &&
        !EnsureDirectory(destination.parent_path(), error)) {
        return false;
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    const std::string utf8 = ToUtf8(text);
    output.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    if (!output) {
        if (error != nullptr) {
            *error = L"cannot write report output: " + destination.wstring();
        }
        return false;
    }
    return true;
}

int RunReport(const ReportRequest& request) {
    if (IsDiagnosticKind(request.kind)) return RunDiagnosticReport(request);
    std::filesystem::path input = request.input;
    std::filesystem::path output_path = request.output;
    std::wstring symbol_path = request.symbol_path;
    std::filesystem::path archive_directory = request.archive;
    std::wstring kind = request.kind;
    CpuReportOptions report_options;
    report_options.pid = request.pid;
    report_options.weight_mode = request.weight == L"samples" ? CpuReportOptions::WeightMode::Samples : CpuReportOptions::WeightMode::Period;
    report_options.quiet = request.quiet;
    report_options.profile_source = request.source;
    report_options.tid = request.tid;
    report_options.comm_filter = request.comm;
    report_options.dso_filter = request.dso;
    report_options.symbol_filter = request.symbol;
    report_options.percent_limit = request.percent_limit.value_or(0.0);
    report_options.sort_fields = request.sort_fields;
    report_options.exact_stacks_only = request.exact_stacks;
    report_options.include_children = request.include_children;
    report_options.cpu = request.cpu;
    if (request.time_start_ms) report_options.start_offset_ns = *request.time_start_ms * 1000000;
    if (request.time_end_ms) report_options.end_offset_ns = *request.time_end_ms * 1000000;
    if (request.scope == L"user") report_options.scope = CpuReportOptions::Scope::User;
    else if (request.scope == L"kernel") report_options.scope = CpuReportOptions::Scope::Kernel;
    else if (request.scope == L"all") report_options.scope = CpuReportOptions::Scope::All;
    else { PrintError(L"unsupported report scope"); return ExitStatus(ExitCode::InvalidArguments); }
    report_options.folded = request.folded;
    report_options.off_cpu_include_inferred = request.off_cpu_include_inferred;
    report_options.csv = request.csv;
    report_options.json = request.json;
    if (request.call_graph) {
        report_options.call_tree = true;
        if (*request.call_graph == L"caller") report_options.call_tree_caller = true;
        else if (*request.call_graph == L"callee") report_options.call_tree_caller = false;
        else if (*request.call_graph == L"both") { PrintError(L"--call-graph both is not supported; use caller or callee"); return ExitStatus(ExitCode::InvalidArguments); }
    }
    const bool stdio_output = request.stdio_output;
    const bool quiet = request.quiet;
    const bool verbose = request.verbose;
    const bool metadata = request.metadata;
    const bool stream = request.stream;
    const bool off_cpu = request.off_cpu;
    const bool strict_quality = request.strict_quality;
    const std::optional<uint64_t> maximum_lost = request.max_lost_events;
    const std::optional<uint64_t> maximum_missing = request.max_missing_stacks;
    if (report_options.folded && report_options.call_tree) {
        PrintError(L"--folded and --call-graph are mutually exclusive");
        return 2;
    }
    if (report_options.json && report_options.csv) {
        PrintError(L"--json and --csv are mutually exclusive");
        return 2;
    }
    if (stdio_output && (report_options.json || report_options.csv)) {
        PrintError(L"--stdio cannot be combined with --json or --csv");
        return 2;
    }
    if (quiet && verbose) {
        PrintError(L"--quiet and --verbose are mutually exclusive");
        return 2;
    }
    if (off_cpu && (!kind.empty() || report_options.call_tree || stream)) {
        PrintError(L"--off-cpu cannot be combined with --kind, --call-graph, or --stream");
        return 2;
    }
    if (off_cpu && metadata && !report_options.json) {
        PrintError(L"--off-cpu --metadata requires --json");
        return 2;
    }
    if ((report_options.folded || report_options.call_tree) &&
        (report_options.json || report_options.csv)) {
        PrintError(L"graph output cannot be combined with --json or --csv");
        return 2;
    }
    if (kind == L"io" &&
        (report_options.folded || report_options.call_tree ||
         report_options.csv)) {
        PrintError(L"--kind io supports text or --json output");
        return 2;
    }
    if(report_options.include_children&&!report_options.pid) { PrintError(L"--include-children requires --pid"); return 2; }
    if(report_options.start_offset_ns&&report_options.end_offset_ns&&*report_options.start_offset_ns>*report_options.end_offset_ns)return 2;
    if (input.empty()) {
        PrintError(L"report requires -i FILE");
        return 2;
    }
    if (!output_path.empty() && !off_cpu &&
        ((!kind.empty() && kind != L"cpu") || stream || metadata)) {
        PrintError(L"--output currently supports CPU or off-CPU report output without --stream");
        return 2;
    }
    if (!kind.empty() && kind != L"cpu" && kind != L"io" && kind != L"pmc") {
        PrintError(L"unsupported report kind: " + kind);
        return 2;
    }
    if (input.is_relative()) {
        input = std::filesystem::absolute(input);
    }

    if ((kind == L"io" || kind == L"pmc") &&
        (!request.dso.empty() || !request.symbol.empty() || request.scope != L"all" ||
         request.call_graph || request.folded || request.exact_stacks || request.max_missing_stacks ||
         request.percent_limit || !request.sort_fields.empty() || request.weight_explicit ||
         !request.archive.empty() || !request.symbol_path.empty())) {
        PrintError(L"I/O and PMC snapshot reports do not support symbol, stack, scope, weight, sort, or percent options");
        return 2;
    }
    if ((kind == L"io" || off_cpu) && request.source) {
        PrintError(L"--source is only supported for CPU/PMC reports"); return 2;
    }
    if (off_cpu && request.weight_explicit) { PrintError(L"off-CPU reports always use duration weights"); return 2; }
    if (!off_cpu && request.off_cpu_include_inferred) { PrintError(L"--off-cpu-include-inferred requires --off-cpu"); return 2; }
    if (stream && maximum_missing) { PrintError(L"--max-missing-stacks requires loaded stacks"); return 2; }
    const auto quality_failed = [&](const TraceData& data) {
        return (maximum_lost && data.quality.lost_events > *maximum_lost) ||
               (maximum_missing && data.quality.samples_without_stack > *maximum_missing) ||
               (strict_quality && data.quality.incomplete);
    };
    TraceData trace;
    std::wstring error;
    if(stream&&(kind==L"io"||kind==L"pmc"||report_options.call_tree||report_options.folded||report_options.exact_stacks_only)) {
        PrintError(L"--stream supports CPU tables without stack expansion"); return 2;
    }
    if (!(stream ? ReadEtwMetadata(input,trace,&error) : ReadEtwFile(input, trace, &error, off_cpu))) {
        PrintError(error);
        return 6;
    }
    if (metadata && !report_options.json) { PrintError(L"--metadata requires --json"); return 2; }
    std::unique_ptr<JsonStreamWriter> envelope;
    if (metadata && output_path.empty()) {
        envelope = std::make_unique<JsonStreamWriter>(std::wcout);
        envelope->BeginObject();
        envelope->Key(L"rows");
    }
    const auto close_envelope = [&]() {
        if (envelope) {
            envelope->Key(L"quality");
            envelope->Item(QualityJson(trace));
            envelope->EndObject();
            std::wcout << L"\n";
        }
    };
    if ((kind == L"io" || kind == L"pmc") && !quiet) {
        if (verbose) std::wcerr << ToUtf16(QualityJson(trace).dump()) << L"\n";
        else std::wcerr << L"wperf: capture quality lost=" << trace.quality.lost_events
                        << L" decode_errors=" << trace.quality.decode_errors
                        << L" incomplete=" << trace.quality.incomplete << L"\n";
    }
    if (kind == L"pmc") {
        ProcessNameIndex names(trace);
        const auto rows = AggregatePmcCounters(trace, [&](uint64_t start, const TraceEvent& event) {
            if (!MatchesEventSelection(trace, event, report_options, names, true)) return false;
            const uint64_t origin = trace.start_ns == UINT64_MAX ? 0 : trace.start_ns;
            return !report_options.start_offset_ns || (start >= origin && start-origin >= *report_options.start_offset_ns);
        });
        if (trace.pmc_counter_sources.empty()) {
            PrintError(L"counter-source metadata unavailable");
            return 5;
        }
        uint64_t emitted = 0;
        if (report_options.json) {
            JsonStreamWriter local_writer(std::wcout);
            JsonStreamWriter& writer = envelope ? *envelope : local_writer;
            writer.BeginArray();
            for (const auto& row : rows) {

                if (report_options.profile_source && row.source != *report_options.profile_source) continue;
                ++emitted;
                nlohmann::ordered_json item;
                item["cpu"] = row.cpu;
                item["source"] = row.source;
                item["pid"] = row.pid;
                item["tid"] = row.tid;
                item["delta"] = row.delta;
                item["intervals"] = row.intervals;
                item["exact"] = false;
                item["semantics"] = "event-snapshot-derived";
                writer.Item(item);
            }
            writer.EndArray();
            close_envelope();
        } else {
            if (report_options.csv) std::wcout << L"cpu,source,pid,tid,delta,intervals,exact,semantics\n";
            else std::wcout << L"PMC event snapshots: estimated per-CPU/old-thread deltas, no multiplex scaling\nCPU Source PID TID Delta Intervals\n";
            for (const auto& row : rows) {

                if (report_options.profile_source && row.source != *report_options.profile_source) continue;
                ++emitted;
                const wchar_t* sep = report_options.csv ? L"," : L" ";
                std::wcout << row.cpu << sep << row.source << sep << row.pid << sep
                           << row.tid << sep << row.delta << sep << row.intervals;
                if (report_options.csv) std::wcout << L",false,event-snapshot-derived";
                std::wcout << L"\n";
            }
        }
        return quality_failed(trace) ? 6 : (emitted == 0 ? 5 : 0);
    }
    if (kind == L"io") {
        const auto emitted = WriteIoSummary(trace, report_options, envelope.get());
        close_envelope();
        return quality_failed(trace) ? 6 : (emitted == 0 ? 5 : 0);
    }


    if(!archive_directory.empty()) {
        if(!LoadArchiveModules(archive_directory,trace,&error)) { PrintError(error); return 6; }
        symbol_path+=L";"+ArchiveSymbolPath(archive_directory,trace);
    }
    Symbolizer symbolizer;
    std::wstring symbol_error;
    const bool symbols_ready = symbolizer.Initialize(symbol_path, &symbol_error);
    if (!symbols_ready && !symbol_error.empty()) {
        std::wcerr << L"wperf: symbolization disabled: " << symbol_error << L"\n";
    }
    for (const auto& image : trace.images) {
        symbolizer.AddModule(image);
    }

    if (off_cpu) {
        if (!output_path.empty()) {
            std::wostringstream result;
            std::unique_ptr<JsonStreamWriter> file_envelope;
            if (metadata) {
                file_envelope = std::make_unique<JsonStreamWriter>(result);
                file_envelope->BeginObject();
                file_envelope->Key(L"quality");
                file_envelope->Item(QualityJson(trace));
                file_envelope->Key(L"rows");
            }
            WriteOffCpuReport(trace, symbolizer, report_options, result, file_envelope.get());
            if (file_envelope) {
                file_envelope->EndObject();
                result << L"\n";
            }
            if (!WriteReportFile(output_path, result.str(), &error)) {
                PrintError(error);
                return 9;
            }
        } else {
            WriteOffCpuReport(trace, symbolizer, report_options, std::wcout, envelope.get());
            close_envelope();
        }
        const bool exceeds_loss =
            maximum_lost && trace.quality.lost_events > *maximum_lost;
        const bool exceeds_missing =
            maximum_missing && trace.quality.off_cpu_stack_missing > *maximum_missing;
        const bool strict_failure =
            strict_quality &&
            (trace.quality.incomplete || trace.quality.off_cpu_inferred_intervals != 0 ||
             trace.quality.off_cpu_evicted != 0);
        return trace.off_cpu_intervals.empty() || exceeds_loss || exceeds_missing ||
                       strict_failure
                   ? 6
                   : 0;
    }

    const uint64_t unattributed_samples = static_cast<uint64_t>(
        std::count_if(trace.samples.begin(), trace.samples.end(),
                      [](const SampleRecord& sample) {
                          return sample.pid == UINT32_MAX;
                      }));
    if (!quiet) {
        if (verbose) {
            std::wcerr << L"events=" << trace.quality.events_total
                       << L" unknown=" << trace.quality.unknown_events
                       << L" samples=" << trace.quality.samples_total
                       << L" stack_events=" << trace.quality.stack_events_total
                       << L" stack_refs=" << trace.quality.stack_ref_events_total
                       << L" stack_defs=" << trace.quality.stack_def_events_total
                       << L" stack_exact=" << trace.quality.stack_exact_total
                       << L" stack_approx=" << trace.quality.stack_approximate_total
                       << L" stack_key_mismatch=" << trace.quality.stack_key_mismatch
                       << L" ring_dropped=" << trace.quality.ring_dropped
                       << L" pmc_counter_events=" << trace.quality.pmc_counter_events
                       << L" lbr_events=" << trace.quality.lbr_events
                       << L" lbr_branches=" << trace.quality.lbr_branches
                       << L" context_register_events=" << trace.quality.context_register_events
                       << L" sched_unknown_pid=" << trace.quality.sched_unknown_pid
                       << L" io_init=" << trace.quality.io_init_events
                       << L" io_complete=" << trace.quality.io_complete_events
                       << L" io_orphan=" << trace.quality.io_orphaned
                       << L" unattributed=" << unattributed_samples
                       << L" samples_without_stack=" << trace.quality.samples_without_stack
                       << L" stack_ambiguous=" << trace.quality.stack_ambiguous
                       << L" decode_errors=" << trace.quality.decode_errors
                       << L" correlation_evicted=" << trace.quality.correlation_evicted
                       << L" incomplete=" << trace.quality.incomplete
                       << L" lost=" << trace.quality.lost_events
                       << L"\n";
        } else {
            std::wcerr << L"wperf: samples=" << trace.quality.samples_total
                       << L" unattributed=" << unattributed_samples
                       << L" exact=" << trace.quality.stack_exact_total
                       << L" approximate=" << trace.quality.stack_approximate_total
                       << L" missing=" << trace.quality.samples_without_stack
                       << L" ambiguous=" << trace.quality.stack_ambiguous
                       << L" decode_errors=" << trace.quality.decode_errors
                       << L" correlation_evicted=" << trace.quality.correlation_evicted
                       << L" incomplete=" << trace.quality.incomplete
                       << L" lost=" << trace.quality.lost_events
                       << L" unknown_events=" << trace.quality.unknown_events
                       << L"\n";
        }
    }
    if (stream) {
        WriteCpuReportFeed(trace, symbolizer, report_options, std::wcout,
                           [&](const auto& observe) {
                               if (!StreamEtwSamples(input, trace, observe, &error))
                                   throw std::runtime_error(ToUtf8(error));
                           }, envelope.get());
        close_envelope();
    } else if (!output_path.empty()) {
        std::wostringstream result;
        WriteCpuReport(trace, symbolizer, report_options, result);
        std::filesystem::path destination = output_path;
        if (destination.is_relative()) {
            destination = std::filesystem::absolute(destination);
        }
        if (!destination.parent_path().empty() &&
            !EnsureDirectory(destination.parent_path(), &error)) {
            PrintError(error);
            return 9;
        }
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        const std::string utf8 = ToUtf8(result.str());
        output.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        if (!output) {
            PrintError(L"cannot write report output: " + destination.wstring());
            return 9;
        }
    } else {
        WriteCpuReport(trace, symbolizer, report_options, std::wcout, envelope.get());
        close_envelope();
    }
    const bool exceeds_loss=maximum_lost&&trace.quality.lost_events>*maximum_lost;
    const bool exceeds_missing=maximum_missing&&trace.quality.samples_without_stack>*maximum_missing;
    return trace.quality.samples_total==0 || exceeds_loss || exceeds_missing || quality_failed(trace)?6:0;
}
}  // namespace wperf
