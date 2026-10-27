#include "bgiplayer/vm.hpp"

namespace bgiplayer {
namespace {
uint32_t resource_key_address(std::string_view key) {
    uint64_t value = 0;
    const auto separator = key.find(':');
    if (separator == std::string_view::npos || separator == 0)
        throw BpFault("loaded resource has an invalid address key");
    for (const char digit : key.substr(0, separator)) {
        if (digit < '0' || digit > '9')
            throw BpFault("loaded resource has an invalid address key");
        value = value * 10U + static_cast<unsigned>(digit - '0');
        if (value > UINT32_MAX)
            throw BpFault("loaded resource address exceeds VM width");
    }
    return static_cast<uint32_t>(value);
}
}

bool VM::resource_record_is_referenced(uint32_t id) const {
    const auto references = [id](const Thread& thread, bool active, uint32_t base) {
        if (active && base == id) return true;
        return std::any_of(thread.frame_stack.begin(), thread.frame_stack.end(),
            [id](const CallFrame& frame) {
                return frame.has_code_resource && frame.code_resource_base == id;
            });
    };
    if (references(root_container_thread_, false, 0)) return true;
    if (references(thread_, has_active_code_resource_, active_code_resource_base_))
        return true;
    // The saved main program's active segment is held by the scheduler's
    // local selection state. Defer retirement during this pass; the scheduler
    // collects immediately after restoring that selection and its fallbacks.
    if (scheduler_saved_thread_) return true;
    for (const auto& [handle, state] : program_threads_)
        if (references(state.thread, state.has_code_resource, state.code_resource_base))
            return true;
    return false;
}

void VM::retire_resource_record(uint32_t id) {
    // Original program arenas retain their bytes after an unload. A worker
    // or a physical return frame may still address that span. Pin its host
    // translation until that reference has gone, then collect on allocation.
    if (resource_record_is_referenced(id)) return;
    resource_segments_.erase(id);
    const std::string prefix = std::to_string(id) + ':';
    for (auto it = resource_cache_.begin(); it != resource_cache_.end();) {
        if (it->first.starts_with(prefix)) it = resource_cache_.erase(it);
        else ++it;
    }
}

void VM::release_thread_resources(uint32_t id) {
    const auto owner = resource_orders_by_thread_.find(id);
    if (owner == resource_orders_by_thread_.end()) return;
    const auto records = std::move(owner->second);
    resource_orders_by_thread_.erase(owner);
    for (const uint32_t record : records) retire_resource_record(record);
}

void VM::collect_retired_resource_records() {
    std::set<uint32_t> owned;
    for (const auto& [thread, records] : resource_orders_by_thread_)
        owned.insert(records.begin(), records.end());
    std::vector<uint32_t> retired;
    for (const auto& [key, data] : resource_cache_) {
        const uint32_t id = resource_key_address(key);
        if (!owned.contains(id) && !resource_record_is_referenced(id))
            retired.push_back(id);
    }
    for (uint32_t id : retired) retire_resource_record(id);

}

uint32_t VM::allocate_resource_address(std::size_t bytes) {
    // PE64 140088CE0 appends to each thread's +64 cursor; 140088C80
    // subtracts the retired record's length. These addresses are host-side
    // translations of those independent arenas. A process-wide monotonic
    // counter eventually collided with the translated code/data tag bits.
    collect_retired_resource_records();

    // Root code can grow when FF executes a mediated module. Reserve its
    // declared arena capacity, not just today's loaded bytes: otherwise an
    // older translated resource can alias newly appended root literals.
    // Original main-program capacity is 8 MiB (00498250); this is a host
    // address reservation only, not an eagerly allocated pixel/code buffer.
    uint64_t cursor = std::max<std::size_t>({1, memory_.code().size(),
        main_code_.size(), thread_.code_capacity});
    for (const auto& [id, state] : program_threads_) {
        cursor = std::max<uint64_t>(cursor, state.code.size());
        cursor = std::max<uint64_t>(cursor, state.thread.code_capacity);
    }
    for (const auto& frame : module_frames_)
        cursor = std::max<uint64_t>(cursor, frame.root_code_before.size());
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (const auto& [key, data] : resource_cache_) {
        const uint32_t id = resource_key_address(key);
        const auto segment = resource_segments_.find(id);
        const uint64_t size = segment == resource_segments_.end()
            ? data.size() : segment->second.code_size;
        ranges.emplace_back(id, static_cast<uint64_t>(id) + std::max<uint64_t>(1, size));
    }
    // Normal loads have matching cache/segment records. Reserve a segment
    // even if a restored or diagnostic state omitted its cache entry.
    for (const auto& [id, segment] : resource_segments_)
        ranges.emplace_back(id, static_cast<uint64_t>(id) +
            std::max<uint64_t>(1, segment.code_size));
    std::sort(ranges.begin(), ranges.end());
    const uint64_t length = std::max<std::size_t>(1, bytes);
    for (const auto& [begin, end] : ranges) {
        if (end <= cursor) continue;
        if (begin >= cursor && begin - cursor >= length) break;
        cursor = std::max(cursor, end);
    }
    if (cursor >= Memory::kCodeTag || length > Memory::kCodeTag - cursor)
        throw BpFault("loaded program host address space exhausted");
    next_resource_offset_ = static_cast<uint32_t>(cursor + length);
    return static_cast<uint32_t>(cursor);
}
} // namespace bgiplayer
