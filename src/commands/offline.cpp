#include "commands/offline.h"

#include "commands/util.h"
#include "common.h"
#include "cpu_report.h"
#include "etl_reader.h"
#include "symbolizer.h"
#include "trace_model.h"

#include <algorithm>
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
        if (arguments[index] == L"-i" || arguments[index] == L"--input") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            input = arguments[++index];
        } else if (arguments[index] == L"-p" || arguments[index] == L"--pid") {
            uint32_t value = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], value)) {
                return 2;
            }
            pid = value;
        } else if (arguments[index] == L"--fields") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            fields = arguments[++index];
        } else if (arguments[index] == L"--limit") {
            if (index + 1 >= arguments.size() ||
                !ParseUnsigned(arguments[++index], limit)) {
                return 2;
            }
        } else if (arguments[index] == L"--symbol-path") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            symbol_path = arguments[++index];
        } else if (arguments[index] == L"--csv") {
            csv = true;
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf script -i FILE [-p PID] [--fields LIST] "
                          L"[--limit N] [--csv]\n";
            return 0;
        } else {
            PrintError(L"unknown script option: " + arguments[index]);
            return 2;
        }
    }
    if (input.empty()) {
        PrintError(L"script requires -i FILE");
        return 2;
    }

    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error)) {
        PrintError(error);
        return 6;
    }
    Symbolizer symbolizer;
    symbolizer.Initialize({}, nullptr);
    for (const auto& image : trace.images) {
        symbolizer.AddModule(image);
    }

    std::wcout << fields << L"\n";
    uint64_t emitted = 0;
    for (const auto& sample : trace.samples) {
        if (pid.has_value() && sample.pid != *pid) {
            continue;
        }
        if (limit != 0 && emitted++ >= limit) {
            break;
        }
        const ResolvedSymbol leaf = symbolizer.Resolve(
            trace, sample.pid, sample.instruction_pointer, sample.timestamp_ns);
        const std::wstring comm = ProcessDisplayName(trace, sample.pid);
        std::wostringstream row;
        size_t start = 0;
        bool first = true;
        while (start < fields.size()) {
            const size_t comma = fields.find(L',', start);
            const std::wstring field = fields.substr(
                start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
            if (!first) {
                row << (csv ? L"," : L" ");
            }
            first = false;
            if (field == L"timestamp") {
                row << sample.timestamp_ns;
            } else if (field == L"cpu") {
                row << sample.cpu;
            } else if (field == L"pid") {
                row << sample.pid;
            } else if (field == L"tid") {
                row << sample.tid;
            } else if (field == L"ip") {
                row << std::hex << sample.instruction_pointer << std::dec;
            } else if (field == L"comm") {
                row << comm;
            } else if (field == L"dso") {
                row << leaf.module;
            } else if (field == L"symbol") {
                row << leaf.symbol;
            } else if (field == L"stack") {
                for (size_t frame = 0; frame < sample.stack.size(); ++frame) {
                    if (frame != 0) {
                        row << L";";
                    }
                    row << std::hex << sample.stack[frame] << std::dec;
                }
            } else {
                row << L"";
            }
            if (comma == std::wstring::npos) {
                break;
            }
            start = comma + 1;
        }
        std::wcout << row.str() << L"\n";
    }
    return 0;
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
    return kind == filter;
}

