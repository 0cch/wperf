#pragma once

#include "trace_model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wperf {

std::vector<std::wstring> EncodeLiveWindow(const TraceData& trace,
                                           uint64_t interval_ms);

class LiveWindowDecoder {
public:
    bool Begin(const std::wstring& line, std::wstring* error = nullptr);
    bool Consume(const std::wstring& line, std::wstring* error = nullptr);
    bool Finish(TraceData& trace, uint64_t& interval_ms,
                std::wstring* error = nullptr);
    bool Active() const { return active_; }

private:
    TraceData trace_;
    uint64_t interval_ms_ = 0;
    uint64_t expected_samples_ = 0;
    bool active_ = false;
};

}  // namespace wperf