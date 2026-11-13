#include "trace_model.h"

#include <filesystem>
#include <algorithm>

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
        case TraceEventKind::LastBranchRecord:
            return L"lbr";
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
        const bool kernel = (trace.pointer_size == 8
                                 ? address >= 0xffff800000000000ull
                                 : address >= 0x80000000ull);
        if ((image.pid != pid && !(kernel && (image.pid == 0 || image.pid == 4))) ||
            address < image.base || address - image.base >= image.size) {
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

std::wstring ProcessDisplayName(const TraceData& trace, uint32_t pid,
                                uint64_t timestamp_ns) {
    if (pid == UINT32_MAX) {
        return L"[unattributed]";
    }
    if (pid == 0) {
        return L"Idle";
    }
    if (pid == 4) {
        return L"System";
    }
    if (const auto* process = FindProcess(trace, pid, timestamp_ns)) {
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
        if (timestamp_ns != UINT64_MAX &&
            ((image.load_ns != 0 && image.load_ns > timestamp_ns) ||
             (image.unload_ns != 0 && image.unload_ns < timestamp_ns))) continue;
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

uint64_t SampleWeightNs(const TraceData& trace, const SampleRecord& sample) {
    if (sample.kind != SampleKind::Cpu) return 0;
    return sample.period != 0 ? sample.period : trace.profile_interval_ns;
}

ProcessNameIndex::ProcessNameIndex(const TraceData& trace) {
    for (const auto& process : trace.processes) {
        if (!process.image_name.empty()) {
            const auto name = std::filesystem::path(process.image_name).filename().wstring();
            if (name.find(L".exe") != std::wstring::npos || name.find(L".EXE") != std::wstring::npos)
                names_[process.pid].push_back({process.start_ns, process.end_ns, name});
        }
    }
    for (const auto& image : trace.images) {
        const auto name = std::filesystem::path(image.path).filename().wstring();
        if (name.find(L".exe") != std::wstring::npos || name.find(L".EXE") != std::wstring::npos)
            names_[image.pid].push_back({image.load_ns, image.unload_ns, name});
    }
    for (auto& [pid, names] : names_) {
        (void)pid;
        std::stable_sort(names.begin(), names.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    }
}

std::wstring ProcessNameIndex::Name(uint32_t pid, uint64_t timestamp_ns) const {
    if (pid == UINT32_MAX) return L"[unattributed]";
    if (pid == 0) return L"Idle";
    if (pid == 4) return L"System";
    const auto found = names_.find(pid);
    if (found != names_.end()) {
        auto upper = std::upper_bound(found->second.begin(), found->second.end(), timestamp_ns,
                                      [](uint64_t time, const auto& name) { return time < name.start; });
        while (upper != found->second.begin()) {
            --upper;
            if (upper->end == 0 || timestamp_ns <= upper->end) return upper->name;
        }
    }
    return std::to_wstring(pid);
}

void ImageIndex::Build(const TraceData& trace) {
    trace_ = &trace;
    by_pid_.clear();
    for (size_t i = 0; i < trace.images.size(); ++i)
        by_pid_[trace.images[i].pid][trace.images[i].base].push_back(i);
}

const ImageRecord* ImageIndex::Find(uint32_t pid, uint64_t address,
                                   uint64_t timestamp_ns) const {
    if (trace_ == nullptr) return nullptr;
    const auto search = [&](uint32_t target) -> const ImageRecord* {
        const auto found = by_pid_.find(target);
        if (found == by_pid_.end()) return nullptr;
        auto upper = found->second.upper_bound(address);
        while (upper != found->second.begin()) {
            --upper;
            const ImageRecord* best = nullptr;
            for (size_t i : upper->second) {
                const auto& image = trace_->images[i];
                if (address - image.base >= image.size ||
                    (image.load_ns != 0 && image.load_ns > timestamp_ns) ||
                    (image.unload_ns != 0 && image.unload_ns < timestamp_ns)) continue;
                if (best == nullptr || image.load_ns >= best->load_ns) best = &image;
            }
            if (best != nullptr) return best;
        }
        return nullptr;
    };
    if (const auto* image = search(pid)) return image;
    const bool kernel = trace_->pointer_size == 8
                            ? address >= 0xffff800000000000ull : address >= 0x80000000ull;
    if (kernel) {
        if (const auto* image = search(0)) return image;
        return search(4);
    }
    return nullptr;
}

}  // namespace wperf
