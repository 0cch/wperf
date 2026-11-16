#include "diagnostic_report.h"
#include "analysis.h"
#include "common.h"
#include "json_writer.h"
#include "output.h"
#include <algorithm>
#include <filesystem>
#include <limits>
#include <set>
#include <sstream>
#include <tuple>

namespace wperf {
namespace {
constexpr size_t kMaximumRows = 100000, kMaximumPending = 200000;
bool Contains(std::wstring value, std::wstring filter) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    std::transform(filter.begin(), filter.end(), filter.begin(), towlower);
    return value.find(filter) != std::wstring::npos;
}
void Add(uint64_t& target, uint64_t value) {
    if (value > UINT64_MAX - target) throw CpuReportLimitError("diagnostic counter overflow");
    target += value;
}
bool Select(const TraceData& trace, const CpuReportOptions& options, uint64_t time,
            uint32_t pid, uint32_t tid, uint32_t cpu) {
    SampleRecord sample; sample.timestamp_ns = time; sample.pid = pid;
    sample.tid = tid; sample.cpu = cpu;
    CpuReportOptions selection = options;
    selection.exact_stacks_only = false;
    if (!MatchesSampleSelection(trace, sample, selection)) return false;
    return options.comm_filter.empty() || Contains(ProcessDisplayName(trace, pid, time), options.comm_filter);
}
uint64_t Boundary(const TraceData& trace, const std::optional<uint64_t>& offset, uint64_t fallback) {
    if (!offset) return fallback;
    const uint64_t origin = trace.start_ns == UINT64_MAX ? 0 : trace.start_ns;
    if (*offset > UINT64_MAX - origin) throw CpuReportLimitError("diagnostic time boundary overflow");
    return origin + *offset;
}
std::wstring ImageKey(const ImageIndex& images, uint32_t pid, uint64_t address, uint64_t time) {
    const auto* image = images.Find(pid, address, time);
    return image ? ModuleIdentityKey(*image) + L":" + std::to_wstring(image->load_ns) : L"[unmapped]";
}
std::wstring StackKey(const ImageIndex& images, const AllocationRecord& allocation) {
    std::wstring result;
    for (const uint64_t frame : allocation.stack) {
        const auto* image = images.Find(allocation.domain == MemoryDomain::Pool ? 0 : allocation.pid,
                                       frame, allocation.timestamp_ns);
        result += image ? ModuleIdentityKey(*image) + L":" + std::to_wstring(image->load_ns) +
                          L":" + std::to_wstring(frame - image->base) : std::to_wstring(frame);
        result += L";";
    }
    return result;
}
uint64_t AllocationRoutine(const ImageIndex& images, const AllocationRecord& allocation) {
    for (uint64_t frame : allocation.stack) {
        const auto* image = images.Find(allocation.domain == MemoryDomain::Pool ? 0 : allocation.pid,
                                       frame, allocation.timestamp_ns);
        if (!image) continue;
        const std::wstring name = std::filesystem::path(image->path).filename().wstring();
        if (_wcsicmp(name.c_str(), L"ntoskrnl.exe") != 0 && _wcsicmp(name.c_str(), L"ntdll.dll") != 0 &&
            _wcsicmp(name.c_str(), L"kernelbase.dll") != 0) return frame;
    }
    return allocation.stack.empty() ? 0 : allocation.stack.front();
}
const wchar_t* InterruptName(InterruptKind kind) {
    switch (kind) {
    case InterruptKind::Dpc: return L"dpc";
    case InterruptKind::TimerDpc: return L"timer-dpc";
    case InterruptKind::ThreadedDpc: return L"threaded-dpc";
    case InterruptKind::Isr: return L"isr";
    }
    return L"unknown";
}
std::wstring PoolTag(uint32_t value) {
    std::wstring result;
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned ch = (value >> (i * 8)) & 255u;
        result += ch >= 32 && ch <= 126 ? static_cast<wchar_t>(ch) : L'.';
    }
    return result;
}
}

bool IsDiagnosticKind(std::wstring_view kind) {
    return kind == L"dpc" || kind == L"isr" || kind == L"interrupt" ||
           kind == L"heap" || kind == L"pool" || kind == L"driver";
}

