#pragma once

#include "etw.h"
#include "trace_model.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace wperf {

class LiveTopCapture {
public:
    LiveTopCapture() = default;
    ~LiveTopCapture();

    LiveTopCapture(const LiveTopCapture&) = delete;
    LiveTopCapture& operator=(const LiveTopCapture&) = delete;

    bool Start(std::wstring session_name, std::wstring profile_source = L"cpu",
               std::wstring* error = nullptr);
    TraceData Snapshot();
    void Stop();
    bool Running() const;
    std::wstring LastError() const;

private:
    static void WINAPI EventCallback(PEVENT_RECORD record);
    void OnEvent(PEVENT_RECORD record);
    void ParseImage(PEVENT_RECORD record);
    void ParseThread(PEVENT_RECORD record);
    void ParseSample(PEVENT_RECORD record);
    void ParseEvent(PEVENT_RECORD record, TraceEventKind kind,
                    uint64_t value1 = 0, uint64_t value2 = 0);
    uint64_t TimestampToNs(const EVENT_RECORD& record) const;

    EtwSession session_;
    std::wstring session_name_;
    TRACEHANDLE trace_handle_ = INVALID_PROCESSTRACE_HANDLE;
    std::thread worker_;
    std::atomic_bool running_{false};
    std::atomic_bool stopping_{false};

    mutable std::mutex mutex_;
    std::vector<ImageRecord> images_;
    std::vector<ThreadRecord> threads_;
    std::deque<SampleRecord> samples_;
    std::deque<TraceEvent> events_;
    std::unordered_map<uint32_t, uint32_t> thread_to_process_;
    uint32_t pointer_size_ = sizeof(void*);
    uint64_t qpc_frequency_ = 10000000;
    uint64_t events_total_ = 0;
    uint64_t lost_events_ = 0;
    uint64_t ring_dropped_ = 0;
    std::wstring last_error_;
};

}  // namespace wperf