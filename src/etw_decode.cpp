#include "etw_decode.h"
#include "common.h"
#include <sstream>

#include <cstring>

namespace wperf {
namespace {
template<typename T>
T Read(const void* data, size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const BYTE*>(data) + offset, sizeof(value));
    return value;
}
uint64_t QpcToNs(uint64_t value, uint64_t frequency) {
    if (frequency == 0) frequency = 10000000;
    return (value / frequency) * 1000000000ull +
           ((value % frequency) * 1000000000ull) / frequency;
}
}

uint32_t EtwProcessorIndex(const EVENT_RECORD& record) {
    return (record.EventHeader.Flags & EVENT_HEADER_FLAG_PROCESSOR_INDEX) != 0
               ? record.BufferContext.ProcessorIndex
               : record.BufferContext.ProcessorNumber;
}

void CopyEtwPmcCounters(const EVENT_RECORD& record, std::vector<uint64_t>& counters,
                        TraceQuality& quality) {
    for (USHORT index = 0; index < record.ExtendedDataCount; ++index) {
        const auto& item = record.ExtendedData[index];
        if (item.ExtType != EVENT_HEADER_EXT_TYPE_PMC_COUNTERS) continue;
        if (item.DataPtr == 0 || item.DataSize == 0 || item.DataSize>512 || item.DataSize % 8 != 0) {
            ++quality.decode_errors;
            return;
        }
        counters.resize(item.DataSize / 8);
        std::memcpy(counters.data(), reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(item.DataPtr)), item.DataSize);
        ++quality.pmc_counter_events;
        return;
    }
}

bool DecodeProfileSample(const EVENT_RECORD& record, const TraceData& metadata,
                         SampleRecord& sample) {
    const uint32_t p = (record.EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER)
                           ? 4 : (record.EventHeader.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER)
                                     ? 8 : metadata.pointer_size;
    const auto code = record.EventHeader.EventDescriptor.Opcode != 0
                          ? record.EventHeader.EventDescriptor.Opcode
                          : record.EventHeader.EventDescriptor.Id;
    if ((p != 4 && p != 8) || (code != 46 && code != 47) ||
        record.UserData == nullptr || record.UserDataLength < p + 8) return false;
    sample = {};
    sample.timestamp_ns = QpcToNs(static_cast<uint64_t>(
        record.EventHeader.TimeStamp.QuadPart), metadata.qpc_frequency);
    sample.cpu = EtwProcessorIndex(record);
    sample.pid = record.EventHeader.ProcessId;
    sample.tid = Read<uint32_t>(record.UserData, p);
    sample.instruction_pointer = p == 8 ? Read<uint64_t>(record.UserData, 0)
                                       : Read<uint32_t>(record.UserData, 0);
    if (code == 47) {
        sample.kind = SampleKind::Pmc;
        sample.profile_source = Read<uint16_t>(record.UserData, p + 4);
        const auto found = metadata.profile_intervals.find(sample.profile_source);
        if (found != metadata.profile_intervals.end()) sample.period = found->second;
    } else {
        sample.period = metadata.profile_interval_ns;
    }
    return true;
}

