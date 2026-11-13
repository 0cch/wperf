#include "top_renderer.h"

#include "archive.h"
#include "common.h"
#include "cpu_report.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iostream>
#include <mutex>
#include <utility>

namespace wperf {
namespace {

void BeginTopRender() {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode)) {
        if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 ||
            SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            std::wcout << L"\x1b[2J\x1b[H";
            return;
        }
    }
    std::wcout << L"\n===== wperf top =====\n";
}

bool RenderTopWindow(TraceData& trace, Symbolizer& symbolizer,
                     const std::filesystem::path& archive_directory,
                     const std::optional<uint32_t>& pid,
                     const std::wstring& sort_field, double percent_limit,
                     const std::wstring& profile_source, uint64_t interval_ms,
                     std::wstring* error) {
    if (!archive_directory.empty() &&
        !LoadArchiveModules(archive_directory, trace, error)) return false;
    symbolizer.PrepareTrace(trace, true);
    for (const auto& image : trace.images) symbolizer.AddModule(image);
    CpuReportOptions options;
    options.percent_limit = percent_limit;
    if (!sort_field.empty()) options.sort_fields.push_back(sort_field);
    options.pid = pid;
    if (profile_source != L"cpu" && !trace.samples.empty())
        options.profile_source = trace.samples.front().profile_source;
    const uint64_t unattributed = static_cast<uint64_t>(std::count_if(
        trace.samples.begin(), trace.samples.end(),
        [](const SampleRecord& sample) { return sample.pid == UINT32_MAX; }));
    BeginTopRender();
    std::wcout << L"wperf top  interval=" << interval_ms
               << L"ms  samples=" << trace.quality.samples_total
               << L"  events=" << trace.quality.events_total
               << L"  unattributed=" << unattributed
               << L"  unknown_events=" << trace.quality.unknown_events
               << L"  metadata_dropped=" << trace.quality.metadata_dropped
               << L"  pmc_samples=" << trace.quality.pmc_samples
               << L"  lost=" << trace.quality.lost_events
               << L"  lost_buffers=" << trace.quality.lost_buffers
               << L"  ring_dropped=" << trace.quality.ring_dropped
               << L"  incomplete=" << (trace.quality.incomplete ? 1 : 0) << L"\n";
    if (trace.samples.empty()) std::wcout << L"No CPU samples in this interval.\n";
    else WriteCpuReport(trace, symbolizer, options, std::wcout);
    std::wcout.flush();
    if (!std::wcout) {
        if (error != nullptr) *error = L"cannot write live top output";
        return false;
    }
    return true;
}

}  // namespace

struct AsyncTopRenderer::State {
    struct Job { TraceData trace; uint64_t interval_ms; };
    Render render;
    mutable std::mutex mutex;
    std::condition_variable wake, finished_cv;
    std::deque<Job> jobs;
    uint64_t dropped_windows = 0;
    bool active = false;
    bool stopping = false;
    bool drain = false;
    bool failed = false;
    bool finished = false;
    std::wstring error;
};

AsyncTopRenderer::AsyncTopRenderer(Render render) : state_(std::make_shared<State>()) {
    state_->render = std::move(render);
    thread_ = std::thread([state = state_] { Run(std::move(state)); });
}

AsyncTopRenderer::AsyncTopRenderer(std::shared_ptr<Symbolizer> symbolizer,
                                  std::filesystem::path archive_directory,
                                  std::optional<uint32_t> pid, std::wstring sort_field,
                                  double percent_limit, std::wstring profile_source)
    : AsyncTopRenderer([symbolizer = std::move(symbolizer),
                        archive_directory = std::move(archive_directory), pid,
                        sort_field = std::move(sort_field), percent_limit,
                        profile_source = std::move(profile_source)]
                       (TraceData& trace, uint64_t interval_ms, std::wstring* error) {
        return RenderTopWindow(trace, *symbolizer, archive_directory, pid,
                               sort_field, percent_limit, profile_source, interval_ms, error);
    }) {}

AsyncTopRenderer::~AsyncTopRenderer() { Close(false, 0); }

void AsyncTopRenderer::Push(TraceData trace, uint64_t interval_ms) {
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping || state_->failed) return;
        if (state_->jobs.size() >= 2) {
            state_->jobs.pop_front();
            ++state_->dropped_windows;
        }
        state_->jobs.push_back({std::move(trace), interval_ms});
    }
    state_->wake.notify_one();
}

bool AsyncTopRenderer::Failed(std::wstring* error) const {
    std::lock_guard lock(state_->mutex);
    if (!state_->failed) return false;
    if (error != nullptr) *error = state_->error;
    return true;
}

uint64_t AsyncTopRenderer::DroppedWindows() const {
    std::lock_guard lock(state_->mutex);
    return state_->dropped_windows;
}

bool AsyncTopRenderer::Close(bool drain, uint32_t timeout_ms) {
    std::unique_lock lock(state_->mutex);
    if (!thread_.joinable()) return state_->finished && !state_->failed;
    state_->stopping = true;
    state_->drain = drain;
    if (!drain) {
        state_->dropped_windows += state_->jobs.size();
        state_->jobs.clear();
    }
    state_->wake.notify_all();
    const bool finished = state_->finished_cv.wait_for(
        lock, std::chrono::milliseconds(timeout_ms), [state = state_] { return state->finished; });
    if (finished) {
        const bool success = !state_->failed;
        lock.unlock();
        thread_.join();
        return success;
    }
    if (drain) {
        state_->failed = true;
        state_->error = L"live top rendering exceeded its drain deadline; output is incomplete";
        state_->dropped_windows += state_->jobs.size() + (state_->active ? 1 : 0);
        state_->jobs.clear();
        state_->drain = false;
    }
    lock.unlock();
    // The thread retains its state and rendering resources until it returns.
    // Capture/control cleanup has no dependency on this remaining render work.
    thread_.detach();
    return false;
}

void AsyncTopRenderer::Run(std::shared_ptr<State> state) {
    while (true) {
        State::Job job;
        {
            std::unique_lock lock(state->mutex);
            state->wake.wait(lock, [&] { return state->stopping || !state->jobs.empty(); });
            if (state->stopping && (!state->drain || state->jobs.empty())) break;
            job = std::move(state->jobs.front());
            state->jobs.pop_front();
            state->active = true;
        }
        std::wstring error;
        bool success = false;
        try {
            success = state->render(job.trace, job.interval_ms, &error);
        } catch (const std::exception& exception) {
            error = L"live top rendering failed: " + ToUtf16(exception.what());
        } catch (...) {
            error = L"live top rendering failed with an unknown exception";
        }
        {
            std::lock_guard lock(state->mutex);
            state->active = false;
            if (!success) {
                if (!state->failed) {
                    state->failed = true;
                    state->error = error.empty() ? L"live top rendering failed" : std::move(error);
                    state->dropped_windows += state->jobs.size() + 1;
                }
                state->jobs.clear();
                break;
            }
        }
    }
    {
        std::lock_guard lock(state->mutex);
        state->finished = true;
    }
    state->finished_cv.notify_all();
}

}  // namespace wperf