int RunTrace(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    std::wstring filter = L"all";
    std::wstring pid_text;
    std::wstring max_text = L"500";
    bool json = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"-i" || arguments[index] == L"--input") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            input = arguments[++index];
        } else if (arguments[index] == L"-e" || arguments[index] == L"--event") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            filter = arguments[++index];
        } else if (arguments[index] == L"-p" || arguments[index] == L"--pid") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            pid_text = arguments[++index];
        } else if (arguments[index] == L"--max") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            max_text = arguments[++index];
        } else if (arguments[index] == L"--json") {
            json = true;
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf trace -i FILE [-e sched|syscall|pagefault|io] "
                          L"[-p PID] [--max N] [--json]\n";
            return 0;
        } else {
            PrintError(L"unknown trace option: " + arguments[index]);
            return 2;
        }
    }
    if (input.empty()) {
        PrintError(L"trace requires -i FILE for the offline M5 path");
        return 2;
    }

    TraceData trace;
    std::wstring error;
    if (!ReadEtwFile(input, trace, &error)) {
        PrintError(error);
        return 6;
    }
    std::optional<uint32_t> pid;
    if (!pid_text.empty()) {
        uint32_t value = 0;
        if (!ParseUint32(pid_text, value)) {
            return 2;
        }
        pid = value;
    }
    uint64_t max_events = 500;
    ParseUnsigned(max_text, max_events);

    uint64_t first_timestamp = UINT64_MAX;
    size_t emitted = 0;
    if (json) {
        std::wcout << L"[\n";
    } else {
        std::wcout << L"timestamp_ns cpu pid tid event values comm\n";
    }
    bool first_json = true;
    for (const auto& event : trace.events) {
        uint32_t display_pid = event.pid;
        uint32_t display_tid = event.tid;
        if ((display_pid == 0 || display_pid == UINT32_MAX) &&
            (event.kind == TraceEventKind::ContextSwitch ||
             event.kind == TraceEventKind::ReadyThread)) {
            display_tid = static_cast<uint32_t>(event.value1);
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
                       << L",\"comm\":\"" << comm << L"\"}";
        } else {
            std::wcout << relative << L" " << event.cpu << L" " << display_pid << L" "
                       << display_tid << L" " << TraceEventKindName(event.kind) << L" "
                       << event.value1 << L" " << event.value2 << L" " << comm << L"\n";
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
                return 2;
            }
            input = arguments[++index];
        } else if (arguments[index] == L"--help" || arguments[index] == L"-h") {
            std::wcout << L"Usage: wperf sched -i FILE\n";
            return 0;
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
        uint64_t context_switches = 0;
        uint64_t ready_threads = 0;
    };
    std::map<uint32_t, Counts> counts;
    for (const auto& event : trace.events) {
        uint32_t pid = event.pid;
        if ((pid == 0 || pid == UINT32_MAX) &&
            (event.kind == TraceEventKind::ContextSwitch ||
             event.kind == TraceEventKind::ReadyThread)) {
            if (const auto* thread = FindThread(
                    trace, static_cast<uint32_t>(event.value1), event.timestamp_ns)) {
                pid = thread->pid;
            }
        }
        if (event.kind == TraceEventKind::ContextSwitch) {
            ++counts[pid].context_switches;
        } else if (event.kind == TraceEventKind::ReadyThread) {
            ++counts[pid].ready_threads;
        }
    }
    std::wcout << L"Context switches / ready events by process\n";
    std::wcout << L"PID  Command  ContextSwitches  ReadyThreads\n";
    for (const auto& [pid, value] : counts) {
        std::wcout << pid << L"  " << ProcessDisplayName(trace, pid) << L"  "
                   << value.context_switches << L"  " << value.ready_threads << L"\n";
    }
    return 0;
}
int RunReport(const std::vector<std::wstring>& arguments) {
    std::filesystem::path input;
    std::wstring symbol_path;
    CpuReportOptions report_options;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == L"-i" || argument == L"--input") {
            if (index + 1 >= arguments.size()) {
                PrintError(L"--input requires a trace file");
                return 2;
            }
            input = arguments[++index];
        } else if (argument == L"--pid") {
            uint32_t pid = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], pid)) {
                PrintError(L"--pid requires a process id");
                return 2;
            }
            report_options.pid = pid;
        } else if (argument == L"--tid") {
            uint32_t tid = 0;
            if (index + 1 >= arguments.size() ||
                !ParseUint32(arguments[++index], tid)) {
                PrintError(L"--tid requires a thread id");
                return 2;
            }
            report_options.tid = tid;
        } else if (argument == L"--comm") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            report_options.comm_filter = arguments[++index];
        } else if (argument == L"--dso") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            report_options.dso_filter = arguments[++index];
        } else if (argument == L"--symbol") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            report_options.symbol_filter = arguments[++index];
        } else if (argument == L"--symbol-path") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            symbol_path = arguments[++index];
        } else if (argument == L"--percent-limit") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            report_options.percent_limit = wcstod(arguments[++index].c_str(), nullptr);
        } else if (argument == L"--folded") {
            report_options.folded = true;
        } else if (argument == L"--call-graph") {
            report_options.call_tree = true;
            if (index + 1 < arguments.size() &&
                (arguments[index + 1] == L"caller" || arguments[index + 1] == L"callee" ||
                 arguments[index + 1] == L"both")) {
                ++index;
            }
        } else if (argument == L"--csv") {
            report_options.csv = true;
        } else if (argument == L"--json") {
            report_options.json = true;
        } else if (argument == L"--sort") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            const std::wstring value = arguments[++index];
            size_t start = 0;
            while (start < value.size()) {
                const size_t comma = value.find(L',', start);
                report_options.sort_fields.push_back(
                    value.substr(start, comma == std::wstring::npos
                                            ? std::wstring::npos
                                            : comma - start));
                if (comma == std::wstring::npos) {
                    break;
                }
                start = comma + 1;
            }
        } else if (argument == L"--fields") {
            if (index + 1 >= arguments.size()) {
                return 2;
            }
            ++index;
        } else if (argument == L"--help" || argument == L"-h") {
            std::wcout << L"Usage: wperf report -i FILE [--pid PID] [--tid TID] "
                          L"[--comm TEXT] [--dso TEXT] [--symbol TEXT] "
                          L"[--percent-limit N] [--folded|--call-graph] [--csv|--json]\n";
            return 0;
        } else {
            PrintError(L"unknown report option: " + argument);
            return 2;
        }
    }

    if (input.empty()) {
        PrintError(L"report requires -i FILE");
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

    Symbolizer symbolizer;
    std::wstring symbol_error;
    const bool symbols_ready = symbolizer.Initialize(symbol_path, &symbol_error);
    if (!symbols_ready && !symbol_error.empty()) {
        std::wcerr << L"wperf: symbolization disabled: " << symbol_error << L"\n";
    }
    for (const auto& image : trace.images) {
        symbolizer.AddModule(image);
    }


    std::wcerr << L"events=" << trace.quality.events_total
               << L" unknown=" << trace.quality.unknown_events
               << L" samples=" << trace.quality.samples_total
               << L" stack_events=" << trace.quality.stack_events_total
               << L" stack_refs=" << trace.quality.stack_ref_events_total
               << L" stack_defs=" << trace.quality.stack_def_events_total
               << L" stack_exact=" << trace.quality.stack_exact_total
               << L" stack_approx=" << trace.quality.stack_approximate_total
               << L" stack_key_mismatch=" << trace.quality.stack_key_mismatch
               << L" samples_without_stack=" << trace.quality.samples_without_stack
               << L" stack_ambiguous=" << trace.quality.stack_ambiguous
               << L" lost=" << trace.quality.lost_events
               << L"\n";
    WriteCpuReport(trace, symbolizer, report_options, std::wcout);
    return trace.samples.empty() ? 6 : 0;
}
}  // namespace wperf