bool DecodeContextRegisters(const EVENT_RECORD& record, uint32_t pointer_size,
                            ContextRegisterRecord& registers) {
    const auto code = record.EventHeader.EventDescriptor.Opcode != 0
                          ? record.EventHeader.EventDescriptor.Opcode
                          : record.EventHeader.EventDescriptor.Id;
    constexpr size_t kAmd64PayloadBytes = 18 * sizeof(uint64_t);
    if (code != 122 || pointer_size != 8 || record.UserData == nullptr ||
        record.UserDataLength < kAmd64PayloadBytes) {
        return false;
    }
    registers = {};
    registers.timestamp_qpc = Read<uint64_t>(record.UserData, 0);
    registers.rip = Read<uint64_t>(record.UserData, 8);
    registers.rax = Read<uint64_t>(record.UserData, 16);
    registers.rcx = Read<uint64_t>(record.UserData, 24);
    registers.rdx = Read<uint64_t>(record.UserData, 32);
    registers.rbx = Read<uint64_t>(record.UserData, 40);
    registers.rsp = Read<uint64_t>(record.UserData, 48);
    registers.rbp = Read<uint64_t>(record.UserData, 56);
    registers.rsi = Read<uint64_t>(record.UserData, 64);
    registers.rdi = Read<uint64_t>(record.UserData, 72);
    registers.r8 = Read<uint64_t>(record.UserData, 80);
    registers.r9 = Read<uint64_t>(record.UserData, 88);
    registers.r10 = Read<uint64_t>(record.UserData, 96);
    registers.r11 = Read<uint64_t>(record.UserData, 104);
    registers.r12 = Read<uint64_t>(record.UserData, 112);
    registers.r13 = Read<uint64_t>(record.UserData, 120);
    registers.r14 = Read<uint64_t>(record.UserData, 128);
    registers.r15 = Read<uint64_t>(record.UserData, 136);
    return true;
}
bool DecodePmcCounterMetadata(const EVENT_RECORD& record, TraceData& metadata) {
    const auto code = record.EventHeader.EventDescriptor.Opcode != 0
                          ? record.EventHeader.EventDescriptor.Opcode
                          : record.EventHeader.EventDescriptor.Id;
    if (code != 49 || record.UserData == nullptr || record.UserDataLength < 8) {
        return false;
    }
    const uint32_t count = Read<uint32_t>(record.UserData, 4);
    if (count == 0 || count > 32 || record.UserDataLength < 8 + count * 12) {
        return false;
    }
    std::vector<uint32_t> sources;
    sources.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t source = Read<uint32_t>(record.UserData, 8 + index * 12);
        if (source == 0) return false;
        sources.push_back(source);
    }
    if (!metadata.pmc_counter_sources.empty() &&
        metadata.pmc_counter_sources != sources) {
        return false;
    }
    metadata.pmc_counter_sources = std::move(sources);
    return true;
}
bool DecodeProfileInterval(const EVENT_RECORD& record, TraceData& metadata) {
    const auto code = record.EventHeader.EventDescriptor.Opcode != 0
                          ? record.EventHeader.EventDescriptor.Opcode
                          : record.EventHeader.EventDescriptor.Id;
    if ((code != 72 && code != 73 && code != 74) ||
        record.UserData == nullptr || record.UserDataLength < 12) return false;
    if (code == 74) return true;  // CollectionEnd is not a new sampling period.
    const uint32_t source = Read<uint32_t>(record.UserData, 0);
    const uint32_t interval = Read<uint32_t>(record.UserData, 4);
    if (interval == 0) return false;
    metadata.profile_intervals[source] = interval;
    if (source == 0) metadata.profile_interval_ns = uint64_t{interval} * 100;
    return true;
}

uint64_t EtwTimestampNs(const EVENT_RECORD& record,uint64_t frequency) {
    return QpcToNs(static_cast<uint64_t>(record.EventHeader.TimeStamp.QuadPart),frequency);
}
uint32_t EtwPointerSize(const EVENT_RECORD& record,uint32_t fallback) {
    return record.EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER ? 4 :
           record.EventHeader.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER ? 8 : fallback;
}
namespace {
uint64_t Pointer(const EVENT_RECORD& record,size_t offset,uint32_t p) {
    return p==8 ? Read<uint64_t>(record.UserData,offset) : Read<uint32_t>(record.UserData,offset);
}
std::wstring Wide(const EVENT_RECORD& record,size_t offset) {
    std::wstring value;
    for(size_t i=offset;i+2<=record.UserDataLength;i+=2) {
        wchar_t ch=Read<wchar_t>(record.UserData,i);
        if(ch==0)break;
        value+=ch;
    }
    return value;
}
}
bool DecodeImagePayload(const EVENT_RECORD& record,uint32_t p,uint64_t frequency,ImageRecord& image) {
    p=EtwPointerSize(record,p);
    if((p!=4&&p!=8)||!record.UserData) return false;
    const auto version=record.EventHeader.EventDescriptor.Version;
    const size_t offset=version==0 ? p+4 : version==1 ? 2*p+4 : 3*p+32;
    if(record.UserDataLength<offset) return false;
    image={}; image.load_ns=EtwTimestampNs(record,frequency);
    image.base=Pointer(record,0,p);
    image.size=version==0 ? Read<uint32_t>(record.UserData,p) : Pointer(record,p,p);
    image.pid=version==0 ? record.EventHeader.ProcessId : Read<uint32_t>(record.UserData,2*p);
    // Modern payload PID 0 denotes a global image (e.g. kernel rundown).
    // The header PID is the emitting context, not the image owner.
    if(image.pid==UINT32_MAX) image.pid=record.EventHeader.ProcessId;
    if(version>=2) { image.checksum=Read<uint32_t>(record.UserData,2*p+4); image.timestamp=Read<uint32_t>(record.UserData,2*p+8); }
    image.path=Wide(record,offset);
    return true;
}
bool DecodeThreadPayload(const EVENT_RECORD& record,uint64_t frequency,ThreadRecord& thread) {
    if(!record.UserData||record.UserDataLength<8)return false;
    thread={Read<uint32_t>(record.UserData,0),Read<uint32_t>(record.UserData,4),EtwTimestampNs(record,frequency),0};
    return true;
}
bool DecodeProcessPayload(const EVENT_RECORD& record,uint32_t p,uint64_t frequency,ProcessRecord& process) {
    p=EtwPointerSize(record,p);
    const auto version=record.EventHeader.EventDescriptor.Version;
    if(!record.UserData||(p!=4&&p!=8)||record.UserDataLength<p+8)return false;
    process={}; process.start_ns=EtwTimestampNs(record,frequency);
    if(version==0) {
        process.pid=static_cast<uint32_t>(Pointer(record,0,p));
        process.parent_pid=static_cast<uint32_t>(Pointer(record,p,p));
        return true;
    }
    process.pid=Read<uint32_t>(record.UserData,p); process.parent_pid=Read<uint32_t>(record.UserData,p+4);
    const size_t sid=version>=4 ? 28+2*(p-4) : version>=3 ? 24+2*(p-4) : 20+(p-4);
    if(record.UserDataLength<sid+4)return true; // Stop records may omit strings.
    size_t name=sid+4;
    if(Read<uint32_t>(record.UserData,sid)!=0) {
        const size_t token=2*p;
        if(record.UserDataLength<sid+token+8)return false;
        const size_t authorities=Read<uint8_t>(record.UserData,sid+token+1);
        name=sid+token+8+4*authorities;
        if(name>record.UserDataLength)return false;
    }
    size_t end=name;
    while(end<record.UserDataLength && Read<uint8_t>(record.UserData,end)!=0)++end;
    process.image_name=ToUtf16(std::string_view(static_cast<const char*>(record.UserData)+name,end-name));
    if(version>=2&&end<record.UserDataLength)process.command_line=Wide(record,end+1);
    return true;
}

