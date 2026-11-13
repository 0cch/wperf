#include "commands/offline.h"

#include "commands/help.h"
#include "commands/live_control.h"
#include "commands/util.h"
#include "common.h"
#include "cpu_report.h"
#include "elevation.h"
#include "etl_reader.h"
#include "symbolizer.h"
#include "trace_events.h"
#include "trace_model.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

namespace wperf {
using namespace commands;

int RunScript(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    std::optional<uint32_t> pid;
    std::wstring fields = L"timestamp,cpu,pid,tid,ip,comm,dso,symbol";
    std::wstring symbol_path;
    uint64_t limit = 0;
    bool csv = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == L"-i" || argument == L"--input") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--input requires a trace file");
                return 2;
            }
            input = arguments[++index];
        } else if (argument == L"-p" || argument == L"--pid") {
            uint32_t value = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], value)) {
                PrintError(L"--pid requires a process id");
                return 2;
            }
            pid = value;
        } else if (argument == L"-F" || argument == L"--fields") {
            if (index + 1 >= arguments.size() || arguments[index + 1].empty()) {
                PrintError(L"--fields requires a comma-separated field list");
                return 2;
            }
            fields = arguments[++index];
        } else if (argument == L"--limit") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], limit)) {
                PrintError(L"--limit requires a non-negative sample count");
                return 2;
            }
        } else if (argument == L"--symbol-path") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--symbol-path requires a directory");
                return 2;
            }
            symbol_path = arguments[++index];
        } else if (argument == L"--csv") {
            csv = true;
        } else if (argument == L"--help" || argument == L"-h") {
            PrintCommandHelp(L"script");
            return 0;
        } else {
            PrintError(L"unknown script option: " + argument);
            return 2;
        }
    }
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
            field != L"dso" && field != L"symbol" && field != L"stack") {
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
    if (!ReadEtwFile(input, trace, &error)) {
        PrintError(error);
        return 6;
    }
    Symbolizer symbolizer;
    symbolizer.Initialize(symbol_path, nullptr);
    for (const auto& image : trace.images) {
        symbolizer.AddModule(image);
    }

    for (size_t field_index = 0; field_index < field_list.size(); ++field_index) {
        if (field_index != 0) {
            std::wcout << (csv ? L"," : L" ");
        }
        std::wcout << field_list[field_index];
    }
    std::wcout << L"\n";

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
        const std::wstring comm = ProcessDisplayName(trace, sample.pid);
        for (size_t field_index = 0; field_index < field_list.size(); ++field_index) {
            if (field_index != 0) {
                std::wcout << (csv ? L"," : L" ");
            }
            const std::wstring& field = field_list[field_index];
            if (field == L"timestamp") {
                std::wcout << sample.timestamp_ns;
            } else if (field == L"cpu") {
                std::wcout << sample.cpu;
            } else if (field == L"pid") {
                std::wcout << sample.pid;
            } else if (field == L"tid") {
                std::wcout << sample.tid;
            } else if (field == L"ip") {
                std::wcout << std::hex << sample.instruction_pointer << std::dec;
            } else if (field == L"comm") {
                std::wcout << (csv ? CsvEscape(comm) : comm);
            } else if (field == L"dso") {
                std::wcout << (csv ? CsvEscape(leaf.module) : leaf.module);
            } else if (field == L"symbol") {
                std::wcout << (csv ? CsvEscape(leaf.symbol) : leaf.symbol);
            } else if (field == L"stack") {
                std::wostringstream stack;
                for (size_t frame = 0; frame < sample.stack.size(); ++frame) {
                    if (frame != 0) {
                        stack << L";";
                    }
                    stack << std::hex << sample.stack[frame] << std::dec;
                }
                std::wcout << (csv ? CsvEscape(stack.str()) : stack.str());
            }
        }
        std::wcout << L"\n";
        ++emitted;
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
        if (line == L"JSON_BEGIN") {
            std::wcout << L"[\n";
        } else if (line == L"JSON_END") {
            std::wcout << L"\n]\n";
        } else if (line.rfind(L"OUT ", 0) == 0) {
            std::wcout << line.substr(4) << L"\n";
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
    return kind == filter;
}

std::wstring EventDetail(const TraceEvent& event) {
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
        case TraceEventKind::LastBranchRecord:
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

int RunTrace(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    std::wstring filter = L"all";
    std::wstring pid_text;
    std::wstring max_text = L"500";
    std::wstring duration_text = L"0";
    bool duration_explicit = false;
    bool json = false;
    bool live = false;
    bool elevate = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == L"-i" || argument == L"--input") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--input requires a trace file");
                return 2;
            }
            input = arguments[++index];
        } else if (argument == L"-e" || argument == L"--event") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--event requires sched, syscall, pagefault, io, or lbr");
                return 2;
            }
            filter = arguments[++index];
        } else if (argument == L"-p" || argument == L"--pid") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--pid requires a process id");
                return 2;
            }
            pid_text = arguments[++index];
        } else if (argument == L"--max") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--max requires an event count");
                return 2;
            }
            max_text = arguments[++index];
        } else if (argument == L"--duration") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--duration requires milliseconds");
                return 2;
            }
            duration_text = arguments[++index];
            duration_explicit = true;
        } else if (argument == L"--json") {
            json = true;
        } else if (argument == L"--live") {
            live = true;
        } else if (argument == L"--elevate") {
            elevate = true;
        } else if (argument == L"--help" || argument == L"-h") {
            PrintCommandHelp(L"trace");
            return 0;
        } else {
            PrintError(L"unknown trace option: " + argument);
            return 2;
        }
    }
    if (filter != L"all" && filter != L"sched" && filter != L"syscall" &&
        filter != L"pagefault" && filter != L"io" && filter != L"lbr") {
        PrintError(L"unsupported trace event: " + filter);
        return 2;
    }
    std::optional<uint32_t> pid;
    if (!pid_text.empty()) {
        uint32_t value = 0;
        if (!ParseUint32(pid_text, value)) {
            PrintError(L"--pid requires a process id");
            return 2;
        }
        pid = value;
        pid_text = std::to_wstring(value);
    }
    uint64_t max_events = 500;
    if (!ParseUnsigned(max_text, max_events)) {
        PrintError(L"--max requires a non-negative event count");
        return 2;
    }
    if (duration_explicit) {
        uint64_t duration_ms = 0;
        if (!ParseUnsigned(duration_text, duration_ms)) {
            PrintError(L"--duration requires milliseconds");
            return 2;
        }
    }
    if (live) {
        if (!input.empty()) {
            PrintError(L"trace --live cannot be combined with -i FILE");
            return 2;
        }
        return RunLiveTrace(filter, pid_text, max_text, duration_text, json, elevate);
    }
    if (duration_explicit) {
        PrintError(L"--duration is only valid with trace --live");
        return 2;
    }
    if (elevate) {
        PrintError(L"--elevate is only valid with trace --live");
        return 2;
    }
    if (input.empty()) {
        PrintError(L"trace requires -i FILE for offline mode or --live");
        return 2;
    }

    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error)) {
        PrintError(error);
        return 6;
    }

    uint64_t first_timestamp = UINT64_MAX;
    size_t emitted = 0;
    if (json) {
        std::wcout << L"[\n";
    } else {
        std::wcout << L"timestamp_ns cpu pid tid event value1 value2 "
                      L"related_tid related_pid detail comm\n";
    }
    bool first_json = true;
    for (const auto& event : trace.events) {
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
        if (emitted++ >= max_events) {
            break;
        }
        if (first_timestamp == UINT64_MAX) {
            first_timestamp = event.timestamp_ns;
        }
        const uint64_t relative =
            event.timestamp_ns >= first_timestamp ? event.timestamp_ns - first_timestamp
                                                  : 0;
        const std::wstring comm = ProcessDisplayName(trace, display_pid);
        const std::wstring detail = EventDetail(event);
        if (json) {
            if (!first_json) {
                std::wcout << L",\n";
            }
            first_json = false;
            std::wcout << L"{\"ts\":" << relative << L",\"cpu\":" << event.cpu
                       << L",\"pid\":" << display_pid << L",\"tid\":" << display_tid
                       << L",\"event\":\"" << TraceEventKindName(event.kind)
                       << L"\",\"value1\":" << event.value1
                       << L",\"value2\":" << event.value2
                       << L",\"related_tid\":" << event.related_tid
                       << L",\"related_pid\":" << event.related_pid
                       << L",\"status\":" << event.status
                       << L",\"has_status\":" << (event.has_status ? L"true" : L"false")
                       << L",\"duration_ns\":" << event.duration_ns
                       << L",\"has_duration\":" << (event.has_duration ? L"true" : L"false")
                       << L",\"size\":" << event.size
                       << L",\"offset\":" << event.offset
                       << L",\"correlation_id\":" << event.correlation_id
                       << L",\"wait_reason\":" << event.wait_reason
                       << L",\"previous_state\":" << event.previous_state
                       << L",\"flags\":" << event.flags
                       << L",\"detail\":\"" << detail
                       << L"\",\"comm\":\"" << comm << L"\"}";
        } else {
            std::wcout << relative << L" " << event.cpu << L" " << display_pid << L" "
                       << display_tid << L" " << TraceEventKindName(event.kind) << L" "
                       << event.value1 << L" " << event.value2 << L" "
                       << event.related_tid << L" " << event.related_pid << L" "
                       << detail << L" " << comm << L"\n";
        }
    }
    if (json) {
        std::wcout << L"\n]\n";
    }
    return 0;
}

