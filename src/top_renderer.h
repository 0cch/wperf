#pragma once

#include "symbolizer.h"
#include "trace_model.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace wperf {

class AsyncTopRenderer {
public:
    using Render = std::function<bool(TraceData&, uint64_t, std::wstring*)>;

    explicit AsyncTopRenderer(Render render);
    AsyncTopRenderer(std::shared_ptr<Symbolizer> symbolizer,
                     std::filesystem::path archive_directory,
                     std::optional<uint32_t> pid, std::wstring sort_field,
                     double percent_limit, std::wstring profile_source);
    ~AsyncTopRenderer();

    AsyncTopRenderer(const AsyncTopRenderer&) = delete;
    AsyncTopRenderer& operator=(const AsyncTopRenderer&) = delete;

    void Push(TraceData trace, uint64_t interval_ms);
    bool Failed(std::wstring* error) const;
    uint64_t DroppedWindows() const;
    // A normal drain that exceeds its deadline fails and counts unfinished work.
    // Cancellation can abandon rendering after the capture has been cleaned up.
    bool Close(bool drain, uint32_t timeout_ms);

private:
    struct State;
    static void Run(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
    std::thread thread_;
};

}  // namespace wperf
