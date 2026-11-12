#include "trace_model.h"

#include <filesystem>

namespace wperf {

const wchar_t* TraceEventKindName(TraceEventKind kind) {
    switch (kind) {
        case TraceEventKind::ContextSwitch:
            return L"context-switch";
        case TraceEventKind::ReadyThread:
            return L"ready-thread";
        case TraceEventKind::SyscallEnter:
            return L"syscall-enter";
        case TraceEventKind::SyscallExit:
            return L"syscall-exit";
        case TraceEventKind::PageFault:
            return L"page-fault";
        case TraceEventKind::DiskIo:
            return L"disk-io";
        case TraceEventKind::FileIo:
            return L"file-io";
        case TraceEventKind::Unknown:
            return L"unknown";
    }
    return L"unknown";
}


const ProcessRecord* FindProcess(const TraceData& trace, uint32_t pid,
                                 uint64_t timestamp_ns) {
    const ProcessRecord* best = nullptr;
    for (const auto& process : trace.processes) {
        if (process.pid != pid) {
            continue;
        }
        if (process.start_ns != 0 && process.start_ns > timestamp_ns) {
            continue;
        }
        if (process.end_ns != 0 && process.end_ns < timestamp_ns) {
            continue;
        }
        if (best == nullptr || process.start_ns >= best->start_ns) {
            best = &process;
        }
    }
    return best;
}

const ThreadRecord* FindThread(const TraceData& trace, uint32_t tid,
                               uint64_t timestamp_ns) {
    const ThreadRecord* best = nullptr;
    for (const auto& thread : trace.threads) {
        if (thread.tid != tid) {
            continue;
        }
        if (thread.start_ns != 0 && thread.start_ns > timestamp_ns) {
            continue;
        }
        if (thread.end_ns != 0 && thread.end_ns < timestamp_ns) {
            continue;
        }
        if (best == nullptr || thread.start_ns >= best->start_ns) {
            best = &thread;
        }
    }
    return best;
}
const ImageRecord* FindImage(const TraceData& trace, uint32_t pid, uint64_t address,
                             uint64_t timestamp_ns) {
    const ImageRecord* best = nullptr;
    for (const auto& image : trace.images) {
        if (image.pid != pid || address < image.base ||
            address >= image.base + image.size) {
            continue;
        }
        if (image.load_ns != 0 && image.load_ns > timestamp_ns) {
            continue;
        }
        if (image.unload_ns != 0 && image.unload_ns < timestamp_ns) {
            continue;
        }
        if (best == nullptr || image.load_ns >= best->load_ns) {
            best = &image;
        }
    }
    return best;
}

std::wstring ProcessDisplayName(const TraceData& trace, uint32_t pid) {
    if (pid == 0) {
        return L"Idle";
    }
    if (pid == 4) {
        return L"System";
    }
    if (const auto* process = FindProcess(trace, pid, UINT64_MAX)) {
        if (!process->image_name.empty()) {
            const std::wstring name =
                std::filesystem::path(process->image_name).filename().wstring();
            if (name.find(L".exe") != std::wstring::npos ||
                name.find(L".EXE") != std::wstring::npos) {
                return name;
            }
        }
    }

    const ImageRecord* best = nullptr;
    for (const auto& image : trace.images) {
        if (image.pid != pid || image.path.empty()) {
            continue;
        }
        const std::wstring name =
            std::filesystem::path(image.path).filename().wstring();
        if (name.find(L".exe") == std::wstring::npos &&
            name.find(L".EXE") == std::wstring::npos) {
            continue;
        }
        if (best == nullptr || image.load_ns < best->load_ns) {
            best = &image;
        }
    }
    if (best != nullptr) {
        return std::filesystem::path(best->path).filename().wstring();
    }
    return std::to_wstring(pid);
}

}  // namespace wperf