int RunSched(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"-i" || arguments[index] == L"--input") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--input requires a trace file");
                return 2;
            }
            input = arguments[++index];
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            PrintCommandHelp(L"sched");
            return 0;
        } else {
            PrintError(L"unknown sched option: " + arguments[index]);
            return 2;
        }
    }
    if (input.empty()) {
        PrintError(L"sched requires -i FILE");
        return 2;
    }
    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error)) {
        PrintError(error);
        return 6;
    }

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
            if (pid == 0 || pid == UINT32_MAX) {
                pid = event.related_pid;
            }
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
                target.wakeup_max_ns =
                    std::max(target.wakeup_max_ns, event.duration_ns);
            }
        } else if (event.kind == TraceEventKind::ReadyThread) {
            const uint32_t pid = event.related_pid;
            if (pid != 0 && pid != UINT32_MAX) {
                ++counts[pid].ready;
            }
        }
    }
    std::wcout << std::left << std::setw(8) << L"PID" << std::setw(26)
               << L"Command" << std::right << std::setw(9) << L"Running"
               << std::setw(9) << L"Ready" << std::setw(9) << L"Blocked"
               << std::setw(9) << L"Wakeups" << std::setw(13)
               << L"WakeupAvgUs" << std::setw(13) << L"WakeupMaxUs"
               << L"  TopWaitReason\n";
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
        const uint64_t average_ns =
            value.wakeups == 0 ? 0 : value.wakeup_total_ns / value.wakeups;
        std::wcout << std::left << std::setw(8) << pid << std::setw(26)
                   << ClampText(ProcessDisplayName(trace, pid), 24) << std::right
                   << std::setw(9) << value.running << std::setw(9)
                   << value.ready << std::setw(9) << value.blocked
                   << std::setw(9) << value.wakeups << std::setw(13)
                   << average_ns / 1000 << std::setw(13)
                   << value.wakeup_max_ns / 1000 << L"  "
                   << (top_reason_count == 0 ? L"-"
                                              : TraceWaitReasonName(top_reason))
                   << L"\n";
    }
    return 0;
}
void WriteIoSummary(const TraceData& trace, bool json) {
    uint64_t file_io = 0;
    uint64_t disk_io = 0;
    uint64_t file_duration_total_ns = 0;
    uint64_t disk_duration_total_ns = 0;
    uint64_t file_duration_max_ns = 0;
    uint64_t disk_duration_max_ns = 0;
    uint64_t file_duration_count = 0;
    uint64_t disk_duration_count = 0;
    std::map<uint32_t, uint64_t> file_operations;
    for (const TraceEvent& event : trace.events) {
        if (event.kind == TraceEventKind::FileIo) {
            ++file_io;
            ++file_operations[event.event_id];
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
        std::wcout << L"{\"file_io\":" << file_io
                   << L",\"disk_io\":" << disk_io
                   << L",\"init\":" << trace.quality.io_init_events
                   << L",\"complete\":" << trace.quality.io_complete_events
                   << L",\"orphan\":" << trace.quality.io_orphaned
                   << L",\"file_duration_count\":" << file_duration_count
                   << L",\"file_duration_total_ns\":" << file_duration_total_ns
                   << L",\"file_duration_max_ns\":" << file_duration_max_ns
                   << L",\"disk_duration_count\":" << disk_duration_count
                   << L",\"disk_duration_total_ns\":" << disk_duration_total_ns
                   << L",\"disk_duration_max_ns\":" << disk_duration_max_ns
                   << L"}\n";
        return;
    }

    std::wcout << L"IO quality  init=" << trace.quality.io_init_events
               << L" complete=" << trace.quality.io_complete_events
               << L" orphan=" << trace.quality.io_orphaned << L"\n";
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
    std::wcout << L"File operation event IDs\n";
    for (const auto& [id, count] : file_operations) {
        std::wcout << L"  " << id << L"  " << count << L"\n";
    }
}

int RunReport(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    std::wstring symbol_path;
    std::wstring kind;
    CpuReportOptions report_options;
    bool stdio_output = false;
    bool quiet = false;
    bool verbose = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == L"-i" || argument == L"--input") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--input requires a trace file");
                return 2;
            }
            input = arguments[++index];
        } else if (argument == L"--kind") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--kind requires cpu or io");
                return 2;
            }
            kind = arguments[++index];
        } else if (argument.rfind(L"--kind=", 0) == 0) {
            kind = argument.substr(7);
        } else if (argument == L"-p" || argument == L"--pid") {
            uint32_t pid = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], pid)) {
                PrintError(L"--pid requires a process id");
                return 2;
            }
            report_options.pid = pid;
        } else if (argument == L"-t" || argument == L"--tid") {
            uint32_t tid = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], tid)) {
                PrintError(L"--tid requires a thread id");
                return 2;
            }
            report_options.tid = tid;
        } else if (argument == L"--comm") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--comm requires text");
                return 2;
            }
            report_options.comm_filter = arguments[++index];
        } else if (argument == L"--dso") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--dso requires text");
                return 2;
            }
            report_options.dso_filter = arguments[++index];
        } else if (argument == L"--symbol") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--symbol requires text");
                return 2;
            }
            report_options.symbol_filter = arguments[++index];
        } else if (argument == L"--symbol-path") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--symbol-path requires a directory");
                return 2;
            }
            symbol_path = arguments[++index];
        } else if (argument == L"--percent-limit") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--percent-limit requires a number");
                return 2;
            }
            const std::wstring value = arguments[++index];
            wchar_t* parse_end = nullptr;
            report_options.percent_limit = wcstod(value.c_str(), &parse_end);
            if (parse_end == nullptr || *parse_end != L'\0' ||
                report_options.percent_limit < 0.0 ||
                report_options.percent_limit > 100.0) {
                PrintError(L"--percent-limit requires a number from 0 to 100");
                return 2;
            }
        } else if (argument == L"--folded" || argument == L"--collapsed") {
            report_options.folded = true;
        } else if (argument == L"-g" || argument == L"--call-graph") {
            report_options.call_tree = true;
            if (index + 1 < arguments.size() &&
                (arguments[index + 1] == L"caller" ||
                 arguments[index + 1] == L"callee")) {
                report_options.call_tree_caller =
                    arguments[++index] == L"caller";
            } else if (index + 1 < arguments.size() &&
                       arguments[index + 1] == L"both") {
                PrintError(L"--call-graph both is not supported; use caller or callee");
                return 2;
            }
        } else if (argument == L"--stdio") {
            stdio_output = true;
        } else if (argument == L"--quiet") {
            quiet = true;
        } else if (argument == L"--verbose") {
            verbose = true;
        } else if (argument == L"--csv") {
            report_options.csv = true;
        } else if (argument == L"--json") {
            report_options.json = true;
        } else if (argument == L"-s" || argument == L"--sort") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--sort requires one or more fields");
                return 2;
            }
            const std::wstring value = arguments[++index];
            size_t start = 0;
            while (start <= value.size()) {
                const size_t comma = value.find(L',', start);
                const std::wstring field = value.substr(
                    start, comma == std::wstring::npos ? std::wstring::npos
                                                       : comma - start);
                if (field != L"samples" && field != L"comm" && field != L"dso" &&
                    field != L"symbol") {
                    PrintError(L"unsupported report sort field: " + field);
                    return 2;
                }
                report_options.sort_fields.push_back(field);
                if (comma == std::wstring::npos) {
                    break;
                }
                start = comma + 1;
            }
        } else if (argument == L"--fields") {
            PrintError(L"report --fields is not supported; use script -F");
            return 2;
        } else if (argument == L"--help" || argument == L"-h") {
            PrintCommandHelp(L"report");
            return 0;
        } else {
            PrintError(L"unknown report option: " + argument);
            return 2;
        }
    }

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
    if (input.empty()) {
        PrintError(L"report requires -i FILE");
        return 2;
    }
    if (!kind.empty() && kind != L"cpu" && kind != L"io") {
        PrintError(L"unsupported report kind: " + kind);
        return 2;
    }
    if (input.is_relative()) {
        input = std::filesystem::absolute(input);
    }

    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error)) {
        PrintError(error);
        return 6;
    }
    if (kind == L"io") {
        WriteIoSummary(trace, report_options.json);
        return 0;
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
                       << L" sched_unknown_pid=" << trace.quality.sched_unknown_pid
                       << L" io_init=" << trace.quality.io_init_events
                       << L" io_complete=" << trace.quality.io_complete_events
                       << L" io_orphan=" << trace.quality.io_orphaned
                       << L" unattributed=" << unattributed_samples
                       << L" samples_without_stack=" << trace.quality.samples_without_stack
                       << L" stack_ambiguous=" << trace.quality.stack_ambiguous
                       << L" lost=" << trace.quality.lost_events
                       << L"\n";
        } else {
            std::wcerr << L"wperf: samples=" << trace.quality.samples_total
                       << L" unattributed=" << unattributed_samples
                       << L" exact=" << trace.quality.stack_exact_total
                       << L" approximate=" << trace.quality.stack_approximate_total
                       << L" missing=" << trace.quality.samples_without_stack
                       << L" ambiguous=" << trace.quality.stack_ambiguous
                       << L" lost=" << trace.quality.lost_events
                       << L" unknown_events=" << trace.quality.unknown_events
                       << L"\n";
        }
    }
    WriteCpuReport(trace, symbolizer, report_options, std::wcout);
    return trace.samples.empty() ? 6 : 0;
}
}  // namespace wperf