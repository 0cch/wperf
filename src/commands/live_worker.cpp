#include "commands/live_worker.h"

#include "commands/util.h"
#include "common.h"
#include "output.h"
#include "cpu_report.h"
#include "ipc.h"
#include "live_data.h"
#include "live_top.h"
#include "session_guard.h"
#include "trace_events.h"
#include "trace_model.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <sstream>

namespace wperf {
using namespace commands;

namespace {

bool MatchesEventFilter(const TraceEvent& event, const std::wstring& filter) {
    if (filter.empty() || filter == L"all") {
        return true;
    }
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
    return TraceEventKindName(event.kind) == filter;
}

bool ReceiveCommand(PipeClient& pipe, HANDLE controller, bool& stop) {
    if(WaitForSingleObject(controller,0)!=WAIT_TIMEOUT)return false;
    if (!pipe.IsConnected()) return false;
    if (!pipe.HasData()) {
        return true;
    }
    std::wstring line;
    std::wstring error;
    if (!pipe.ReceiveLine(line, 1000, &error)) {
        return false;
    }
    if (line == L"STOP") {
        stop = true;
    }
    return true;
}

bool WaitInterruptibly(PipeClient& pipe, HANDLE controller, uint64_t duration_ms, bool& stop,
                       std::wstring& error) {
    const ULONGLONG deadline = GetTickCount64() + duration_ms;
    while (GetTickCount64() < deadline) {
        if (!ReceiveCommand(pipe,controller,stop)) {
            error = L"live control pipe disconnected";
            return false;
        }
        if (stop) {
            return true;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        const ULONGLONG remaining = deadline - now;
        Sleep(static_cast<DWORD>(std::min<ULONGLONG>(remaining, 50)));
    }
    return true;
}

bool SendWindowInterruptibly(PipeClient& pipe, HANDLE controller,
                                const std::vector<std::wstring>& lines, bool& stop,
                                std::wstring& error) {
    for (const std::wstring& line : lines) {
        if (!ReceiveCommand(pipe, controller, stop)) {
            error = L"live control pipe disconnected";
            return false;
        }
        if (stop) return true;
        if (!pipe.SendLine(line, &error, 2000)) return false;
    }
    return true;
}

bool SendTextLines(PipeClient& pipe, const std::wstring& prefix,
                   const std::wstring& text, std::wstring& error) {
    std::wistringstream stream(text);
    std::wstring line;
    while (std::getline(stream, line)) {
        if (!pipe.SendLine(prefix + line, &error)) {
            return false;
        }
    }
    return true;
}

struct LiveWorkerRequest {
    std::wstring pipe_path;
    std::wstring nonce;
    std::wstring mode;
    std::wstring session_name;
    std::wstring event_filter = L"all";
    std::wstring profile_source = L"cpu";
    uint32_t controller_pid = 0;
    std::optional<uint32_t> pid;
    uint64_t interval_ms = 1000;
    uint64_t duration_ms = 0;
    uint64_t max_events = 0;
    bool json = false;
};

bool ParseLiveWorkerRequest(const std::vector<std::wstring>& arguments,
                            LiveWorkerRequest& request) {
    for (size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        auto value = [&](std::wstring& target) {
            if (index + 1 >= arguments.size()) return false;
            target = arguments[++index];
            return true;
        };
        std::wstring text;
        if (argument == L"--pipe") { if (!value(request.pipe_path)) return false; }
        else if (argument == L"--nonce") { if (!value(request.nonce)) return false; }
        else if (argument == L"--mode") { if (!value(request.mode)) return false; }
        else if (argument == L"--session") { if (!value(request.session_name)) return false; }
        else if (argument == L"--event") { if (!value(request.event_filter)) return false; }
        else if (argument == L"--profile-source") { if (!value(request.profile_source)) return false; }
        else if (argument == L"--json") { request.json = true; }
        else if (argument == L"--controller-pid") { if (!value(text) || !ParseUint32(text, request.controller_pid)) return false; }
        else if (argument == L"--pid") {
            if (!value(text)) return false;
            uint32_t pid = 0;
            if (!ParseUint32(text, pid)) return false;
            request.pid = pid;
        } else if (argument == L"--interval") { if (!value(text) || !ParseUnsigned(text, request.interval_ms)) return false; }
        else if (argument == L"--duration") { if (!value(text) || !ParseUnsigned(text, request.duration_ms)) return false; }
        else if (argument == L"--max") { if (!value(text) || !ParseUnsigned(text, request.max_events)) return false; }
        else return false;
    }
    return !request.pipe_path.empty() && !request.nonce.empty() &&
           !request.session_name.empty() && request.controller_pid != 0 &&
           (request.mode == L"top" || request.mode == L"trace") &&
           request.interval_ms != 0;
}

}  // namespace

int RunLiveWorker(const std::vector<std::wstring>& arguments) {
    LiveWorkerRequest internal;
    if (!ParseLiveWorkerRequest(arguments, internal)) return 2;
    const std::wstring& pipe_path = internal.pipe_path;
    const std::wstring& nonce = internal.nonce;
    const std::wstring& mode = internal.mode;
    const std::wstring& session_name = internal.session_name;
    const std::wstring& event_filter = internal.event_filter;
    const std::wstring& profile_source = internal.profile_source;
    const uint32_t expected_controller_pid = internal.controller_pid;
    const std::optional<uint32_t> pid = internal.pid;
    const uint64_t interval_ms = internal.interval_ms;
    const uint64_t duration_ms = internal.duration_ms;
    const uint64_t max_events = internal.max_events;
    const bool json = internal.json;

    PipeClient pipe;
    std::wstring error;
    if (!pipe.Connect(pipe_path, 20000, &error)) {
        return 4;
    }
    uint32_t actual_controller_pid = 0;
    if (!pipe.ServerProcessId(actual_controller_pid) ||
        actual_controller_pid != expected_controller_pid) {
        return 4;
    }
    ScopedHandle controller(OpenProcess(SYNCHRONIZE,FALSE,expected_controller_pid));
    if(!controller.get()) { pipe.SendLine(L"FINISHED 4",nullptr); return 4; }
    if (!pipe.SendLine(L"HELLO " + nonce + L" " +
                           std::to_wstring(GetCurrentProcessId()), &error)) {
        return 4;
    }
    std::wstring command;
    if (!pipe.ReceiveLine(command, 20000, &error) || command != L"START") {
        return 4;
    }

    SessionGuard guard;
    if(!guard.Start(session_name,{},&error)) {
        pipe.SendLine(L"ERROR "+error,nullptr); pipe.SendLine(L"FINISHED 4",nullptr); return 4;
    }
    LiveTopCapture capture;
    capture.SetRecoveryWriter([&](const ProfileRestoreInfo& restore){ return guard.UpdateRestore(restore,&error); });
    const ULONG enable_flags = mode == L"top"
                                   ? TraceFlagsForProfile(L"cpu")
                                   : TraceFlagsForLiveFilter(event_filter);
    const bool enable_lbr = mode == L"trace" && event_filter == L"lbr";
    if (!capture.Start(session_name, profile_source, enable_flags, enable_lbr,
                       &error)) {
        pipe.SendLine(L"ERROR " + error, nullptr);
        pipe.SendLine(L"FINISHED 4", nullptr);
        return 4;
    }

    bool stop = false;
    uint64_t observed_lbr=0;
    int result = 0;
    const ULONGLONG started = GetTickCount64();
    if (mode == L"top") {
        while (!stop) {
            if (!WaitInterruptibly(pipe, controller.get(), interval_ms, stop, error)) {
                result = 5;
                break;
            }
            if (stop) {
                break;
            }
            if (!capture.Running()) {
                error = capture.LastError();
                result = 5;
                break;
            }
            TraceData trace = capture.Snapshot();
            const auto window = EncodeLiveWindow(trace, interval_ms);
            if (!SendWindowInterruptibly(pipe, controller.get(), window, stop, error)) {
                result = 5;
                break;
            }
            if (stop || result != 0) {
                break;
            }
            if (duration_ms != 0 && GetTickCount64() - started >= duration_ms) {
                break;
            }
        }
    } else {
        uint64_t emitted = 0;
        uint64_t first_timestamp = UINT64_MAX;
        if (json) {
            pipe.SendLine(L"JSON_BEGIN", &error);
        } else {
            pipe.SendLine(L"OUT timestamp_ns cpu pid tid event values comm", &error);
        }
        while (!stop && (max_events == 0 || emitted < max_events)) {
            if (!WaitInterruptibly(pipe,controller.get(),std::min<uint64_t>(interval_ms,250),stop,
                                   error)) {
                result = 5;
                break;
            }
            if (stop) {
                break;
            }
            if (!capture.Running()) { error = capture.LastError(); result = 5; break; }
            TraceData trace = capture.Snapshot();
            observed_lbr+=trace.quality.lbr_events;
            if (trace.quality.incomplete) {
                pipe.SendLine(L"WARN ring_dropped=" +
                                  std::to_wstring(trace.quality.ring_dropped) +
                                  L" lost=" + std::to_wstring(trace.quality.lost_events) +
                                  L" lost_buffers=" + std::to_wstring(trace.quality.lost_buffers),
                              &error);
            }
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
                if (!MatchesEventFilter(event, event_filter)) {
                    continue;
                }
                if (max_events != 0 && emitted >= max_events) {
                    break;
                }
                if (first_timestamp == UINT64_MAX) {
                    first_timestamp = event.timestamp_ns;
                }
                const uint64_t relative =
                    event.timestamp_ns >= first_timestamp
                        ? event.timestamp_ns - first_timestamp
                        : 0;
                const std::wstring comm = ProcessDisplayName(trace, display_pid, event.timestamp_ns);
                std::wstring line;
                if (json) {
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
                    item["wait_reason"] = event.wait_reason;
                    item["previous_state"] = event.previous_state;
                    item["flags"] = event.flags;
                    item["file_path"] = ToUtf8(event.file_path);
                    item["size"] = event.size;
                    item["offset"] = event.offset;
                    item["correlation_id"] = event.correlation_id;
                    item["comm"] = ToUtf8(comm);
                    line += ToUtf16(item.dump());
                } else {
                    line = std::to_wstring(relative) + L" " +
                           std::to_wstring(event.cpu) + L" " +
                           std::to_wstring(display_pid) + L" " +
                           std::to_wstring(display_tid) + L" " +
                           TraceEventKindName(event.kind) + L" " +
                           std::to_wstring(event.value1) + L" " +
                           std::to_wstring(event.value2) + L" " +
                           std::to_wstring(event.related_tid) + L" " +
                           std::to_wstring(event.related_pid) + L" " +
                           std::to_wstring(event.duration_ns / 1000) + L"us " + comm;
                }
                if (!SendWindowInterruptibly(pipe, controller.get(), {L"OUT " + line},
                                              stop, error)) {
                    result = 5;
                    break;
                }
                if (stop) break;
                ++emitted;
            }
            if (stop || result != 0) {
                break;
            }
            if (duration_ms != 0 && GetTickCount64() - started >= duration_ms) {
                break;
            }
        }
        if (json) {
            pipe.SendLine(L"JSON_END", &error);
        }
    }

    {
        WorkerStopWatchdog timeout;
        if(capture.Stop())guard.Complete();
        else { error=capture.LastError(); result=4; }
    }
    const auto final=capture.Snapshot();
    if(final.quality.incomplete)pipe.SendLine(L"WARN final_lost="+std::to_wstring(final.quality.lost_events)+
        L" final_lost_buffers="+std::to_wstring(final.quality.lost_buffers),nullptr);
    observed_lbr+=final.quality.lbr_events;
    if(mode==L"trace" && event_filter==L"lbr" && !stop && result==0 && observed_lbr==0) {
        error=L"LBR configuration produced no LBR events; unavailable"; result=5;
    }
    if (!error.empty()) {
        pipe.SendLine(L"ERROR " + error, nullptr);
    }
    pipe.SendLine(L"FINISHED " + std::to_wstring(result), nullptr);
    return result;
}

}  // namespace wperf
