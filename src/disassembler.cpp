#include "disassembler.h"

#include "common.h"
#include "symbolizer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#if defined(WPERF_HAVE_ZYDIS)
#include <Zydis/Zydis.h>
#endif

namespace wperf {
namespace {

struct Section {
    uint32_t virtual_address = 0;
    uint32_t virtual_size = 0;
    uint32_t raw_size = 0;
    uint32_t raw_offset = 0;
};


bool ReadExact(std::ifstream& stream, uint64_t offset, void* data, size_t size) {
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream) {
        return false;
    }
    stream.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return stream.gcount() == static_cast<std::streamsize>(size);
}

bool ReadImageBytes(const ImageRecord& image, uint64_t address, size_t size,
                    std::vector<uint8_t>& bytes, std::wstring* error) {
    bytes.clear();
    if (address < image.base) {
        if (error != nullptr) {
            *error = L"address is below module base";
        }
        return false;
    }
    const std::wstring path = ModuleFilePath(image);
    if (!ModuleIdentityMatches(image, ReadModuleIdentity(path))) {
        if (error) *error = L"module identity does not match the trace";
        return false;
    }
    const uint64_t rva = address - image.base;
    std::ifstream file(std::filesystem::path(path),
                       std::ios::binary);
    if (!file) {
        if (error != nullptr) {
            *error = L"cannot open module file";
        }
        return false;
    }
    IMAGE_DOS_HEADER dos{};
    if (!ReadExact(file, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
        if (error != nullptr) {
            *error = L"invalid DOS header";
        }
        return false;
    }
    DWORD signature = 0;
    IMAGE_FILE_HEADER header{};
    if (!ReadExact(file, dos.e_lfanew, &signature, sizeof(signature)) ||
        signature != IMAGE_NT_SIGNATURE ||
        !ReadExact(file, dos.e_lfanew + sizeof(signature), &header, sizeof(header))) {
        if (error != nullptr) {
            *error = L"invalid NT header";
        }
        return false;
    }
    std::vector<BYTE> optional(header.SizeOfOptionalHeader);
    if (!ReadExact(file, dos.e_lfanew + sizeof(signature) + sizeof(header),
                   optional.data(), optional.size())) {
        if (error != nullptr) {
            *error = L"truncated optional header";
        }
        return false;
    }
    if (header.NumberOfSections > 96) return false;
    std::vector<Section> sections;
    const uint64_t section_offset =
        static_cast<uint64_t>(dos.e_lfanew) + sizeof(signature) + sizeof(header) +
        header.SizeOfOptionalHeader;
    for (WORD index = 0; index < header.NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER section{};
        if (!ReadExact(file, section_offset + index * sizeof(section), &section,
                       sizeof(section))) {
            return false;
        }
        sections.push_back(Section{section.VirtualAddress,
                                   section.Misc.VirtualSize, section.SizeOfRawData,
                                   section.PointerToRawData});
    }
    for (const Section& section : sections) {
        const uint32_t span = section.raw_size;
        if (rva >= section.virtual_address &&
            rva - section.virtual_address < span) {
            const uint64_t file_offset =
                section.raw_offset + (rva - section.virtual_address);
            const size_t available = static_cast<size_t>(section.raw_size - (rva - section.virtual_address));
            bytes.resize(std::min(size, available));
            if (!ReadExact(file, file_offset, bytes.data(), bytes.size())) {
                bytes.clear();
                if (error != nullptr) {
                    *error = L"cannot read code bytes";
                }
                return false;
            }
            return true;
        }
    }
    if (error != nullptr) {
        *error = L"address is outside image sections";
    }
    return false;
}

}  // namespace

bool DisassemblyAvailable() {
#if defined(WPERF_HAVE_ZYDIS)
    return true;
#else
    return false;
#endif
}

bool DisassembleBuffer(const uint8_t* data, size_t size, uint64_t address,
                       uint32_t pointer_size, size_t max_instructions,
                       std::vector<DisassembledInstruction>& instructions,
                       std::wstring* error) {
    instructions.clear();
#if defined(WPERF_HAVE_ZYDIS)
    if (data == nullptr || size == 0 || max_instructions == 0) {
        return false;
    }
    ZydisDecoder decoder{};
    const ZyanStatus init_status = ZydisDecoderInit(
        &decoder,
        pointer_size == 4 ? ZYDIS_MACHINE_MODE_LONG_COMPAT_32
                          : ZYDIS_MACHINE_MODE_LONG_64,
        pointer_size == 4 ? ZYDIS_STACK_WIDTH_32 : ZYDIS_STACK_WIDTH_64);
    if (!ZYAN_SUCCESS(init_status)) {
        if (error != nullptr) {
            *error = L"ZydisDecoderInit failed";
        }
        return false;
    }
    ZydisFormatter formatter{};
    if (!ZYAN_SUCCESS(ZydisFormatterInit(&formatter,
                                         ZYDIS_FORMATTER_STYLE_INTEL))) {
        if (error != nullptr) {
            *error = L"ZydisFormatterInit failed";
        }
        return false;
    }
    size_t offset = 0;
    while (offset < size && instructions.size() < max_instructions) {
        ZydisDecodedInstruction instruction{};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(
                &decoder, data + offset, size - offset, &instruction, operands))) {
            break;
        }
        char formatted[256]{};
        const uint64_t instruction_address = address + offset;
        if (!ZYAN_SUCCESS(ZydisFormatterFormatInstruction(
                &formatter, &instruction, operands,
                instruction.operand_count_visible, formatted, sizeof(formatted),
                instruction_address, ZYAN_NULL))) {
            break;
        }
        DisassembledInstruction row;
        row.address = instruction_address;
        row.length = instruction.length;
        row.text = formatted;
        char bytes[3]{};
        for (uint16_t index = 0; index < instruction.length; ++index) {
            if (index != 0) {
                row.bytes += ' ';
            }
            sprintf_s(bytes, "%02X", data[offset + index]);
            row.bytes += bytes;
        }
        instructions.push_back(std::move(row));
        offset += instruction.length;
    }
    return !instructions.empty();
#else
    (void)data;
    (void)size;
    (void)address;
    (void)pointer_size;
    (void)max_instructions;
    if (error != nullptr) {
        *error = L"disassembly support is not enabled";
    }
    return false;
#endif
}

bool DisassembleImageAddress(const ImageRecord& image, uint64_t address,
                             uint32_t pointer_size,
                             size_t max_instructions,
                             std::vector<DisassembledInstruction>& instructions,
                             std::wstring* error) {
    instructions.clear();
    std::vector<uint8_t> bytes;
    if (!ReadImageBytes(image, address, 64, bytes, error)) {
        return false;
    }
    return DisassembleBuffer(bytes.data(), bytes.size(), address, pointer_size,
                             max_instructions, instructions, error);
}

bool DisassembleImageRange(const ImageRecord& image,uint64_t address,size_t size,uint32_t p,size_t limit,
                            std::vector<DisassembledInstruction>& output,std::wstring* error) {
    output.clear();
    if(size==0||size>65536) { if(error)*error=L"function disassembly range exceeds 64 KiB"; return false; }
    std::vector<uint8_t> bytes;
    if(!ReadImageBytes(image,address,size,bytes,error))return false;
    return DisassembleBuffer(bytes.data(),bytes.size(),address,p,limit,output,error);
}

}  // namespace wperf
