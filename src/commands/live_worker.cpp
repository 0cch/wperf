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

}  // namespace

int RunLiveWorker(const std::vector<std::wstring>& arguments) {
    const std::wstring pipe_path = OptionValue(arguments, L"--pipe");
    const std::wstring nonce = OptionValue(arguments, L"--nonce");
    const std::wstring controller_pid_text =
        OptionValue(arguments, L"--controller-pid");
    const std::wstring mode = OptionValue(arguments, L"--mode");
    const std::wstring session_name = OptionValue(arguments, L"--session");
    const std::wstring interval_text = OptionValue(arguments, L"--interval", L"1000");
    const std::wstring duration_text = OptionValue(arguments, L"--duration", L"0");
    const std::wstring max_text = OptionValue(arguments, L"--max", L"500");
    const std::wstring event_filter = OptionValue(arguments, L"--event", L"all");
    const std::wstring profile_source =
        OptionValue(arguments, L"--profile-source", L"cpu");

    const std::wstring pid_text = OptionValue(arguments, L"--pid");
    const bool json = HasFlag(arguments, L"--json");
    if (pipe_path.empty() || nonce.empty() || session_name.empty() ||
        (mode != L"top" && mode != L"trace")) {
        return 2;
    }

    uint64_t interval_ms = 1000;
    uint64_t duration_ms = 0;
    uint64_t max_events = 500;
    if (!ParseUnsigned(interval_text, interval_ms) || interval_ms == 0 ||
        !ParseUnsigned(duration_text, duration_ms) ||
        !ParseUnsigned(max_text, max_events)) {
        return 2;
    }

    std::optional<uint32_t> pid;
    if (!pid_text.empty()) {
        uint32_t value = 0;
        if (!ParseUint32(pid_text, value)) {
            return 2;
        }
        pid = value;
    }

    PipeClient pipe;
    std::wstring error;
    if (!pipe.Connect(pipe_path, 20000, &error)) {
        return 4;
    }
    uint32_t expected_controller_pid = 0;
    uint32_t actual_controller_pid = 0;
    if (!ParseUint32(controller_pid_text, expected_controller_pid) ||
        !pipe.ServerProcessId(actual_controller_pid) ||
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
            for (const std::wstring& line : window) {
                if (!pipe.SendLine(line, &error)) {
                    result = 5;
                    break;
                }
            }
            if (result != 0) {
                break;
            }
            if (duration_ms != 0 && GetTickCount64() - started >= duration_ms) {
                break;
            }
        }
    } else {
        uint64_t emitted = 0;
        uint64_t first_timestamp = UINT64_MAX;
        bool first_json = true;
        if (json) {
            pipe.SendLine(L"JSON_BEGIN", &error);
        } else {
            pipe.SendLine(L"OUT timestamp_ns cpu pid tid event values comm", &error);
        }
        while (!stop && emitted < max_events) {
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
                if (emitted++ >= max_events) {
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
                    if (!first_json) {
                        line += L",";
                    }
                    first_json = false;
                    line += L"{\"ts\":" + std::to_wstring(relative) +
                            L",\"cpu\":" + std::to_wstring(event.cpu) +
                            L",\"pid\":" + std::to_wstring(display_pid) +
                            L",\"tid\":" + std::to_wstring(display_tid) +
                            L",\"event\":\"" + TraceEventKindName(event.kind) +
                            L"\",\"value1\":" + std::to_wstring(event.value1) +
                            L",\"value2\":" + std::to_wstring(event.value2) +
                            L",\"related_tid\":" + std::to_wstring(event.related_tid) +
                            L",\"related_pid\":" + std::to_wstring(event.related_pid) +
                            L",\"status\":" + std::to_wstring(event.status) +
                            L",\"has_status\":" + (event.has_status ? L"true" : L"false") +
                            L",\"duration_ns\":" + std::to_wstring(event.duration_ns) +
                            L",\"has_duration\":" + (event.has_duration ? L"true" : L"false") +
                            L",\"wait_reason\":" + std::to_wstring(event.wait_reason) +
                            L",\"previous_state\":" + std::to_wstring(event.previous_state) +
                            L",\"flags\":" + std::to_wstring(event.flags) +
                            L",\"file_path\":\""+EscapeJson(event.file_path)+L"\""+
                            L",\"size\":" + std::to_wstring(event.size) +
                            L",\"offset\":" + std::to_wstring(event.offset) +
                            L",\"correlation_id\":" + std::to_wstring(event.correlation_id) +
                            L",\"comm\":\"" + EscapeJson(comm) + L"\"}";
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
                if (!pipe.SendLine(L"OUT " + line, &error)) {
                    result = 5;
                    break;
                }
            }
            if (result != 0) {
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
