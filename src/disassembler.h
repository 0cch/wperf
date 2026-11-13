#pragma once

#include "trace_model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wperf {

struct DisassembledInstruction {
    uint64_t address = 0;
    uint16_t length = 0;
    std::string bytes;
    std::string text;
};

bool DisassemblyAvailable();
bool DisassembleBuffer(const uint8_t* data, size_t size, uint64_t address,
                       uint32_t pointer_size, size_t max_instructions,
                       std::vector<DisassembledInstruction>& instructions,
                       std::wstring* error = nullptr);
bool DisassembleImageAddress(const ImageRecord& image, uint64_t address,
                             uint32_t pointer_size,
                             size_t max_instructions,
                             std::vector<DisassembledInstruction>& instructions,
                             std::wstring* error = nullptr);
bool DisassembleImageRange(const ImageRecord& image, uint64_t address, size_t bytes,
                            uint32_t pointer_size, size_t max_instructions,
                            std::vector<DisassembledInstruction>& instructions,
                            std::wstring* error = nullptr);

}  // namespace wperf
