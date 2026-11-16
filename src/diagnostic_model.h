#pragma once

#include <cstdint>
#include <vector>
#include "shared_stack.h"

namespace wperf {

enum class InterruptKind { Dpc, TimerDpc, ThreadedDpc, Isr };
struct SchedulingContextRecord {
    uint64_t timestamp_ns = 0;
    uint32_t cpu = 0, tid = 0;
};
struct InterruptRecord {
    uint64_t start_ns = 0, end_ns = 0, routine = 0;
    uint32_t cpu = 0;
    InterruptKind kind = InterruptKind::Dpc;
};

enum class MemoryDomain { Heap, Pool };
enum class MemoryOperation { Allocate, Free, Reallocate, DestroyHeap };
struct AllocationRecord {
    uint64_t timestamp_ns = 0;
    uint32_t pid = UINT32_MAX, tid = UINT32_MAX, cpu = 0;
    MemoryDomain domain = MemoryDomain::Heap;
    MemoryOperation operation = MemoryOperation::Allocate;
    uint64_t heap = 0, address = 0, size = 0, old_address = 0, old_size = 0;
    uint32_t tag = 0, pool_type = 0, session_id = UINT32_MAX;
    SharedStack stack;
    StackQuality stack_quality = StackQuality::Missing;
};

enum class DriverOperation { DispatchEnter, DispatchReturn, CompleteEnter, CompleteReturn, CompletionRoutine };
struct DriverRecord {
    uint64_t timestamp_ns = 0, irp = 0, routine = 0, file_key = 0;
    uint32_t match_id = 0, major = 0, minor = 0, cpu = 0;
    uint32_t pid = UINT32_MAX, tid = UINT32_MAX;
    DriverOperation operation = DriverOperation::DispatchEnter;
};

}  // namespace wperf
