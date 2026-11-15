#include "etw_decode.h"
#include "lbr.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    uint32_t seed = 0x5eed1234u;
    const auto next = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return seed;
    };
    uint64_t decoded = 0;
    for (size_t iteration = 0; iteration < 10000; ++iteration) {
        const USHORT length = static_cast<USHORT>(next() % 513);
        std::vector<BYTE> payload(length);
        for (BYTE& value : payload) value = static_cast<BYTE>(next());
        EVENT_RECORD record{};
        record.EventHeader.ProviderId = {0x11111111,0x2222,0x3333,{0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb}};
        record.EventHeader.EventDescriptor.Opcode = static_cast<UINT8>(next() % 128);
        record.EventHeader.EventDescriptor.Version = static_cast<UINT8>(next() % 5);
        record.EventHeader.TimeStamp.QuadPart = next();
        record.UserData = payload.empty() ? nullptr : payload.data();
        record.UserDataLength = length;
        const uint32_t pointer_size = (iteration & 1u) ? 8u : 4u;
        wperf::TraceData metadata;
        metadata.pointer_size = pointer_size;
        wperf::SampleRecord sample;
        if (wperf::DecodeProfileSample(record, metadata, sample)) ++decoded;
        wperf::TraceEvent event;
        if (wperf::DecodeKernelEvent(record, pointer_size, 10000000,
                                     static_cast<wperf::TraceEventKind>(iteration % 9), event)) ++decoded;
        wperf::LbrSample lbr;
        if (wperf::DecodeLbrPayload(payload.data(), payload.size(), pointer_size, lbr)) ++decoded;
        wperf::ContextRegisterRecord registers;
        if (wperf::DecodeContextRegisters(record, pointer_size, registers)) ++decoded;
    }
    if (decoded == 0) {
        std::cerr << "decoder fuzz smoke decoded no records\n";
        return 1;
    }
    std::cout << "decoder fuzz smoke: PASS\n";
    return 0;
}