DiagnosticAnalysis AnalyzeDiagnostics(const TraceData& trace, std::wstring_view kind, const CpuReportOptions& options) {
    DiagnosticAnalysis result;
    result.incomplete = trace.quality.incomplete;
    ImageIndex images; images.Build(trace);
    ProcessNameIndex process_names(trace);
    ThreadIndex threads; threads.Build(trace);
    std::map<std::pair<uint32_t, uint64_t>, uint64_t> process_ends;
    for (const auto& process : trace.processes) process_ends[{process.pid, process.start_ns}] = process.end_ns;
    const uint64_t start = Boundary(trace, options.start_offset_ns, trace.start_ns == UINT64_MAX ? 0 : trace.start_ns);
    const uint64_t end = Boundary(trace, options.end_offset_ns, trace.end_ns);
    using ExecutionKey = std::tuple<uint32_t, std::wstring, uint32_t, uint32_t, uint64_t, uint64_t, std::wstring, uint32_t, uint32_t, std::wstring>;
    std::map<ExecutionKey, size_t> execution_keys;
    const auto span = [&](std::wstring name, uint32_t cpu, uint32_t pid, uint32_t tid,
                          uint64_t begin, uint64_t finish, uint64_t routine, uint32_t major, uint32_t minor, std::wstring basis) {
        if (begin > finish || finish < start || begin > end || (options.cpu && cpu != *options.cpu)) return;
        const uint64_t instance = pid == UINT32_MAX ? 0 : process_names.InstanceStart(pid, begin);
        const std::wstring identity = ImageKey(images, pid == UINT32_MAX ? 0 : pid, routine, begin);
        const ExecutionKey key{cpu, name, pid, tid, instance, routine, identity, major, minor, basis};
        auto found = execution_keys.find(key);
        if (found == execution_keys.end()) {
            if (result.execution.size() >= kMaximumRows) { ++result.budget_dropped; return; }
            ExecutionRow row; row.kind = std::move(name); row.cpu = cpu; row.pid = pid; row.tid = tid;
            row.process_start_ns = instance; row.routine = routine; row.timestamp_ns = begin;
            row.module_identity = identity; row.major = major; row.minor = minor; row.context_basis = std::move(basis);
            found = execution_keys.emplace(key, result.execution.size()).first;
            result.execution.push_back(std::move(row));
        }
        auto& row = result.execution[found->second];
        begin = std::max(begin, start); finish = std::min(finish, end);
        const uint64_t duration = finish - begin;
        Add(row.inclusive_ns, duration); row.max_ns = std::max(row.max_ns, duration);
        row.durations.push_back(duration); row.spans.emplace_back(begin, finish);
    };
    if (kind == L"dpc" || kind == L"isr" || kind == L"interrupt") {
        for (const auto& event : trace.interrupts) {
            if ((kind == L"dpc" && event.kind == InterruptKind::Isr) ||
                (kind == L"isr" && event.kind != InterruptKind::Isr)) continue;
            span(InterruptName(event.kind), event.cpu, UINT32_MAX, UINT32_MAX,
                 event.start_ns, event.end_ns, event.routine, 0, 0, L"system");
        }
    } else if (kind == L"driver") {
        using PendingKey = std::tuple<uint64_t, uint32_t, bool>;
        struct Pending { const DriverRecord* event; uint32_t pid, tid; const wchar_t* basis; };
        std::map<PendingKey, Pending> pending;
        std::map<uint32_t, std::vector<std::pair<uint64_t, uint32_t>>> running;
        for (const auto& event : trace.scheduling_contexts) running[event.cpu].emplace_back(event.timestamp_ns, event.tid);
        for (const auto& event : trace.events) if (event.kind == TraceEventKind::ContextSwitch)
            running[event.cpu].emplace_back(event.timestamp_ns, static_cast<uint32_t>(event.value1));
        for (auto& [cpu, switches] : running) { (void)cpu; std::stable_sort(switches.begin(), switches.end()); }
        const auto context = [&](const DriverRecord* event) {
            uint32_t pid = event->pid, tid = event->tid;
            const bool supplied_tid = tid != 0 && tid != UINT32_MAX;
            if (!supplied_tid) {
                const auto found = running.find(event->cpu);
                if (found != running.end()) {
                    const auto upper = std::upper_bound(found->second.begin(), found->second.end(), std::pair{event->timestamp_ns, UINT32_MAX});
                    if (upper != found->second.begin()) tid = std::prev(upper)->second;
                }
            }
            const wchar_t* basis = L"event-header";
            if (pid == 0 || pid == UINT32_MAX) {
                if (const auto* thread = threads.Find(tid, event->timestamp_ns)) { pid = thread->pid; basis = supplied_tid ? L"thread-lifecycle" : L"scheduler"; }
            } else if (!supplied_tid) basis = L"scheduler";
            if (!tid || tid == UINT32_MAX || !pid || pid == UINT32_MAX) { basis = L"unavailable"; ++result.driver_context_unknown; }
            if (!tid) tid = UINT32_MAX;
            if (!pid) pid = UINT32_MAX;
            return Pending{event, pid, tid, basis};
        };
        std::set<PendingKey> ambiguous;
        std::vector<const DriverRecord*> ordered;
        for (const auto& event : trace.driver_events) ordered.push_back(&event);
        std::stable_sort(ordered.begin(), ordered.end(), [](auto a, auto b) { return a->timestamp_ns < b->timestamp_ns; });
        for (const auto* event : ordered) {
            if (event->operation == DriverOperation::CompletionRoutine) { ++result.completion_points; continue; }
            const bool completion = event->operation == DriverOperation::CompleteEnter || event->operation == DriverOperation::CompleteReturn;
            const bool enter = event->operation == DriverOperation::DispatchEnter || event->operation == DriverOperation::CompleteEnter;
            const PendingKey key{event->irp, event->match_id, completion};
            if (enter) {
                if (pending.contains(key) || ambiguous.contains(key)) {
                    pending.erase(key); ambiguous.insert(key); ++result.duplicate_entries; continue;
                }
                if (pending.size() + ambiguous.size() >= kMaximumPending) { ++result.budget_dropped; continue; }
                pending[key] = context(event);
            } else {
                const auto found = pending.find(key);
                if (ambiguous.erase(key) || found == pending.end()) { ++result.unmatched_returns; continue; }
                const auto entry_context = found->second;
                const auto* entry = entry_context.event;
                const auto* thread = threads.Find(entry_context.tid, entry->timestamp_ns);
                const auto process_end = process_ends.find({entry_context.pid, process_names.InstanceStart(entry_context.pid, entry->timestamp_ns)});
                if (event->timestamp_ns < entry->timestamp_ns ||
                    (thread && thread->end_ns && thread->end_ns < event->timestamp_ns) ||
                    (process_end != process_ends.end() && process_end->second && process_end->second < event->timestamp_ns)) {
                    ++result.unmatched_returns;
                } else {
                    CpuReportOptions select = options; select.start_offset_ns.reset(); select.end_offset_ns.reset(); select.cpu.reset();
                    if (Select(trace, select, entry->timestamp_ns, entry_context.pid, entry_context.tid, entry->cpu))
                        span(completion ? L"driver-complete" : L"driver-dispatch", entry->cpu, entry_context.pid, entry_context.tid,
                             entry->timestamp_ns, event->timestamp_ns, entry->routine, entry->major, entry->minor, entry_context.basis);
                }
                pending.erase(found);
            }
        }
        result.unmatched_entries = pending.size() + ambiguous.size();
    } else {
        const MemoryDomain domain = kind == L"heap" ? MemoryDomain::Heap : MemoryDomain::Pool;
        using AddressKey = std::tuple<uint32_t, uint64_t, uint64_t, uint32_t, uint64_t>;
        using RowKey = std::tuple<uint32_t, uint64_t, uint64_t, uint32_t, uint32_t, uint32_t, std::wstring>;
        struct Live { uint64_t size; size_t row; };
        std::map<AddressKey, Live> live;
        std::map<RowKey, size_t> row_keys;
        struct Item { uint64_t time; const AllocationRecord* allocation; const ProcessRecord* exit; };
        std::vector<Item> ordered;
        for (const auto& event : trace.allocations) if (event.domain == domain && event.timestamp_ns <= end)
            ordered.push_back({event.timestamp_ns, &event, nullptr});
        if (domain == MemoryDomain::Heap) for (const auto& process : trace.processes)
            if (process.end_ns && process.end_ns <= end) ordered.push_back({process.end_ns, nullptr, &process});
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        const auto release = [&](auto found, int reason) {
            if (found->second.row != SIZE_MAX) {
                auto& row = result.memory[found->second.row];
                row.live_bytes -= found->second.size; --row.live_allocations;
                if (reason == 1) Add(row.lifecycle_released_bytes, found->second.size);
                else if (reason == 2) Add(row.invalidated_bytes, found->second.size);
                else { Add(row.freed_bytes, found->second.size); ++row.frees; }
            }
            live.erase(found);
        };
        for (const auto& item : ordered) {
            if (item.exit) {
                for (auto found = live.begin(); found != live.end();) {
                    const auto& [pid, instance, heap, session, address] = found->first;
                    (void)heap; (void)session; (void)address;
                    if (pid == item.exit->pid && instance == item.exit->start_ns) release(found++, true);
                    else ++found;
                }
                continue;
            }
            const auto& event = *item.allocation;
            const uint64_t instance = domain == MemoryDomain::Heap ? ProcessInstanceStart(trace, event.pid, event.timestamp_ns) : 0;
            const auto key = [&](uint64_t address) { return AddressKey{event.pid, instance, event.heap, event.session_id, address}; };
            if (event.operation == MemoryOperation::DestroyHeap) {
                for (auto found = live.begin(); found != live.end();) {
                    if (std::get<0>(found->first) == event.pid && std::get<1>(found->first) == instance &&
                        std::get<2>(found->first) == event.heap) release(found++, true);
                    else ++found;
                }
                continue;
            }
            if (event.operation == MemoryOperation::Free || event.operation == MemoryOperation::Reallocate) {
                if (event.operation == MemoryOperation::Reallocate) {
                    ++result.realloc_notifications;
                    // Native HeapReAlloc can first log its internal alloc/free,
                    // then notify the API-level realloc. The already observed
                    // result object must not be allocated a second time.
                    if (event.address == event.old_address && event.size == event.old_size) continue;
                    const auto current = live.find(key(event.address));
                    if (event.address != event.old_address && current != live.end() && current->second.size == event.size) {
                        const auto old = live.find(key(event.old_address));
                        if (old != live.end()) release(old, 0);
                        continue;
                    }
                }
                const auto found = live.find(key(event.operation == MemoryOperation::Free ? event.address : event.old_address));
                if (found == live.end()) ++result.unmatched_frees;
                else release(found, false);
                if (event.operation == MemoryOperation::Free) continue;
            }
            const auto duplicate = live.find(key(event.address));
            if (duplicate != live.end()) { ++result.address_reuse; release(duplicate, 2); }
            if (live.size() >= kMaximumPending) { ++result.budget_dropped; continue; }
            size_t row_index = SIZE_MAX;
            if (Select(trace, options, event.timestamp_ns, event.pid, event.tid, event.cpu) &&
                (!options.exact_stacks_only || event.stack_quality == StackQuality::Exact)) {
                const RowKey row_key{event.pid, instance, event.heap, event.tag, event.pool_type, event.session_id, StackKey(images, event)};
                auto found = row_keys.find(row_key);
                if (found == row_keys.end()) {
                    if (result.memory.size() >= kMaximumRows) { ++result.budget_dropped; continue; }
                    MemoryRow row; row.pid = event.pid; row.process_start_ns = instance; row.heap = event.heap;
                    row.tag = event.tag; row.pool_type = event.pool_type; row.session_id = event.session_id;
                    row.timestamp_ns = event.timestamp_ns; row.stack = event.stack; row.routine = AllocationRoutine(images, event);
                    found = row_keys.emplace(row_key, result.memory.size()).first;
                    result.memory.push_back(std::move(row));
                }
                row_index = found->second;
                auto& row = result.memory[row_index];
                ++row.allocations; Add(row.allocated_bytes, event.size);
                ++row.live_allocations; Add(row.live_bytes, event.size);
                row.peak_live_bytes = std::max(row.peak_live_bytes, row.live_bytes);
                if (event.stack.empty()) { ++row.missing_stacks; ++result.missing_stacks; }
            }
            live.emplace(key(event.address), Live{event.size, row_index});
        }
    }
    for (auto& row : result.execution) {
        row.p50_ns = Percentile(row.durations, 0.50); row.p95_ns = Percentile(row.durations, 0.95); row.p99_ns = Percentile(row.durations, 0.99);
        std::sort(row.spans.begin(), row.spans.end());
        uint64_t begin = 0, finish = 0;
        for (const auto& interval : row.spans) {
            if (interval.first > finish) { Add(row.busy_ns, finish - begin); begin = interval.first; finish = interval.second; }
            else finish = std::max(finish, interval.second);
        }
        Add(row.busy_ns, finish - begin);
    }
    result.incomplete = result.incomplete || result.address_reuse || result.duplicate_entries ||
        result.unmatched_entries || result.unmatched_returns || result.budget_dropped;
    return result;
}

