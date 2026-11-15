#pragma once

#include "trace_model.h"
#include <unordered_map>

namespace wperf {

const wchar_t* TraceWaitReasonName(uint32_t reason);
const wchar_t* TraceThreadStateName(uint32_t state);
class TraceCorrelationState {
public:
    explicit TraceCorrelationState(size_t maximum_pending = 200000,
                                   uint64_t maximum_age_ns = 60000000000ull)
        : maximum_pending_(maximum_pending), maximum_age_ns_(maximum_age_ns) {}
    void Enrich(TraceData& trace);
    void Reset();
private:
    struct Pending {
        uint64_t timestamp_ns = 0;
        uint64_t offset = 0;
        uint64_t size = 0;
        uint64_t thread_start_ns = 0;
        uint32_t pid = 0;
        uint64_t file_key = 0;
        std::wstring file_path;
        uint32_t operation_id = 0;
        uint32_t tid = UINT32_MAX;
    };
    std::unordered_map<uint64_t, Pending> ready_, syscalls_, files_, disks_;
    std::unordered_map<uint64_t,std::wstring> file_names_;
    size_t maximum_pending_;
    uint64_t maximum_age_ns_;
    uint64_t last_prune_ns_ = 0;
};
void FinalizeTraceQuality(TraceData& trace);
void EnrichTraceEvents(TraceData& trace);

}  // namespace wperf
