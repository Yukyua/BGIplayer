// Original 0x0044BD40 unlinks and destroys one forward-chain node.
namespace bgiplayer {
void VM::remove_thread_subtree(uint32_t id) {
    const auto found = program_threads_.find(id);
    if (found == program_threads_.end()) {
        return;
    }
    const ProgramThreadState& removed = found->second;
    if (Thread* allocator = find_thread_object(removed.allocation_owner_id);
        allocator != nullptr) {
        release_thread_range(allocator->code_allocations,
                             removed.code_offset, removed.reserved_code_bytes);
        release_thread_range(allocator->data_allocations,
                             removed.data_offset, removed.reserved_data_bytes);
        if (removed.allocation_owner_id != 0 && allocator->scheduler_marker != 0) {
            --allocator->scheduler_marker;
        }
    }
    if (Thread* owner = find_thread_object(removed.thread.parent_id);
        owner != nullptr) {
        std::erase(owner->children, id);
    }
    std::erase(root_children_, id);
    program_threads_.erase(found);
    release_thread_resources(id);
}
}  // namespace bgiplayer