uint64_t WriteDiagnosticRows(const TraceData& trace, const DiagnosticAnalysis& analysis,
    std::wstring_view kind, Symbolizer& symbols, const CpuReportOptions& options,
    std::wostream& output, JsonStreamWriter* envelope) {
    symbols.PrepareTrace(trace);
    ImageIndex images; images.Build(trace);
    JsonStreamWriter local(output);
    JsonStreamWriter& writer = envelope ? *envelope : local;
    if (options.json) writer.BeginArray();
    const bool memory = kind == L"heap" || kind == L"pool";
    if (options.csv) output << (memory ?
        L"kind,pid,process_start_ns,heap,tag,pool_type,session_id,dso,symbol,allocations,allocated_bytes,frees,freed_bytes,live_allocations,live_bytes,peak_live_bytes,lifecycle_released_bytes,invalidated_bytes,missing_stacks,module_provenance,semantics\n" :
        L"kind,cpu,pid,tid,context_basis,process_start_ns,routine,dso,symbol,count,inclusive_ns,busy_ns,max_ns,p50_ns,p95_ns,p99_ns,major,minor,semantics\n");
    else if (!options.json) output << (memory ?
        L"Kind PID Tag Allocations AllocatedBytes FreedBytes LiveBytes PeakLiveBytes Module Symbol ModuleProvenance\n" :
        L"Kind CPU Count InclusiveNs BusyNs MaxNs P95Ns Module Symbol\n");
    uint64_t emitted = 0;
    const auto matches = [&](const ResolvedSymbol& symbol) { return Contains(symbol.module, options.dso_filter) && Contains(symbol.symbol, options.symbol_filter); };
    if (!memory) {
        std::vector<const ExecutionRow*> ordered;
        for (const auto& row : analysis.execution) ordered.push_back(&row);
        std::stable_sort(ordered.begin(), ordered.end(), [](auto a, auto b) { return a->inclusive_ns > b->inclusive_ns; });
        for (const auto* value : ordered) {
            const auto& row = *value;
            const auto symbol = symbols.Resolve(trace, row.pid == UINT32_MAX ? 0 : row.pid, row.routine, row.timestamp_ns);
            if (!matches(symbol)) continue;
            ++emitted;
            const std::string semantics = kind == L"driver" ? "observed-call-span" : "inclusive-execution-span";
            if (options.json) {
                nlohmann::ordered_json item;
                item["kind"] = ToUtf8(row.kind); item["cpu"] = row.cpu;
                item["pid"] = row.pid == UINT32_MAX ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(row.pid);
                item["tid"] = row.tid == UINT32_MAX ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(row.tid);
                item["owner"] = kind == L"driver" ? "entry-context" : "system";
                item["context_basis"] = ToUtf8(row.context_basis);
                item["process_start_ns"] = row.process_start_ns; item["routine"] = row.routine;
                item["dso"] = ToUtf8(symbol.module); item["symbol"] = ToUtf8(symbol.symbol);
                item["count"] = row.durations.size(); item["inclusive_ns"] = row.inclusive_ns; item["busy_ns"] = row.busy_ns;
                item["max_ns"] = row.max_ns; item["p50_ns"] = row.p50_ns; item["p95_ns"] = row.p95_ns; item["p99_ns"] = row.p99_ns;
                item["major"] = row.major; item["minor"] = row.minor; item["semantics"] = semantics;
                writer.Item(item);
            } else if (options.csv) {
                output << row.kind << L"," << row.cpu << L",";
                if (row.pid != UINT32_MAX) output << row.pid;
                output << L","; if (row.tid != UINT32_MAX) output << row.tid;
                output << L"," << row.context_basis << L"," << row.process_start_ns << L"," << row.routine << L"," << EscapeCsv(symbol.module) << L"," << EscapeCsv(symbol.symbol)
                    << L"," << row.durations.size() << L"," << row.inclusive_ns << L"," << row.busy_ns << L"," << row.max_ns
                    << L"," << row.p50_ns << L"," << row.p95_ns << L"," << row.p99_ns << L"," << row.major << L"," << row.minor << L"," << ToUtf16(semantics) << L"\n";
            } else output << row.kind << L" " << row.cpu << L" " << row.durations.size() << L" " << row.inclusive_ns << L" "
                << row.busy_ns << L" " << row.max_ns << L" " << row.p95_ns << L" " << symbol.module << L" " << symbol.symbol << L"\n";
        }
    } else {
        std::vector<const MemoryRow*> ordered;
        for (const auto& row : analysis.memory) ordered.push_back(&row);
        std::stable_sort(ordered.begin(), ordered.end(), [](auto a, auto b) { return a->live_bytes > b->live_bytes; });
        for (const auto* value : ordered) {
            const auto& row = *value;
            const auto symbol = symbols.Resolve(trace, row.pid == UINT32_MAX ? 0 : row.pid, row.routine, row.timestamp_ns);
            if (!matches(symbol)) continue;
            const auto* image = images.Find(row.pid == UINT32_MAX ? 0 : row.pid, row.routine, row.timestamp_ns);
            const std::string provenance = !image ? "unavailable" : image->snapshot_only ? "attach-snapshot" : "etw";
            ++emitted;
            if (options.json) {
                nlohmann::ordered_json item;
                item["kind"] = ToUtf8(std::wstring(kind));
                item["pid"] = row.pid == UINT32_MAX ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(row.pid);
                item["owner"] = kind == L"pool" ? "system" : "process";
                item["process_start_ns"] = row.process_start_ns; item["heap"] = row.heap;
                item["tag"] = kind == L"pool" ? ToUtf8(PoolTag(row.tag)) : ""; item["tag_value"] = row.tag;
                item["pool_type"] = row.pool_type;
                item["session_id"] = row.session_id == UINT32_MAX ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(row.session_id);
                item["dso"] = ToUtf8(symbol.module); item["symbol"] = ToUtf8(symbol.symbol);
                item["allocations"] = row.allocations; item["allocated_bytes"] = row.allocated_bytes;
                item["frees"] = row.frees; item["freed_bytes"] = row.freed_bytes;
                item["live_allocations"] = row.live_allocations; item["live_bytes"] = row.live_bytes;
                item["peak_live_bytes"] = row.peak_live_bytes; item["lifecycle_released_bytes"] = row.lifecycle_released_bytes;
                item["invalidated_bytes"] = row.invalidated_bytes;
                item["missing_stacks"] = row.missing_stacks; item["module_provenance"] = provenance; item["semantics"] = "observed-unfreed";
                item["allocation_stack"] = nlohmann::ordered_json::array();
                for (uint64_t frame : row.stack) {
                    const auto resolved = symbols.Resolve(trace, row.pid == UINT32_MAX ? 0 : row.pid, frame, row.timestamp_ns);
                    const auto* frame_image = images.Find(row.pid == UINT32_MAX ? 0 : row.pid, frame, row.timestamp_ns);
                    item["allocation_stack"].push_back({{"ip", frame}, {"dso", ToUtf8(resolved.module)}, {"symbol", ToUtf8(resolved.symbol)},
                        {"module_provenance", !frame_image ? "unavailable" : frame_image->snapshot_only ? "attach-snapshot" : "etw"}});
                }
                writer.Item(item);
            } else if (options.csv) {
                output << kind << L","; if (row.pid != UINT32_MAX) output << row.pid;
                output << L"," << row.process_start_ns << L"," << row.heap << L"," << EscapeCsv(kind == L"pool" ? PoolTag(row.tag) : L"")
                    << L"," << row.pool_type << L","; if (row.session_id != UINT32_MAX) output << row.session_id;
                output << L"," << EscapeCsv(symbol.module) << L"," << EscapeCsv(symbol.symbol) << L"," << row.allocations << L"," << row.allocated_bytes
                    << L"," << row.frees << L"," << row.freed_bytes << L"," << row.live_allocations << L"," << row.live_bytes
                    << L"," << row.peak_live_bytes << L"," << row.lifecycle_released_bytes << L"," << row.invalidated_bytes << L"," << row.missing_stacks << L"," << ToUtf16(provenance) << L",observed-unfreed\n";
            } else output << kind << L" " << (row.pid == UINT32_MAX ? L"system" : std::to_wstring(row.pid)) << L" " << (kind == L"pool" ? PoolTag(row.tag) : L"-")
                << L" " << row.allocations << L" " << row.allocated_bytes << L" " << row.freed_bytes << L" " << row.live_bytes << L" "
                << row.peak_live_bytes << L" " << symbol.module << L" " << symbol.symbol << L" " << ToUtf16(provenance) << L"\n";
        }
    }
    if (options.json) { writer.EndArray(); if (!envelope) output << L"\n"; }
    return emitted;
}
}  // namespace wperf
