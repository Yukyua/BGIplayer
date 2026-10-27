#include "bgiplayer/vm.hpp"
#include <cmath>

namespace bgiplayer {
namespace {
// Target 0x1400D1800 (64-bit engine): comparison used by the compact
// compare/branch instructions. Modes above 9 report false; modes 6/7 return
// the raw AND/OR result, matching the original for the pushing variants.
uint32_t compact_compare(uint32_t mode, uint32_t left, uint32_t right) {
    switch (mode) {
    case 0: return left == right ? 1U : 0U;
    case 1: return left != right ? 1U : 0U;
    case 2: return static_cast<int32_t>(left) < static_cast<int32_t>(right) ? 1U : 0U;
    case 3: return static_cast<int32_t>(left) <= static_cast<int32_t>(right) ? 1U : 0U;
    case 4: return static_cast<int32_t>(left) >= static_cast<int32_t>(right) ? 1U : 0U;
    case 5: return static_cast<int32_t>(left) > static_cast<int32_t>(right) ? 1U : 0U;
    case 6: return left & right;
    case 7: return left | right;
    case 8: return (left != 0 && right != 0) ? 1U : 0U;
    case 9: return (left != 0 || right != 0) ? 1U : 0U;
    default: return 0U;
    }
}

// Target 0x1400D18B0 (64-bit engine): arithmetic selector for the compact
// store-operations. Division/modulo by zero yields 0x80000000; the x86 idiv
// overflow case (INT32_MIN / -1) is fatal instead of undefined.
uint32_t compact_arith(uint32_t selector, uint32_t left, uint32_t right) {
    const int32_t integer_left = static_cast<int32_t>(left);
    const int32_t integer_right = static_cast<int32_t>(right);
    switch (selector) {
    case 0: return left + right;
    case 1: return left - right;
    case 2: return left * right;
    case 3:
        if (integer_right == 0) return 0x80000000U;
        if (integer_left == std::numeric_limits<int32_t>::min() &&
            integer_right == -1) throw BpFault("compact BP integer divide exception");
        return static_cast<uint32_t>(integer_left / integer_right);
    case 4:
        if (integer_right == 0) return 0x80000000U;
        if (integer_left == std::numeric_limits<int32_t>::min() &&
            integer_right == -1) throw BpFault("compact BP integer divide exception");
        return static_cast<uint32_t>(integer_left % integer_right);
    case 5: return left & right;
    case 6: return left | right;
    case 7: return left ^ right;
    case 8: return left << (right & 31U);
    case 9: return left >> (right & 31U);
    case 10: return static_cast<uint32_t>(integer_left >> (right & 31U));
    default: return 0x80000000U;
    }
}
}  // namespace

// 2022 target: 0052F660 opcode table; 0044D190 reads signed LEB groups.
// 64-bit target (0x1401D0B80) adds the compact 0x0D/0x0E/0x13/0x36/0x37/
// 0x3E/0x3F, 0xD4..0xDF, 0xE2..0xEA, 0xEE and 0xF0..0xFB handlers described
// below without changing the earlier compact encodings.
// x86 shift counts wrap modulo 32, including noncanonical five-byte encodings.
VM::StepResult VM::op_compact() {
    const uint8_t opcode = current_code()[current_opcode_ip_];
    const auto immediate = [this]() {
        uint32_t value=0, count=0; uint8_t byte;
        do { byte=read_code8();value |= static_cast<uint32_t>(byte&0x7fU) << ((count*7U)&31U);++count; } while(byte&0x80U);
        if(byte&0x40U) {
            // PE64 140088884..88E uses a 64-bit sign-extension mask,
            // then ORs its low dword. Five-byte negative operands must not
            // wrap the mask shift back into their low bits.
            value |= compact_64_opcodes_
                ? static_cast<uint32_t>(UINT64_MAX << ((count*7U)&63U))
                : 0xffffffffU << ((count*7U)&31U);
        }
        return value;
    };
    const auto load = [this](uint32_t address,uint32_t width) -> uint32_t {
        if(width==2)return memory_.read_u32(address,thread_.base);
        if(width==1)return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(memory_.read_u16(address,thread_.base))));
        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(memory_.read_u8(address,thread_.base))));
    };
    const auto store = [this](uint32_t address,uint32_t width,uint32_t value) {
        if(width==2)memory_.write_u32(address,thread_.base,value);
        else if(width==1)memory_.write_u16(address,thread_.base,static_cast<uint16_t>(value));
        else memory_.write_u8(address,thread_.base,static_cast<uint8_t>(value));
    };
    const auto local = [this](uint32_t offset) {
        return (thread_.base-offset)|Memory::kFrameTag;
    };
    const auto read_local = [&](uint32_t offset,uint32_t width) {
        return load(local(offset),width);
    };
    const auto write_local = [&](uint32_t offset,uint32_t width,uint32_t value) {
        store(local(offset),width,value);
    };
    // 0x140088750: signed LEB of (value<<2 | width); returns value, sets width.
    const auto packed = [&](uint32_t& width) {
        if(compact_64_opcodes_) {
            // 1400887BE..7E0 strips width from the first group before
            // continuing. Accumulating the whole encoding into a dword
            // first loses the top two value bits (80000000 becomes zero).
            uint8_t byte=read_code8();
            width=byte&3U;
            uint32_t value=(byte&0x7fU)>>2U,shift=5U;
            while(byte&0x80U) {
                byte=read_code8();
                value|=static_cast<uint32_t>(byte&0x7fU)<<(shift&31U);
                shift+=7U;
            }
            if(byte&0x40U)value|=static_cast<uint32_t>(UINT64_MAX<<(shift&63U));
            return value;
        }
        const uint32_t raw=immediate();
        width=raw&3U;
        return static_cast<uint32_t>(static_cast<int32_t>(raw)>>2);
    };
    const auto relative = [this](int32_t displacement) {
        return current_opcode_ip_+static_cast<uint32_t>(displacement)
            +(has_active_code_resource_?active_code_resource_base_:0U);
    };
    const auto divide = [](uint32_t numerator,uint32_t denominator) {
        const int32_t n=static_cast<int32_t>(numerator),d=static_cast<int32_t>(denominator);
        if(d==0 || (n==std::numeric_limits<int32_t>::min()&&d==-1))throw BpFault("compact BP integer divide exception");
        return static_cast<uint32_t>(n/d);
    };
    if(opcode==0x03U) {
        // 0x1400D16C0: push count+1 signed LEB immediates (selector < 0x80),
        // or one immediate selected by the low two selector bits. Other high
        // selector bits make the target instruction a silent no-op.
        const uint32_t selector=read_code8();
        if((selector&0x80U)==0U) {
            const uint32_t count=selector+1U;
            for(uint32_t index=0;index<count;++index)
                thread_.stack.push(immediate());
        } else if((selector&0x7cU)==0U) {
            switch(selector&3U) {
            case 0: thread_.stack.push(static_cast<uint32_t>(
                        static_cast<int32_t>(static_cast<int8_t>(read_code8())))); break;
            case 1: thread_.stack.push(static_cast<uint32_t>(
                        static_cast<int32_t>(static_cast<int16_t>(read_code16())))); break;
            case 2: thread_.stack.push(read_code32()); break;
            default: {
                // The target pushes only the low dword of the 64-bit value.
                const uint32_t low=read_code32();
                read_code32();
                thread_.stack.push(low);
                break;
            }
            }
        }
        return StepResult::Continue;
    }
    if(opcode==0x46U || opcode==0x47U) {
        // 00483500/004835E0: SSE single precision at every stage,
        // CVTPS2DQ nearest-even, then snap a 64/65536 edge neighborhood.
        const int32_t first=static_cast<int32_t>(thread_.stack.pop());
        const int32_t second=static_cast<int32_t>(thread_.stack.pop());
        const int32_t third=static_cast<int32_t>(thread_.stack.pop());
        float start,end,ratio;
        if(opcode==0x46U) {
            start=static_cast<float>(static_cast<int32_t>(thread_.stack.pop()));
            end=static_cast<float>(third);
            ratio=first==0?0.0F:static_cast<float>(second)/static_cast<float>(first);
        } else {
            start=static_cast<float>(third)*0x1p-16F;
            end=static_cast<float>(second)*0x1p-16F;
            ratio=static_cast<float>(first)*0x1p-16F;
        }
        ratio=std::clamp(ratio,0.0F,1.0F);
        // Volatile temporaries prevent host multiply-add fusion.
        volatile float delta=end-start;
        volatile float scaled=delta*ratio;
        volatile float value=start+scaled;
        const float rounded=std::nearbyint(value*65536.0F);
        uint32_t result=(!std::isfinite(rounded) || rounded < -0x1p31F || rounded >= 0x1p31F)
            ?0x80000000U:static_cast<uint32_t>(static_cast<int32_t>(rounded));
        const uint32_t fraction=result&0xffffU;
        if(fraction<64U)result&=0xffff0000U;
        else if(fraction>0xffc0U)result=(result&0xffff0000U)+0x10000U;
        thread_.stack.push(result);return StepResult::Continue;
    }
    if(opcode==0x12U) {
        const uint32_t next_base=thread_.base+immediate();
        // 1400D11AB..D11B4 checks the unsigned sum before committing it.
        if(compact_64_opcodes_ && next_base>data_stack_limit())
            throw BpFault("frame stack pointer exceeds data arena");
        thread_.base=next_base;
        return StepResult::Continue;
    }
    if(opcode==0x0dU) {
        // 0x1400D1380: store a packed immediate through a popped address.
        uint32_t width=0;
        const uint32_t value=packed(width);
        store(thread_.stack.pop(),width,value);
        return StepResult::Continue;
    }
    if(opcode==0x0eU) {
        // 0x1400D12F0: store a packed immediate into local[width,offset].
        const uint32_t offset=read_code16();
        uint32_t width=0;
        const uint32_t value=packed(width);
        write_local(offset,width,value);
        return StepResult::Continue;
    }
    if(opcode==0x0fU) {
        const uint32_t packed=read_code16();
        const uint32_t address=(thread_.base-(packed&0x3fffU))|Memory::kFrameTag;
        const uint32_t value=thread_.stack.pop();
        if((packed>>14U)==2)memory_.write_u32(address,thread_.base,value);
        else if((packed>>14U)==1)memory_.write_u16(address,thread_.base,static_cast<uint16_t>(value));
        else memory_.write_u8(address,thread_.base,static_cast<uint8_t>(value));
        return StepResult::Continue;
    }
    if(opcode==0x13U) {
        // 0x1400D1160: unconditional int16 relative branch. The target does
        // not validate the destination here; jump() retains the arena checks.
        jump(relative(static_cast<int16_t>(read_code16())));
        return StepResult::Continue;
    }
    if(opcode==0x18U) {
        const uint32_t packed=read_code32();thread_.stack.push(load(packed&0x3fffffffU,packed>>30U));return StepResult::Continue;
    }
    if(opcode>=0x19U && opcode<=0x1eU) {
        const uint32_t packed=read_code16();
        uint32_t operand=0,accumulator=0;
        if(opcode==0x1aU||opcode==0x1bU)operand=immediate();
        if(opcode==0x1bU||opcode==0x1dU||opcode==0x1eU)accumulator=thread_.stack.pop();
        if(opcode==0x1cU)operand=read_code8();
        uint32_t value=load((thread_.base-(packed&0x3fffU))|Memory::kFrameTag,packed>>14U);
        switch(opcode) {
        case 0x1a:value+=operand;break;
        case 0x1b:value=value*operand+accumulator;break;
        case 0x1c: {
            const int32_t shift=static_cast<int8_t>(operand);
            value=shift<0?static_cast<uint32_t>(static_cast<int32_t>(value)>>((-shift)&31)) : value<<(shift&31);break;
        }
        case 0x1d:value*=accumulator;break;
        case 0x1e:value=compact_64_opcodes_ && value==0U?0x80000000U:divide(accumulator,value);break;
        default:break;
        }
        thread_.stack.push(value);return StepResult::Continue;
    }
    if(opcode==0x1fU) {
        if(compact_64_opcodes_) {
            // 1400D0B2E uses the same width-splitting helper as stores.
            uint32_t width=0;
            const uint32_t offset=packed(width);
            thread_.stack.push(load(thread_.stack.pop()+offset,width));
        } else {
            const uint32_t raw=immediate();
            const uint32_t address=thread_.stack.pop()+static_cast<uint32_t>(static_cast<int32_t>(raw)>>2);
            thread_.stack.push(load(address,raw&3U));
        }
        return StepResult::Continue;
    }
    if(opcode>=0x2cU&&opcode<=0x2fU) {
        const uint32_t operand=immediate(), value=thread_.stack.pop();uint32_t result;
        if(opcode==0x2cU)result=value+operand;
        else if(opcode==0x2dU)result=value*operand;
        else if(opcode==0x2eU)result=thread_.stack.pop()+value*operand;
        else result=operand==0?(compact_64_opcodes_?0x80000000U:0xffffffffU):divide(value,operand);
        thread_.stack.push(result);return StepResult::Continue;
    }
    if(opcode==0x36U) {
        // 0x1400D0520: compare the popped value with a signed LEB immediate
        // and push the raw 0x1400D1800 result. The full mode byte is used.
        const uint32_t mode=read_code8();
        const uint32_t value=immediate();
        const uint32_t left=thread_.stack.pop();
        thread_.stack.push(compact_compare(mode,left,value));
        return StepResult::Continue;
    }
    if(opcode==0x37U) {
        // 0x1400D0460: compare the popped value with a signed LEB immediate
        // and branch relative to the current opcode.
        const uint32_t mode=read_code8();
        const int32_t displacement=static_cast<int16_t>(read_code16());
        const uint32_t value=immediate();
        const uint32_t left=thread_.stack.pop();
        const uint32_t comparison=compact_compare(mode&0x0fU,left,value);
        if((mode&0x80U)!=0U?comparison!=0U:comparison==0U)
            jump(relative(displacement));
        return StepResult::Continue;
    }
    if(opcode==0x3bU) {
        const uint32_t mode=read_code8();const int32_t displacement=static_cast<int16_t>(read_code16());
        const uint32_t target=relative(displacement);
        const uint32_t right=thread_.stack.pop(),left=thread_.stack.pop();
        const uint32_t comparison=compact_compare(mode&0x0fU,left,right);
        if((mode&0x80U)!=0U?comparison!=0U:comparison==0U)jump(target);
        return StepResult::Continue;
    }
    if(opcode==0x3cU) {
        const int32_t shift=static_cast<int8_t>(read_code8());const uint32_t value=thread_.stack.pop();
        thread_.stack.push(shift<0?static_cast<uint32_t>(static_cast<int32_t>(value)>>((-shift)&31)):value<<(shift&31));
        return StepResult::Continue;
    }
    if(opcode==0x3eU) {
        // 0x1400D01D0: pop two values and the destination address, apply the
        // arith/compare selector (bit 5 selects compare), then store with the
        // width in bits 6..7.
        const uint32_t mode=read_code8();
        const uint32_t right=thread_.stack.pop();
        const uint32_t left=thread_.stack.pop();
        const uint32_t address=thread_.stack.pop();
        const uint32_t selector=mode&0x1fU;
        const uint32_t result=(mode&0x20U)!=0U
            ?compact_compare(selector,left,right)
            :compact_arith(selector,left,right);
        store(address,mode>>6U,result);
        return StepResult::Continue;
    }
    if(opcode==0x3fU) {
        // 0x1400D0140: compare/arith the popped value with an immediate and
        // store through the destination address popped afterwards.
        const uint32_t mode=read_code8();
        const uint32_t value=immediate();
        const uint32_t left=thread_.stack.pop();
        const uint32_t address=thread_.stack.pop();
        const uint32_t selector=mode&0x1fU;
        const uint32_t result=(mode&0x20U)!=0U
            ?compact_compare(selector,left,value)
            :compact_arith(selector,left,value);
        store(address,mode>>6U,result);
        return StepResult::Continue;
    }
    if(opcode==0xd4U) {
        // 0x1400CDD40: push the local dword at base-offset.
        thread_.stack.push(read_local(read_code16(),2));
        return StepResult::Continue;
    }
    if(opcode==0xd5U) {
        // 0x1400CDCE0: push local dword + signed LEB addend.
        const uint32_t offset=read_code16();
        const uint32_t addend=immediate();
        thread_.stack.push(read_local(offset,2)+addend);
        return StepResult::Continue;
    }
    if(opcode==0xd6U) {
        // 0x1400CDC50: push local[offset][packed index] with its width.
        const uint32_t offset=read_code16();
        uint32_t width=0;
        const uint32_t index=packed(width);
        const uint32_t base=read_local(offset,2);
        thread_.stack.push(load(base+index,width));
        return StepResult::Continue;
    }
    if(opcode==0xd8U) {
        // 0x1400CDB40: push local dword * signed LEB factor + popped addend.
        const uint32_t offset=read_code16();
        const uint32_t factor=immediate();
        const uint32_t addend=thread_.stack.pop();
        thread_.stack.push(read_local(offset,2)*factor+addend);
        return StepResult::Continue;
    }
    if(opcode==0xdaU) {
        // 0x1400CDAC0: compare the local dword with a signed LEB immediate
        // and push the raw comparison result (full mode byte).
        const uint32_t mode=read_code8();
        const uint32_t offset=read_code16();
        const uint32_t value=immediate();
        thread_.stack.push(compact_compare(mode,read_local(offset,2),value));
        return StepResult::Continue;
    }
    if(opcode==0xdbU) {
        // 0x1400CDA10: same comparison on local[offset][packed index].
        const uint32_t mode=read_code8();
        const uint32_t offset=read_code16();
        uint32_t width=0;
        const uint32_t index=packed(width);
        const uint32_t value=immediate();
        const uint32_t base=read_local(offset,2);
        thread_.stack.push(compact_compare(mode,load(base+index,width),value));
        return StepResult::Continue;
    }
    if(opcode==0xdcU) {
        // 64-bit target 0x1400CD920: fetch (mode, int16 displacement,
        // uint16 local offset, signed LEB compare value), compare the
        // thread-local dword at base-offset, then conditionally branch to
        // current opcode IP + displacement. Bit 7 selects jump-if-true.
        const uint32_t mode=read_code8();
        const int32_t displacement=static_cast<int16_t>(read_code16());
        const uint32_t offset=read_code16();
        const uint32_t expected=immediate();
        const uint32_t actual=read_local(offset,2);
        const uint32_t comparison=compact_compare(mode&0x0fU,actual,expected);
        if((mode&0x80U)!=0U?comparison!=0U:comparison==0U)
            jump(relative(displacement));
        return StepResult::Continue;
    }
    if(opcode==0xddU) {
        // 0x1400CD800: compare local[offset][packed index] with an immediate
        // and conditionally branch (mode bits 0..3, bit 7 inverts).
        const uint32_t mode=read_code8();
        const int32_t displacement=static_cast<int16_t>(read_code16());
        const uint32_t offset=read_code16();
        uint32_t width=0;
        const uint32_t index=packed(width);
        const uint32_t value=immediate();
        const uint32_t base=read_local(offset,2);
        const uint32_t comparison=
            compact_compare(mode&0x0fU,load(base+index,width),value);
        if((mode&0x80U)!=0U?comparison!=0U:comparison==0U)
            jump(relative(displacement));
        return StepResult::Continue;
    }
    if(opcode==0xdeU) {
        // 0x1400CD740: branch when the local dword is (bit 15 set) or is not
        // (bit 15 clear) zero.
        const int32_t displacement=static_cast<int16_t>(read_code16());
        const uint32_t descriptor=read_code16();
        const uint32_t value=read_local(descriptor&0x7fffU,2);
        const bool branch_if_nonzero=(descriptor&0x8000U)!=0U;
        if(branch_if_nonzero?value!=0U:value==0U)jump(relative(displacement));
        return StepResult::Continue;
    }
    if(opcode==0xdfU) {
        // 0x1400CD640: same zero test on local[offset][packed index].
        const int32_t displacement=static_cast<int16_t>(read_code16());
        const uint32_t descriptor=read_code16();
        uint32_t width=0;
        const uint32_t index=packed(width);
        const uint32_t base=read_local(descriptor&0x7fffU,2);
        const uint32_t value=load(base+index,width);
        const bool branch_if_nonzero=(descriptor&0x8000U)!=0U;
        if(branch_if_nonzero?value!=0U:value==0U)jump(relative(displacement));
        return StepResult::Continue;
    }
    if(opcode==0xe2U) {
        // 0x1400CD5A0: pop count+1 values into packed local descriptors.
        const uint32_t count=read_code8()+1U;
        for(uint32_t index=0;index<count;++index) {
            const uint32_t descriptor=read_code16();
            write_local(descriptor&0x3fffU,descriptor>>14U,thread_.stack.pop());
        }
        return StepResult::Continue;
    }
    if(opcode==0xe3U) {
        // 0x1400CD500: push count+1 packed local descriptors.
        const uint32_t count=read_code8()+1U;
        for(uint32_t index=0;index<count;++index) {
            const uint32_t descriptor=read_code16();
            thread_.stack.push(read_local(descriptor&0x3fffU,descriptor>>14U));
        }
        return StepResult::Continue;
    }
    if(opcode==0xe5U) {
        // 0x1400CD390: push load(local[pw] * packed index + popped address).
        const uint32_t descriptor=read_code16();
        uint32_t width=0;
        const uint32_t index=packed(width);
        const uint32_t addend=thread_.stack.pop();
        const uint32_t stride=read_local(descriptor&0x3fffU,descriptor>>14U);
        thread_.stack.push(load(stride*index+addend,width));
        return StepResult::Continue;
    }
    if(opcode==0xe6U) {
        // 0x1400CD2E0: *popped address = local[pw] + packed immediate.
        const uint32_t descriptor=read_code16();
        uint32_t width=0;
        const uint32_t addend=packed(width);
        const uint32_t address=thread_.stack.pop();
        store(address,width,read_local(descriptor&0x3fffU,descriptor>>14U)+addend);
        return StepResult::Continue;
    }
    if(opcode==0xe7U) {
        // 0x1400CD220: *popped address = local[pw] * packed + popped addend.
        const uint32_t descriptor=read_code16();
        uint32_t width=0;
        const uint32_t factor=packed(width);
        const uint32_t addend=thread_.stack.pop();
        const uint32_t address=thread_.stack.pop();
        const uint32_t value=read_local(descriptor&0x3fffU,descriptor>>14U);
        store(address,width,value*factor+addend);
        return StepResult::Continue;
    }
    if(opcode==0xe8U) {
        // 0x1400CD170: local[pw] = condition ? second : first, where the
        // condition is the third popped value.
        const uint32_t descriptor=read_code16();
        const uint32_t first=thread_.stack.pop();
        const uint32_t second=thread_.stack.pop();
        const uint32_t condition=thread_.stack.pop();
        write_local(descriptor&0x3fffU,descriptor>>14U,
                    condition==0U?first:second);
        return StepResult::Continue;
    }
    if(opcode==0xeaU) {
        // 0x1400CD050: store an immediate into local[offset][uint16 index].
        const uint32_t descriptor=read_code16();
        const uint32_t index=read_code16();
        const uint32_t value=immediate();
        const uint32_t base=read_local(descriptor&0x3fffU,2);
        store(base+index,descriptor>>14U,value);
        return StepResult::Continue;
    }
    if(opcode==0xeeU) {
        // 0x1400CCEA0: call with an inline int16 displacement. The return
        // address is the byte after the three-byte instruction.
        const uint32_t target=relative(static_cast<int16_t>(read_code16()));
        if(target==0U)throw BpFault("compact call target is null");
        push_physical_return(current_opcode_ip_+3U);
        jump(target);
        return StepResult::Continue;
    }
    if(opcode==0xf0U) {
        // 0x1400CCD10: *(u32 address) = local[pw] + signed LEB addend.
        const uint32_t address=read_code32();
        const uint32_t descriptor=read_code16();
        const uint32_t addend=immediate();
        store(address,descriptor>>14U,read_local(descriptor&0x3fffU,2)+addend);
        return StepResult::Continue;
    }
    if(opcode==0xf1U) {
        // 0x1400CCC60: local[destination] = local[pw] + signed LEB addend.
        const uint32_t destination=read_code16();
        const uint32_t descriptor=read_code16();
        const uint32_t addend=immediate();
        write_local(destination,descriptor>>14U,
                    read_local(descriptor&0x3fffU,2)+addend);
        return StepResult::Continue;
    }
    if(opcode==0xf2U) {
        // 0x1400CCB90: indexed local store from local[pw] + LEB addend.
        const uint32_t destination=read_code16();
        const uint32_t index=read_code16();
        const uint32_t descriptor=read_code16();
        const uint32_t addend=immediate();
        const uint32_t address=read_local(destination,2)+index;
        store(address,descriptor>>14U,
              read_local(descriptor&0x3fffU,2)+addend);
        return StepResult::Continue;
    }
    if(opcode==0xf5U) {
        // 0x1400CC9E0: local[dst] = local[pw]*imm2 + imm1 + local[second].
        const uint32_t destination=read_code16();
        const uint32_t second=read_code16();
        const uint32_t first_immediate=
            static_cast<uint32_t>(static_cast<int16_t>(read_code16()));
        const uint32_t descriptor=read_code16();
        const uint32_t second_immediate=
            static_cast<uint32_t>(static_cast<int16_t>(read_code16()));
        const uint32_t value=
            read_local(descriptor&0x3fffU,2)*second_immediate
            +first_immediate+read_local(second,2);
        write_local(destination,descriptor>>14U,value);
        return StepResult::Continue;
    }
    if(opcode==0xf8U) {
        // 0x1400CC8B0: push local[offset] * int16 factor + u32 term.
        const uint32_t term=read_code32();
        const uint32_t offset=read_code16();
        const uint32_t factor=
            static_cast<uint32_t>(static_cast<int16_t>(read_code16()));
        thread_.stack.push(read_local(offset,2)*factor+term);
        return StepResult::Continue;
    }
    if(opcode==0xf9U) {
        // 0x1400CC800: push local[c]*imm2 + local[a] + imm1.
        const uint32_t first=read_code16();
        const uint32_t first_immediate=
            static_cast<uint32_t>(static_cast<int16_t>(read_code16()));
        const uint32_t third=read_code16();
        const uint32_t second_immediate=
            static_cast<uint32_t>(static_cast<int16_t>(read_code16()));
        thread_.stack.push(read_local(third,2)*second_immediate
                           +read_local(first,2)+first_immediate);
        return StepResult::Continue;
    }
    if(opcode==0xfbU) {
        // 0x1400CC6B0: push load((base-a) + local[pw]*factor, width).
        const uint32_t base_offset=read_code16();
        const uint32_t descriptor=read_code16();
        const uint32_t factor=
            static_cast<uint32_t>(static_cast<int16_t>(read_code16()));
        const uint32_t address=
            local(base_offset)+read_local(descriptor&0x3fffU,2)*factor;
        thread_.stack.push(load(address,descriptor>>14U));
        return StepResult::Continue;
    }
    if(opcode==0xd7U) {
        // 0x1400CDBC0: local[off1][off2] = packed immediate.
        const uint32_t base_offset=read_code16();
        const uint32_t index=read_code16();
        uint32_t width=0;
        const uint32_t value=packed(width);
        store(read_local(base_offset,2)+index,width,value);
        return StepResult::Continue;
    }
    if(opcode==0xe4U) {
        // 0x1400CD450: push local[descriptor][packed index].
        const uint32_t descriptor=read_code16();
        uint32_t width=0;
        const uint32_t index=packed(width);
        thread_.stack.push(load(
            read_local(descriptor&0x3fffU,descriptor>>14U)+index,width));
        return StepResult::Continue;
    }
    if(opcode==0xe9U) {
        // 0x1400CD0E0: local[descriptor][index] = popped value.
        const uint32_t descriptor=read_code16();
        const uint32_t index=read_code16();
        const uint32_t value=thread_.stack.pop();
        store(read_local(descriptor&0x3fffU,2)+index,descriptor>>14U,value);
        return StepResult::Continue;
    }
    if(opcode==0xecU) {
        // 0x1400CCFF0: memmove(second popped destination, first popped
        // source, signed LEB count).
        const uint32_t count=immediate();
        const uint32_t source=thread_.stack.pop();
        const uint32_t destination=thread_.stack.pop();
        memory_.write_bytes(
            destination,
            memory_.read_bytes(source,count,thread_.base),
            thread_.base);
        return StepResult::Continue;
    }
    if(opcode==0xedU) {
        // 0x1400CCF70: memmove(popped destination, local[offset], count).
        const uint32_t offset=read_code16();
        const uint32_t count=immediate();
        const uint32_t destination=thread_.stack.pop();
        memory_.write_bytes(
            destination,
            memory_.read_bytes(local(offset),count,thread_.base),
            thread_.base);
        return StepResult::Continue;
    }
    if(opcode==0xefU) {
        // 0x1400CCDB0: absolute call through a packed memory operand. The
        // return address is the byte after the five-byte instruction.
        const uint32_t packed_address=read_code32();
        const uint32_t target=
            load(packed_address&0x3fffffffU,packed_address>>30U);
        if(target==0U)throw BpFault("compact call target is null");
        push_physical_return(current_opcode_ip_+5U);
        jump(target);
        return StepResult::Continue;
    }
    if(opcode==0xf4U) {
        // 0x1400CCAD0: local[A] = local[C] * int16 + u32 term.
        const uint32_t destination=read_code16();
        const uint32_t term=read_code32();
        const uint32_t descriptor=read_code16();
        const uint32_t factor=
            static_cast<uint32_t>(static_cast<int16_t>(read_code16()));
        write_local(destination,descriptor>>14U,
                    read_local(descriptor&0x3fffU,2)*factor+term);
        return StepResult::Continue;
    }
    if(opcode==0xf7U) {
        // 0x1400CC920: push local[A][index] * int16 + popped addend.
        const uint32_t descriptor=read_code16();
        const uint32_t index=read_code16();
        const uint32_t factor=
            static_cast<uint32_t>(static_cast<int16_t>(read_code16()));
        const uint32_t addend=thread_.stack.pop();
        const uint32_t element=load(
            read_local(descriptor&0x3fffU,2)+index,descriptor>>14U);
        thread_.stack.push(element*factor+addend);
        return StepResult::Continue;
    }
    if(opcode==0xfaU) {
        // 0x1400CC760: push load(term + local[C]*int16, width C>>14).
        const uint32_t term=read_code32();
        const uint32_t descriptor=read_code16();
        const uint32_t factor=
            static_cast<uint32_t>(static_cast<int16_t>(read_code16()));
        thread_.stack.push(load(
            term+read_local(descriptor&0x3fffU,2)*factor,
            descriptor>>14U));
        return StepResult::Continue;
    }
    throw BpFault("unresolved compact BP opcode");
}
}
