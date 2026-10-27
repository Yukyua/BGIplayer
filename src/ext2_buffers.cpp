#include "bgiplayer/vm.hpp"
#include <algorithm>
#include <limits>
#include <new>

namespace bgiplayer {
// Wrapper results: invalid handle=1, insertion offset=2, range=3,
// allocation=4; unexpected helper errors=-1. Helpers are 1400F0320..0D30.
uint32_t VM::ext2_buffer_operation(uint8_t sub, Thread& thread) {
    auto pop = [&] { return thread.stack.pop(); };
    auto canonical = [](uint32_t id) {
        if (id >= 0x10000000U || (id & 0x0FFF0000U) != 0x0FFF0000U ||
            ((id >> 12) & 15) > 1) return uint32_t(0);
        return 0x0FFF0000U | (id & 0x1000U) | (id & 255U);
    };
    auto lookup = [&](uint32_t id, uint32_t kind) -> std::vector<uint8_t>* {
        const uint32_t key = canonical(id);
        if (!key || (key & 0x1000U) != kind * 0x1000U) return nullptr;
        const auto it = memory_.heap_allocations_.find(key);
        return it == memory_.heap_allocations_.end() ? nullptr : &it->second;
    };
    auto allocate = [&](uint32_t output, uint32_t size, uint32_t kind,
                        const std::string& text) -> uint32_t {
        if (size > 0x40000000U) return 3;
        uint32_t& cursor = kind ? ext2_string_cursor_ : ext2_binary_cursor_;
        for (uint32_t n = 0; n != 256; ++n) {
            cursor = (cursor + 31U) & 255U;
            const uint32_t id = 0x0FFF0000U | kind * 0x1000U | cursor;
            if (memory_.heap_allocations_.contains(id)) continue;
            try {
                std::vector<uint8_t> value;
                value.resize(size, 0);
                if (kind) std::copy(text.begin(), text.end(), value.begin());
                memory_.heap_allocations_.emplace(id, std::move(value));
            } catch (const std::bad_alloc&) { return 4; }
            memory_.write_u32(output, thread.base, id);
            return 0;
        }
        return 4;
    };
    if (sub == 0x80) {
        const uint32_t size = pop(), output = pop();
        return allocate(output, size, 0, {});
    }
    if (sub == 0x88) {
        const uint32_t format = pop(), output = pop();
        const std::string text = format ? memory_.read_cstring(format, thread.base) : std::string();
        return allocate(output, static_cast<uint32_t>(text.size()+1), 1, text);
    }
    if (sub == 0x81 || sub == 0x89) {
        const uint32_t id = pop(), kind = sub == 0x89;
        if (!lookup(id, kind)) return 1;
        memory_.heap_allocations_.erase(canonical(id)); return 0;
    }
    if (sub == 0x83) {
        const uint32_t id = pop(); auto* b = lookup(id, 0);
        return b ? static_cast<uint32_t>((*b).size()) : 0xFFFFFFFFU;
    }
    if (sub == 0x82) {
        const uint32_t size = pop(), id = pop();
        if (size > 0x40000000U) return 3;
        auto* b = lookup(id, 0); if (!b) return 1;
        try { (*b).resize(size, 0); }
        catch (const std::bad_alloc&) { return 4; }
        return 0;
    }
    if (sub == 0x84 || sub == 0x85 || sub == 0x86) {
        const uint32_t count = pop(), address = pop(), offset = pop(), id = pop();
        auto* b = lookup(id, 0); if (!b) return 1;
        const uint32_t old_size = static_cast<uint32_t>((*b).size());
        if (sub == 0x86) {
            const uint32_t position = static_cast<int32_t>(offset) < 0 ? old_size : offset;
            if (position > old_size) return 2;
            const uint32_t new_size = old_size + count; // original 32-bit addition
            if (new_size > 0x40000000U || new_size < old_size) return 3;
            const auto data = memory_.read_bytes(address, count, thread.base);
            try { (*b).insert((*b).begin()+position, data.begin(), data.end()); }
            catch (const std::bad_alloc&) { return 4; }
            return 0;
        }
        if (offset >= old_size) return 2;
        if (count > old_size-offset) return 3;
        if (!count) return 0;
        if (sub == 0x85) memory_.write_bytes(address,
            std::span<const uint8_t>((*b)).subspan(offset,count),thread.base);
        else {
            const auto data = memory_.read_bytes(address,count,thread.base);
            std::copy(data.begin(),data.end(),(*b).begin()+offset);
        }
        return 0;
    }
    uint32_t format = pop(), offset = 0;
    if (sub == 0x8A) offset = pop();
    const uint32_t id = pop(); auto* b = lookup(id, 1); if (!b) return 1;
    if (sub == 0x8B) { (*b).assign(1,0); if (!format) return 0; }
    const uint32_t old_length = static_cast<uint32_t>((*b).size()-1);
    const uint32_t position = sub == 0x8B ? 0 :
        static_cast<int32_t>(offset) < 0 ? old_length : offset;
    if (position > old_length) return 2;
    const std::string text = format_opcode_values(memory_.read_cstring(format,thread.base),pop);
    if (text.size() > 0x40000000U-(*b).size()) return 3;
    try { (*b).insert((*b).begin()+position,text.begin(),text.end()); }
    catch (const std::bad_alloc&) { return 4; }
    return 0;
}

uint32_t VM::ext2_sort_records(Thread& thread) {
    const uint32_t mode = thread.stack.pop(), offset = thread.stack.pop();
    const uint32_t stride = thread.stack.pop(), count = thread.stack.pop();
    const uint32_t address = thread.stack.pop();
    if (count < 2) return 1;
    if (mode > 5) return 3;
    const uint32_t width = 1U << (mode / 2U);
    if (uint64_t(offset) + width > stride) return 2;
    if (uint64_t(count)*stride > std::numeric_limits<uint32_t>::max())
        throw BpFault("Ext2 sort record span overflows VM memory");
    auto bytes = memory_.read_bytes(address,std::size_t(count)*stride,thread.base);
    auto key = [&](int64_t i) -> int32_t {
        uint32_t raw=0; for(uint32_t n=0;n<width;++n)
            raw |= uint32_t(bytes[std::size_t(i)*stride+offset+n]) << (n*8);
        return width==1 ? int32_t(int8_t(raw)) : width==2 ? int32_t(int16_t(raw)) : int32_t(raw);
    };
    auto compare = [&](int64_t a,int64_t b) {
        const uint32_t difference=mode&1U ? uint32_t(key(b))-uint32_t(key(a)) :
                                           uint32_t(key(a))-uint32_t(key(b));
        return static_cast<int32_t>(difference);
    };
    auto swap_record = [&](int64_t a,int64_t b) {
        for(uint32_t n=0;n<stride;++n)
            std::swap(bytes[std::size_t(a)*stride+n],bytes[std::size_t(b)*stride+n]);
    };
    // Direct index translation of 14001AF70: median-of-three quicksort,
    // <=8-element maximum-selection shortsort. Unlike std::sort this bounded
    // flow does not require a strict weak ordering from subtract comparators.
    std::vector<std::pair<int64_t,int64_t>> pending{{0,int64_t(count)-1}};
    while(!pending.empty()) {
        auto [low,high]=pending.back();pending.pop_back();
        while(low<high) {
            if(high-low+1<=8) {
                for(int64_t end=high;end>low;--end) {
                    int64_t maximum=low;
                    for(int64_t i=low+1;i<=end;++i)
                        if(compare(i,maximum)>0) maximum=i;
                    swap_record(maximum,end);
                }
                break;
            }
            int64_t middle=low+(high-low+1)/2;
            if(compare(low,middle)>0)swap_record(low,middle);
            if(compare(low,high)>0)swap_record(low,high);
            if(compare(middle,high)>0)swap_record(middle,high);
            int64_t left=low,right=high,previous_right=right;
            for(;;) {
                if(middle>left) {
                    do { ++left; } while(left<middle && compare(left,middle)<=0);
                }
                if(left>=middle) {
                    do { ++left; } while(left<=high && compare(left,middle)<=0);
                }
                do { previous_right=right;--right; }
                    while(right>middle && compare(right,middle)>0);
                if(right<left)break;
                swap_record(left,right);
                if(middle==right)middle=left;
            }
            int64_t equal_end=previous_right;
            if(middle<equal_end) {
                do {--equal_end;} while(equal_end>middle && compare(equal_end,middle)==0);
            }
            if(equal_end<=middle) {
                do {--equal_end;} while(equal_end>low && compare(equal_end,middle)==0);
            }
            if(equal_end-low >= high-left) {
                if(low<equal_end)pending.emplace_back(low,equal_end);
                low=left;
            } else {
                if(left<high)pending.emplace_back(left,high);
                high=equal_end;
            }
        }
    }
    memory_.write_bytes(address,bytes,thread.base); return 0;
}
} // namespace bgiplayer