bool DecodeKernelEvent(const EVENT_RECORD& record,uint32_t p,uint64_t frequency,TraceEventKind kind,TraceEvent& event) {
    p=EtwPointerSize(record,p);
    if((p!=4&&p!=8)||!record.UserData)return false;
    const size_t size=record.UserDataLength;
    const auto code=record.EventHeader.EventDescriptor.Opcode;
    const auto version=record.EventHeader.EventDescriptor.Version;
    event={}; event.timestamp_ns=EtwTimestampNs(record,frequency); event.cpu=EtwProcessorIndex(record);
    event.pid=record.EventHeader.ProcessId; event.tid=record.EventHeader.ThreadId;
    event.kind=kind; event.event_id=event.operation_id=code;
    switch(kind) {
    case TraceEventKind::ContextSwitch:
        if(size<16)return false;
        event.tid=Read<uint32_t>(record.UserData,0); event.related_tid=Read<uint32_t>(record.UserData,4);
        event.value1=event.related_tid; event.value2=event.tid;
        event.pid=UINT32_MAX; // Header execution context need not be the scheduled-in thread.
        event.wait_reason=Read<uint8_t>(record.UserData,12); event.previous_state=Read<uint8_t>(record.UserData,14);
        break;
    case TraceEventKind::ReadyThread:
        if(size<8)return false;
        event.related_tid=Read<uint32_t>(record.UserData,0); event.value1=event.related_tid;
        event.flags=Read<uint8_t>(record.UserData,6); event.value2=event.flags;
        break;
    case TraceEventKind::SyscallEnter:
        if(size<p)return false;
        event.value1=Pointer(record,0,p); break;
    case TraceEventKind::SyscallExit:
        if(size<4)return false;
        event.status=static_cast<int32_t>(Read<uint32_t>(record.UserData,0));
        event.value2=static_cast<uint32_t>(event.status); event.has_status=true; break;
    case TraceEventKind::PageFault:
        if(size<2*p)return false;
        event.value1=Pointer(record,0,p); event.value2=Pointer(record,p,p); break;
    case TraceEventKind::DiskIo: {
        if(code==12||code==13||code==15) {
            if(size<p)return false;
            event.correlation_id=Pointer(record,0,p);
            if(version>=3 && size>=p+4)event.tid=Read<uint32_t>(record.UserData,p);
        } else if(code==14) {
            if(size<16+p)return false;
            event.correlation_id=Pointer(record,16,p); event.is_completion=true;
            const auto ticks=Read<uint64_t>(record.UserData,8);
            event.duration_ns=QpcToNs(ticks,frequency); event.has_duration=ticks!=0;
        } else if(code==10||code==11) {
            const size_t irp=p==8?32:28, duration=p==8?40:32;
            if(size<duration+8)return false;
            event.size=Read<uint32_t>(record.UserData,8); event.offset=Read<uint64_t>(record.UserData,16);
            event.file_key=Pointer(record,24,p); event.correlation_id=Pointer(record,irp,p);
            event.is_completion=true;
            const auto ticks=Read<uint64_t>(record.UserData,duration);
            event.duration_ns=QpcToNs(ticks,frequency); event.has_duration=ticks!=0;
            if(size>=duration+12) { event.tid=Read<uint32_t>(record.UserData,duration+8); event.pid=UINT32_MAX; }
        }
        break;
    }
    case TraceEventKind::FileIo:
        if(code==0||code==32||code==35||code==36) {
            if(size<p+2)return false;
            event.file_key=Pointer(record,0,p); event.file_path=Wide(record,p);
        } else if(code==67||code==68) {
            const size_t file_object=version<=2?8+2*p:8+p;
            const size_t file_key=file_object+p;
            const size_t bytes=version<=2?(p==8?40:24):(p==8?36:24);
            if(size<bytes+4)return false;
            event.offset=Read<uint64_t>(record.UserData,0); event.correlation_id=Pointer(record,8,p);
            event.file_object=Pointer(record,file_object,p); event.file_key=Pointer(record,file_key,p);
            event.size=Read<uint32_t>(record.UserData,bytes);
            const size_t tid_offset=version<=2?8+p:8+3*p;
            if(size>=tid_offset+4)event.tid=Read<uint32_t>(record.UserData,tid_offset);
        } else if(code==76) {
            const size_t status=p==8?16:8;
            if(size<status+4)return false;
            event.correlation_id=Pointer(record,0,p); event.is_completion=true;
            event.status=static_cast<int32_t>(Read<uint32_t>(record.UserData,status)); event.has_status=true;
            event.value2=static_cast<uint32_t>(event.status);
        } else if(code==64) {
            const size_t file_object=version<=2?2*p:p;
            const size_t name=version<=2?24+3*(p-4):24+2*(p-4);
            if(size<name+2)return false;
            event.correlation_id=Pointer(record,0,p); event.file_object=Pointer(record,file_object,p);
            event.file_path=Wide(record,name);
            event.tid=Read<uint32_t>(record.UserData,version<=2?p:2*p);
        } else if(code>=65&&code<=77) {
            const size_t file_object=version<=2?2*p:p, file_key=version<=2?3*p:2*p;
            if(size<file_key+p)return false;
            event.correlation_id=Pointer(record,0,p); event.file_object=Pointer(record,file_object,p);
            event.file_key=Pointer(record,file_key,p);
        }
        break;
    default:return false;
    }
    return true;
}

