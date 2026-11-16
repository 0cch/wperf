#include "diagnostic_decode.h"
#include "etw_decode.h"
#include <cstring>

namespace wperf {
namespace {
template<typename T> T Read(const EVENT_RECORD& record, size_t offset) {
    T result{};
    std::memcpy(&result, static_cast<const BYTE*>(record.UserData) + offset, sizeof(T));
    return result;
}
uint64_t Pointer(const EVENT_RECORD& r, size_t offset, uint32_t width) {
    return width == 8 ? Read<uint64_t>(r, offset) : Read<uint32_t>(r, offset);
}
void ExtendedStack(const EVENT_RECORD& r, AllocationRecord& allocation) {
    for (USHORT i = 0; i < r.ExtendedDataCount; ++i) {
        const auto& data = r.ExtendedData[i];
        const size_t width = data.ExtType == EVENT_HEADER_EXT_TYPE_STACK_TRACE64 ? 8 :
                             data.ExtType == EVENT_HEADER_EXT_TYPE_STACK_TRACE32 ? 4 : 0;
        if (!width || !data.DataPtr || data.DataSize < 8 + width || (data.DataSize - 8) % width != 0) continue;
        const auto* bytes = reinterpret_cast<const BYTE*>(static_cast<uintptr_t>(data.DataPtr));
        std::vector<uint64_t> stack;
        for (size_t offset = 8; offset < data.DataSize && stack.size() < 192; offset += width) {
            uint64_t address = 0;
            std::memcpy(&address, bytes + offset, width);
            stack.push_back(address);
        }
        allocation.stack = stack;
        allocation.stack_quality = StackQuality::Exact;
        return;
    }
}
}

bool IsDiagnosticClass(const GUID& c) {
    return IsEqualGUID(c, kHeapGuid) || IsEqualGUID(c, kPoolGuid);
}

bool DecodeDiagnosticRecord(const EVENT_RECORD& r, const GUID& c, TraceData& trace) {
    const auto code = r.EventHeader.EventDescriptor.Opcode;
    const auto version = r.EventHeader.EventDescriptor.Version;
    const uint32_t width = EtwPointerSize(r, trace.pointer_size);
    const bool interrupt = IsEqualGUID(c, kDiagnosticPerfGuid) &&
        (code == 50 || code == 66 || code == 67 || code == 68 || code == 69);
    const bool driver = IsEqualGUID(c, kDiagnosticDiskGuid) &&
        (code == 34 || code == 35 || code == 37 || code == 52 || code == 53);
    const bool heap = IsEqualGUID(c, kHeapGuid) && (code == 33 || code == 34 || code == 35 || code == 36);
    const bool pool = IsEqualGUID(c, kPoolGuid) && code >= 32 && code <= 35;
    if (!interrupt && !driver && !heap && !pool) return false;
    if ((interrupt && version != 2 && version != 3) || (driver && version > 2) ||
        (heap && version > 2) || (pool && version != 2)) {
        ++trace.quality.diagnostic_unsupported;
        return true;
    }
    const auto malformed = [&]() { ++trace.quality.decode_errors; return true; };
    if (!r.UserData || (width != 4 && width != 8)) return malformed();
    const uint64_t time = EtwTimestampNs(r, trace.qpc_frequency);
    if (interrupt) {
        const bool isr = code == 50 || code == 67;
        if (r.UserDataLength < 8 + width + (isr ? 4u : 0u)) return malformed();
        EVENT_RECORD start = r;
        const uint64_t initial = Read<uint64_t>(r, 0);
        if (r.EventHeader.TimeStamp.QuadPart < 0 || initial > static_cast<uint64_t>(r.EventHeader.TimeStamp.QuadPart)) return malformed();
        start.EventHeader.TimeStamp.QuadPart = static_cast<LONGLONG>(initial);
        InterruptRecord event;
        event.start_ns = EtwTimestampNs(start, trace.qpc_frequency);
        event.end_ns = time;
        event.routine = Pointer(r, 8, width);
        event.cpu = EtwProcessorIndex(r);
        event.kind = isr ? InterruptKind::Isr : code == 66 ? InterruptKind::ThreadedDpc :
                     code == 69 ? InterruptKind::TimerDpc : InterruptKind::Dpc;
        trace.interrupts.push_back(event);
        return true;
    }
    if (driver) {
        const size_t minimum = code == 34 ? 12 + 3 * width :
                               (code == 37 || code == 52) ? 4 + 2 * width : 4 + width;
        if (r.UserDataLength < minimum) return malformed();
        DriverRecord event;
        event.timestamp_ns = time;
        event.cpu = EtwProcessorIndex(r);
        event.pid = r.EventHeader.ProcessId;
        event.tid = r.EventHeader.ThreadId;
        if (code == 34) {
            event.operation = DriverOperation::DispatchEnter;
            event.major = Read<uint32_t>(r, 0); event.minor = Read<uint32_t>(r, 4);
            event.routine = Pointer(r, 8, width); event.file_key = Pointer(r, 8 + width, width);
            event.irp = Pointer(r, 8 + 2 * width, width); event.match_id = Read<uint32_t>(r, 8 + 3 * width);
        } else if (code == 37 || code == 52) {
            event.operation = code == 37 ? DriverOperation::CompletionRoutine : DriverOperation::CompleteEnter;
            event.routine = Pointer(r, 0, width); event.irp = Pointer(r, width, width);
            event.match_id = Read<uint32_t>(r, 2 * width);
        } else {
            event.operation = code == 35 ? DriverOperation::DispatchReturn : DriverOperation::CompleteReturn;
            event.irp = Pointer(r, 0, width); event.match_id = Read<uint32_t>(r, width);
        }
        if (!event.irp) return malformed();
        trace.driver_events.push_back(event);
        return true;
    }
    AllocationRecord event;
    event.timestamp_ns = time; event.cpu = EtwProcessorIndex(r);
    event.pid = r.EventHeader.ProcessId; event.tid = r.EventHeader.ThreadId;
    if (heap && (event.pid == 0 || event.pid == UINT32_MAX) && trace.heap_target_pid) event.pid = trace.heap_target_pid;
    if (pool) {
        const bool session = code == 33 || code == 35;
        if (r.UserDataLength < 8 + 2 * width + (session ? 4u : 0u)) return malformed();
        event.domain = MemoryDomain::Pool;
        event.operation = code <= 33 ? MemoryOperation::Allocate : MemoryOperation::Free;
        event.pool_type = Read<uint32_t>(r, 0); event.tag = Read<uint32_t>(r, 4);
        if (trace.pool_tag_filter && event.tag != trace.pool_tag_filter) return true;
        event.size = Pointer(r, 8, width); event.address = Pointer(r, 8 + width, width);
        if (session) event.session_id = Read<uint32_t>(r, 8 + 2 * width);
        // Pool belongs to the system, not to the thread executing an allocation.
        event.pid = UINT32_MAX;
    } else {
        const size_t minimum = code == 33 ? 3 * width + 4 : code == 34 ? 5 * width + 4 :
                               code == 36 ? 2 * width + 4 : width;
        if (r.UserDataLength < minimum) return malformed();
        event.heap = Pointer(r, 0, width);
        if (code == 33) {
            event.operation = MemoryOperation::Allocate;
            event.size = Pointer(r, width, width); event.address = Pointer(r, 2 * width, width);
        } else if (code == 34) {
            event.operation = MemoryOperation::Reallocate;
            event.address = Pointer(r, width, width); event.old_address = Pointer(r, 2 * width, width);
            event.size = Pointer(r, 3 * width, width); event.old_size = Pointer(r, 4 * width, width);
        } else if (code == 36) {
            event.operation = MemoryOperation::Free; event.address = Pointer(r, width, width);
        } else event.operation = MemoryOperation::DestroyHeap;
    }
    if (event.operation != MemoryOperation::DestroyHeap && !event.address) return malformed();
    if (event.operation == MemoryOperation::Allocate || event.operation == MemoryOperation::Reallocate) ExtendedStack(r, event);
    trace.allocations.push_back(std::move(event));
    return true;
}
}  // namespace wperf
