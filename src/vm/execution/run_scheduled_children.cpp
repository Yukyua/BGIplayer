// Original 0x00498780: forward-chain scheduling, independent of ownership.
namespace bgiplayer {
bool VM::run_scheduled_children() {
    if (root_children_.empty()) {
        return false;
    }
    uint32_t prior_main_dispatches = main_slice_dispatch_count_;
    main_slice_dispatch_count_ = 0;
    const uint32_t main_id = thread_.id;
    const NativeSignal pending = thread_.scheduler_signal;
    thread_.scheduler_signal = NativeSignal::Continue;
    scheduler_saved_thread_ = thread_;
    bool main_has_resource = has_active_code_resource_;
    uint32_t main_resource_base = active_code_resource_base_;
    bool executed = false;
    bool faulted = false;
    bool suppress_execution = false;
    uint32_t current = next_thread_in_tree(1);

    // The host has already executed the main slice before entering here.
    // Consume its nonzero dispatch result using the same branches as workers.
    switch (pending) {
        case NativeSignal::Yield:
        case NativeSignal::NextThread:
            current = next_thread_in_tree(main_id);
            break;
        case NativeSignal::Switch:
            current = has_thread_object(switch_target_id_)
                ? switch_target_id_ : 0U;
            switch_target_id_ = 0;
            break;
        case NativeSignal::Pause:
            scheduler_saved_thread_->status_flags |= 0x80000000U;
            current = main_id;
            break;
        case NativeSignal::EndPass:
            suppress_execution = true;
            current = next_thread_in_tree(main_id);
            break;
        case NativeSignal::ResumeCurrent:
            current = main_id;
            break;
        case NativeSignal::Continue:
            break;
    }

    const auto select_code = [this](const std::vector<uint8_t>& code,
                                    bool has_resource, uint32_t base) {
        memory_.set_code(code);
        memory_.set_active_code({});
        has_active_code_resource_ = has_resource;
        active_code_resource_base_ = base;
        if (has_resource) {
            const auto active = resource_segments_.find(base);
            if (active == resource_segments_.end()) {
                std::ostringstream diagnostic;
                diagnostic << "scheduled code resource is missing thread="
                           << thread_.id << " name=" << thread_.name
                           << " base=0x" << std::hex << base
                           << " ip=0x" << thread_.ip
                           << " root_code=" << std::dec << code.size()
                           << " loaded=" << resource_segments_.size()
                           << " order=";
                if (const auto owner = resource_orders_by_thread_.find(thread_.id);
                    owner != resource_orders_by_thread_.end()) {
                    for (uint32_t id : owner->second)
                        diagnostic << "0x" << std::hex << id << ',';
                }
                throw BpFault(diagnostic.str());
            }
            memory_.set_active_code(
                std::span<const uint8_t>(active->second.bytes).subspan(
                    active->second.code_offset, active->second.code_size), base);
        }
        sync_code_fallbacks();
    };

    while (current != 0) {
        if (current == 1) {
            last_error_ = "scheduler root container has no executable bytecode";
            faulted = true;
            break;
        }
        if (exclusive_thread_id_ != 0 && current != exclusive_thread_id_) {
            current = next_thread_in_tree(current);
            continue;
        }
        const bool is_main = current == main_id;
        auto found = program_threads_.find(current);
        if (!is_main && found == program_threads_.end()) {
            break;
        }
        // 0045B390 retains the allocator's data backing (+0x4C).
        // Follow borrowers to the loaded owner whose full data arena stores it.
        ProgramThreadState* frame_owner = nullptr;
        if (!is_main) {
            uint32_t backing = current;
            for (;;) {
                auto storage = program_threads_.find(backing);
                if (storage == program_threads_.end()) break;
                if (storage->second.allocation_owner_id == 0) {
                    frame_owner = &storage->second;
                    break;
                }
                backing = storage->second.allocation_owner_id;
                if (backing == main_id) break;
            }
        }
        if (is_main) {
            thread_ = *scheduler_saved_thread_;
            active_child_handle_ = 0;
            select_code(main_code_, main_has_resource, main_resource_base);
        } else {
            thread_ = found->second.thread;
            active_child_handle_ = current;
            if (frame_owner != nullptr) memory_.swap_thread_frame(frame_owner->frame);
            select_code(found->second.code, found->second.has_code_resource,
                        found->second.code_resource_base);
        }
        thread_id_ = current;
        bool cleanup = false;
        bool advance = true;
        uint32_t explicit_next = 0;
        NativeSignal signal = NativeSignal::Continue;

        if ((thread_.status_flags & 0x80000000U) != 0) {
            cleanup = thread_.scheduler_marker == 0;
        } else {
            bool runnable = true;
            if ((thread_.status_flags & 1U) != 0) {
                const int wait_result = evaluate_wait_procedure(thread_);
                executed |= wait_result != 0;
                if (wait_result == -1) {
                    suppress_execution = true;
                }
                runnable = wait_result != 0 && wait_result != -1;
            }
            // Any nonzero wait result except -1 reaches bytecode. Only
            // 1/-1 dispose the procedure, so result 2 may retain bit 0.
            if (runnable && !suppress_execution &&
                thread_.state == ThreadState::Waiting &&
                (thread_.status_flags & 1U) != 0) {
                thread_.state = ThreadState::Running;
            }
            if (runnable && !suppress_execution &&
                thread_.state == ThreadState::Running) {
                thread_.scheduler_signal = NativeSignal::Continue;
                const std::size_t budget = is_main && pending == NativeSignal::Continue
                    ? 0x100000U - std::min(prior_main_dispatches, 0x100000U)
                    : 0x100000U;
                if (is_main) prior_main_dispatches = 0;
                for (std::size_t count = 0; count < budget; ++count) {
                    const StepResult result = step();
                    executed = true;
                    signal = thread_.scheduler_signal;
                    thread_.scheduler_signal = NativeSignal::Continue;
                    if (result == StepResult::Fault) {
                        faulted = true;
                        break;
                    }
                    if (result != StepResult::Continue) {
                        break;
                    }
                }
                switch (signal) {
                    case NativeSignal::ResumeCurrent:
                        advance = false;
                        explicit_next = current;
                        break;
                    case NativeSignal::Switch:
                        advance = false;
                        explicit_next = has_thread_object(switch_target_id_)
                            ? switch_target_id_ : 0U;
                        switch_target_id_ = 0;
                        break;
                    case NativeSignal::Pause:
                        thread_.status_flags |= 0x80000000U;
                        // Result 4 re-enters the status check before advancing.
                        advance = false;
                        explicit_next = current;
                        break;
                    case NativeSignal::EndPass:
                        suppress_execution = true;
                        break;
                    case NativeSignal::Continue:
                    case NativeSignal::Yield:
                    case NativeSignal::NextThread:
                        // Includes the instruction-cap path. Null next ends
                        // the slice; it never resumes the last thread.
                        break;
                }
            }
        }

        // Select successor before unlink/destruction, as 0044BD40 requires.
        const uint32_t next = advance
            ? next_thread_in_tree(current) : explicit_next;
        if (is_main) {
            *scheduler_saved_thread_ = thread_;
            main_has_resource = has_active_code_resource_;
            main_resource_base = active_code_resource_base_;
        } else {
            if (frame_owner != nullptr) memory_.swap_thread_frame(frame_owner->frame);
            found->second.thread = thread_;
            found->second.has_code_resource = has_active_code_resource_;
            found->second.code_resource_base = active_code_resource_base_;
        }
        active_child_handle_ = 0;
        thread_ = *scheduler_saved_thread_;
        thread_id_ = main_id;
        if (cleanup) {
            if (is_main) {
                std::erase(root_children_, main_id);
                std::erase(root_container_thread_.children, main_id);
                scheduler_saved_thread_->state = ThreadState::Done;
            } else {
                remove_thread_subtree(current);
                // Cleanup can release shared ranges in the saved main owner.
                *scheduler_saved_thread_ = thread_;
            }
        }
        if (faulted) {
            break;
        }
        current = next;
    }
    if (faulted) {
        scheduler_saved_thread_->state = ThreadState::Faulted;
    }
    thread_ = *scheduler_saved_thread_;
    thread_id_ = main_id;
    active_child_handle_ = 0;
    select_code(main_code_, main_has_resource, main_resource_base);
    scheduler_saved_thread_.reset();
    collect_retired_resource_records();
    return executed;
}
}  // namespace bgiplayer
