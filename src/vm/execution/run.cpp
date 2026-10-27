// Budgeted host stepping; nonzero native results enter the original flat pass.
namespace bgiplayer {
VM::StepResult VM::run(std::size_t instruction_limit, std::size_t* executed) {
    std::size_t count = 0;
    if (engine_exit_requested_) {
        if (executed != nullptr) *executed = 0;
        return StepResult::Done;
    }
    const auto state_result = [this]() {
        if (engine_exit_requested_) return StepResult::Done;
        if (thread_.state == ThreadState::Faulted) return StepResult::Fault;
        if (thread_.state == ThreadState::Done && !has_live_scheduled_threads()) {
            return StepResult::Done;
        }
        return StepResult::Yield;
    };
    while (count < instruction_limit) {
        if (thread_.state != ThreadState::Running ||
            (thread_.status_flags & 0x80000001U) != 0 ||
            (exclusive_thread_id_ != 0 && exclusive_thread_id_ != thread_.id)) {
            if (thread_.state != ThreadState::Faulted && has_live_scheduled_threads()) {
                run_scheduled_children();
            }
            if (executed != nullptr) *executed = count;
            return state_result();
        }
        const StepResult result = step();
        ++count;
        if (result == StepResult::Continue && main_slice_dispatch_count_ >= 0x100000U) {
            thread_.scheduler_signal = NativeSignal::NextThread;
            run_scheduled_children();
            if (executed != nullptr) *executed = count;
            return state_result();
        }
        if (result != StepResult::Continue) {
            if (result == StepResult::Yield ||
                (result == StepResult::Done && has_live_scheduled_threads())) {
                run_scheduled_children();
            }
            if (executed != nullptr) *executed = count;
            if (engine_exit_requested_) return StepResult::Done;
            return thread_.state == ThreadState::Faulted ? StepResult::Fault
                : (result == StepResult::Done ? state_result() : result);
        }
    }
    if (executed != nullptr) *executed = count;
    return StepResult::Yield;
}
}  // namespace bgiplayer