bool DecodeWperfMetadata(const EVENT_RECORD& record,TraceData& metadata) {
    if(!record.UserData||record.UserDataLength>8192)return false;
    const std::string text(static_cast<const char*>(record.UserData),record.UserDataLength);
    std::istringstream file(text); std::string header;
    if(!std::getline(file,header)||header!="wperf-meta-v1")return false;
    std::vector<uint32_t> sources; std::unordered_map<uint32_t,std::wstring> names;
    std::string kind;
    while(file>>kind) {
        if(kind=="session") {
            std::string session_text; std::wstring name;
            if(!(file>>session_text)||!UnhexText(session_text,name))return false;
            if(!metadata.session_name.empty()&&metadata.session_name!=name)return true;
            if(metadata.session_name.empty())metadata.session_name=name;
            continue;
        }
        uint32_t source=0; if(!(file>>source))return false;
        if(kind=="counter") {
            std::string name; std::wstring decoded;
            if(!(file>>name)||sources.size()>=32||!UnhexText(name,decoded))return false;
            if(names.contains(source))return false;
            sources.push_back(source); names[source]=std::move(decoded);
        } else if(kind=="period") {
            uint64_t period=0; if(!(file>>period)||period==0)return false;
            metadata.profile_intervals[source]=period;
            if(source==0)metadata.profile_interval_ns=period*100;
        } else return false;
    }
    metadata.pmc_counter_sources=std::move(sources); metadata.pmc_source_names=std::move(names);
    return true;
}

}  // namespace wperf
