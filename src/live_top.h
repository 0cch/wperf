#pragma once

#include "etw.h"
#include "trace_model.h"
#include "trace_events.h"

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

    bool Start(std::wstring session_name, std::wstring profile_source,
               ULONG enable_flags, bool enable_lbr,
               std::wstring* error = nullptr);
    TraceData Snapshot();
    bool Stop();
    ProfileRestoreInfo RestoreInfo() const;
    uint32_t ProfileSourceId() const;
    bool Running() const;
    std::wstring LastError() const;

private:
    friend struct LiveCaptureTestAccess;
    static void WINAPI EventCallback(PEVENT_RECORD record);
    void OnEvent(PEVENT_RECORD record);
    void ParseImage(PEVENT_RECORD record);
    void ParseThread(PEVENT_RECORD record);
    void ParseProcessExit(PEVENT_RECORD record);
    void ParseSample(PEVENT_RECORD record);
    void ParseEvent(PEVENT_RECORD record, TraceEventKind kind,
                    uint64_t value1 = 0, uint64_t value2 = 0);
    void ParseLbr(PEVENT_RECORD record);
    uint64_t TimestampToNs(const EVENT_RECORD& record) const;

    EtwSession session_;
    std::wstring session_name_;
    std::atomic<TRACEHANDLE> trace_handle_{INVALID_PROCESSTRACE_HANDLE};
    std::thread worker_;
    std::atomic_bool running_{false};
    std::atomic_bool stopping_{false};

    mutable std::mutex mutex_;
    std::mutex snapshot_mutex_;
    std::vector<ImageRecord> images_;
    std::vector<ThreadRecord> threads_;
    std::deque<SampleRecord> samples_;
    std::deque<TraceEvent> events_;
    std::unordered_map<uint32_t, ThreadRecord> thread_to_process_;
    TraceCorrelationState correlation_;
    TraceData sample_metadata_;
    uint32_t profile_source_id_ = 0;
    uint32_t pointer_size_ = sizeof(void*);
    uint64_t qpc_frequency_ = 10000000;
    uint64_t events_total_ = 0;
    uint64_t unknown_events_ = 0;
    uint64_t lost_events_ = 0;
    uint64_t lost_buffers_ = 0;
    uint64_t decode_errors_ = 0;
    uint64_t metadata_dropped_ = 0;
    uint64_t ring_dropped_ = 0;
    uint64_t lbr_events_ = 0;
    uint64_t lbr_branches_ = 0;
    std::wstring last_error_;
};

}  // namespace wperf
