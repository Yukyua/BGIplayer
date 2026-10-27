#include "bgiplayer/vm.hpp"
#include <iostream>

namespace bgiplayer {
void VM::trace_bs_packet(uint32_t opcode) {
    const auto segment = resource_segments_.find(active_code_resource_base_);
    const std::string source = has_active_code_resource_ && segment != resource_segments_.end()
        ? segment->second.path : thread_.name;
    {
        const bool parameters = (source.ends_with("bsctrl._bp") &&
            (current_opcode_ip_ == 0x147FU || current_opcode_ip_ == 0x1DA1U)) ||
            (source.ends_with("bsctrl4._bp") && current_opcode_ip_ == 0x3544U);
        if (opcode == 0x80U && current_opcode_ip_ + 1U < current_code().size() &&
            current_code()[current_opcode_ip_ + 1U] == 0xD4U) {
            std::cerr << "BS_GROUP_LOOKUP thread=" << thread_.id << " source=" << source
                      << " ip=0x" << std::hex << current_opcode_ip_ << " args=";
            const auto& values = thread_.stack.values();
            for (std::size_t i=values.size()>4U?values.size()-4U:0U;i<values.size();++i)
                std::cerr << values[i] << ',';
            if (values.size()>=2U && values[values.size()-2U]!=0U)
                std::cerr << " name=" << memory_.read_cstring(values[values.size()-2U], thread_.base);
            std::cerr << std::dec << '\n';
        }
        const bool packet = source.ends_with("bsctrl2._bp") &&
            current_opcode_ip_ == 0x4CU;
        if (parameters || packet) {
            std::cerr << "BS_PACKET thread=" << thread_.id
                      << " source=" << source << " ip=0x" << std::hex
                      << current_opcode_ip_ << " base=0x" << thread_.base;
            if (parameters) {
                std::cerr << " disabled=" << memory_.read_u32(0xDBB84U, 0)
                          << " pending=" << memory_.read_u32(0xDBB90U, 0)
                          << " parameters=";
                for (uint32_t off=0x300U;off<=0x388U;off+=4U)
                    std::cerr << memory_.read_u32(Memory::kFrameTag | (thread_.base-off), 0) << ',';
            } else {
                std::cerr << " slot=" << memory_.read_u32(Memory::kFrameTag | (thread_.base-0xE48U), 0)
                          << " record=";
                for (uint32_t off=0;off<192U;off+=4U)
                    std::cerr << memory_.read_u32(Memory::kFrameTag | (thread_.base-0xE30U+off), 0) << ',';
            }
            std::cerr << std::dec << '\n';
        }
    }
}
} // namespace bgiplayer
