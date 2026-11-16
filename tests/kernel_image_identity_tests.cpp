#include "common.h"
#include "cpu_report.h"
#include "etw_decode.h"
#include "etl_reader.h"
#include "live_top.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <iostream>
#include <sstream>

namespace wperf {
struct LiveCaptureTestAccess {
    static void Feed(LiveTopCapture& capture, EVENT_RECORD& event) { capture.OnEvent(&event); }
};
}

namespace {
int failures = 0;
void Check(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
const GUID image_guid = {0x2cb15d1d,0x5fc1,0x11d2,{0xab,0xe1,0x00,0xa0,0xc9,0x11,0xf5,0x18}};
const GUID perf_guid = {0xce1dbfb4,0x137e,0x4da6,{0x87,0xb0,0x3f,0x59,0xaa,0x10,0x2c,0xbc}};
const GUID process_guid = {0x3d6fa8d0,0xfe05,0x11d0,{0x9d,0xda,0x00,0xc0,0x4f,0xd7,0xba,0x7c}};
constexpr uint64_t kernel_base = 0xfffff80000100000ull;
struct OwnedRecord {
    EVENT_RECORD event{};
    std::vector<BYTE> payload;
    void Bind() { event.UserData = payload.data(); event.UserDataLength = static_cast<USHORT>(payload.size()); }
};
template<typename T> void Put(std::vector<BYTE>& data, size_t offset, T value) {
    std::memcpy(data.data()+offset, &value, sizeof(value));
}
void PutPointer(std::vector<BYTE>& data, size_t offset, uint32_t width, uint64_t value) {
    if (width == 4) Put(data, offset, static_cast<uint32_t>(value));
    else Put(data, offset, value);
}
OwnedRecord Image(uint32_t width, BYTE version, BYTE opcode, uint32_t header_pid,
                  uint32_t payload_pid, uint64_t base, uint64_t time) {
    OwnedRecord result;
    result.event.EventHeader.ProviderId = image_guid;
    result.event.EventHeader.Flags = width == 4 ? EVENT_HEADER_FLAG_32_BIT_HEADER : EVENT_HEADER_FLAG_64_BIT_HEADER;
    result.event.EventHeader.ProcessId = header_pid;
    result.event.EventHeader.EventDescriptor.Version = version;
    result.event.EventHeader.EventDescriptor.Opcode = opcode;
    result.event.EventHeader.TimeStamp.QuadPart = time;
    const size_t name_offset = version == 0 ? width+4 : version == 1 ? 2*width+4 : 3*width+32;
    const std::wstring name = L"global-kernel.sys";
    result.payload.resize(name_offset+(name.size()+1)*sizeof(wchar_t));
    PutPointer(result.payload, 0, width, base);
    if (version == 0) Put(result.payload, width, uint32_t{0x10000});
    else {
        PutPointer(result.payload, width, width, 0x10000);
        Put(result.payload, 2*width, payload_pid);
    }
    if (version >= 2) { Put(result.payload, 2*width+4, uint32_t{123}); Put(result.payload, 2*width+8, uint32_t{456}); }
    std::memcpy(result.payload.data()+name_offset, name.c_str(), (name.size()+1)*sizeof(wchar_t));
    result.Bind(); return result;
}
OwnedRecord Sample(uint64_t address, uint64_t time) {
    OwnedRecord result; result.payload.resize(16);
    result.event.EventHeader.ProviderId=perf_guid;
    result.event.EventHeader.Flags=EVENT_HEADER_FLAG_64_BIT_HEADER;
    result.event.EventHeader.ProcessId=42; result.event.EventHeader.ThreadId=50;
    result.event.EventHeader.EventDescriptor.Opcode=46;
    result.event.EventHeader.TimeStamp.QuadPart=time;
    Put(result.payload,0,address); Put(result.payload,8,uint32_t{50}); result.Bind(); return result;
}
OwnedRecord CollectorExit(uint64_t time) {
    OwnedRecord result; result.payload.resize(16);
    result.event.EventHeader.ProviderId=process_guid;
    result.event.EventHeader.Flags=EVENT_HEADER_FLAG_64_BIT_HEADER;
    result.event.EventHeader.EventDescriptor.Opcode=2;
    result.event.EventHeader.TimeStamp.QuadPart=time;
    Put(result.payload,0,uint64_t{887}); result.Bind(); return result;
}
void TestPayloadIdentity() {
    for (const uint32_t width : {4u,8u}) for (const BYTE version : {BYTE{1},BYTE{2},BYTE{3}}) {
        const uint64_t base=width == 4 ? 0x80400000ull : kernel_base;
        for (const BYTE opcode : {BYTE{2},BYTE{3},BYTE{4},BYTE{10}}) {
            auto raw=Image(width,version,opcode,887,0,base,10);
            wperf::ImageRecord decoded;
            Check(wperf::DecodeImagePayload(raw.event,width,10000000,decoded),"decode valid image payload");
            Check(decoded.pid==0,"payload PID zero is global, not the collector header PID");
            Check(decoded.base==base && decoded.size==0x10000 && decoded.path==L"global-kernel.sys","global ownership does not change range/path");
        }
        auto user=Image(width,version,10,887,42,width == 4 ? 0x90000000ull : 0x180000000ull,10);
        wperf::ImageRecord decoded;
        Check(wperf::DecodeImagePayload(user.event,width,10000000,decoded) && decoded.pid==42,"explicit user PID is authoritative, including large 32-bit addresses");
        auto unknown=Image(width,version,10,887,UINT32_MAX,0x1000,10);
        Check(wperf::DecodeImagePayload(unknown.event,width,10000000,decoded) && decoded.pid==887,"unavailable PID retains documented header fallback");
        auto legacy=Image(width,0,10,887,0,0x1000,10);
        Check(wperf::DecodeImagePayload(legacy.event,width,10000000,decoded) && decoded.pid==887,"version zero has only header identity");
        const size_t minimum=version == 1 ? 2*width+4 : 3*width+32;
        for (size_t length=0;length<minimum;++length) {
            user.event.UserDataLength=static_cast<USHORT>(length);
            Check(!wperf::DecodeImagePayload(user.event,width,10000000,decoded),"truncated image payload is rejected");
        }
    }
}
void CheckLookupAndReport(wperf::TraceData& trace) {
    wperf::ImageIndex index; index.Build(trace);
    Check(index.Find(42,kernel_base+0x10,2000)!=nullptr,"indexed lookup shares global kernel image with target");
    Check(wperf::FindImage(trace,43,kernel_base+0x10,2000)!=nullptr,"linear lookup shares kernel image with unrelated PID");
    Check(index.Find(43,0x1810,2000)==nullptr,"user images are not borrowed across PIDs");
    wperf::Symbolizer symbols;
    wperf::CpuReportOptions options; options.pid=42; options.json=true; options.quiet=true;
    std::wostringstream output; wperf::WriteCpuReport(trace,symbols,options,output);
    const auto rows=nlohmann::json::parse(wperf::ToUtf8(output.str()));
    Check(rows.size()==1 && rows[0]["dso"]=="global-kernel.sys" && rows[0]["samples"]==1,"CPU report identifies target kernel sample without requiring a PDB");
}
void TestOfflineReplayAndUnload() {
    auto kernel=Image(8,2,3,887,0,kernel_base,10);
    auto user=Image(8,2,10,887,42,0x1800,10);
    auto sample=Sample(kernel_base+0x10,20);
    std::vector<EVENT_RECORD> records{kernel.event,user.event,sample.event};
    wperf::TraceData trace;
    Check(wperf::ReadEtwRecords(records,trace),"raw offline replay succeeds");
    CheckLookupAndReport(trace);
    auto unload=Image(8,2,2,899,0,kernel_base,30);
    records.push_back(unload.event);
    Check(wperf::ReadEtwRecords(records,trace),"raw unload replay succeeds");
    wperf::ImageIndex index; index.Build(trace);
    Check(index.Find(42,kernel_base+0x10,2000)!=nullptr && index.Find(42,kernel_base+0x10,4000)==nullptr,
          "unload in another execution context closes the same global image lifetime");
}
void TestLiveSnapshotsAndCollectorExit() {
    wperf::LiveTopCapture capture;
    auto kernel=Image(8,2,3,887,0,kernel_base,10);
    auto user=Image(8,2,10,887,42,0x1800,10);
    auto sample=Sample(kernel_base+0x10,20);
    wperf::LiveCaptureTestAccess::Feed(capture,kernel.event);
    wperf::LiveCaptureTestAccess::Feed(capture,user.event);
    wperf::LiveCaptureTestAccess::Feed(capture,sample.event);
    auto initial=capture.Snapshot(); CheckLookupAndReport(initial);
    auto exit=CollectorExit(25); wperf::LiveCaptureTestAccess::Feed(capture,exit.event);
    auto retained=capture.Snapshot(); wperf::ImageIndex index; index.Build(retained);
    Check(index.Find(42,kernel_base+0x10,2600)!=nullptr,"collector exit must not unload global kernel image");
    auto unload=Image(8,2,2,899,0,kernel_base,30);
    wperf::LiveCaptureTestAccess::Feed(capture,unload.event);
    auto ended=capture.Snapshot(); index.Build(ended);
    Check(index.Find(42,kernel_base+0x10,4000)==nullptr,"live global unload is independent of header PID");
}
}
int main() {
    TestPayloadIdentity(); TestOfflineReplayAndUnload(); TestLiveSnapshotsAndCollectorExit();
    if (!failures) std::cout << "kernel image identity: PASS\n";
    return failures ? 1 : 0;
}
