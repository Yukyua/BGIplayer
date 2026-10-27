#include "bgiplayer/vm.hpp"
#include <sstream>

namespace bgiplayer {
namespace {
// Host execution guard only: there is no suspended asynchronous VM state here.
// The original shared worker-pool callback/data slots are not reentrant.
thread_local std::vector<const VM*> active_parallel_vms;
struct ParallelDispatchGuard {
    explicit ParallelDispatchGuard(const VM* vm){
        if(std::find(active_parallel_vms.begin(),active_parallel_vms.end(),vm)!=active_parallel_vms.end())
            throw BpFault("Sys1:0x48 shared worker pool reentry is unsupported");
        active_parallel_vms.push_back(vm);
    }
    ~ParallelDispatchGuard(){active_parallel_vms.pop_back();}
};
}

// Original 0046FB20 / 1400B7090: synchronous worker group, not scheduler spawn.
void VM::dispatch_parallel_script(uint32_t operand_slots, uint32_t code_bytes,
                                  uint32_t data_bytes, uint32_t entry_ip) {
    uint32_t allocator_id=thread_.id;
    for(;;){const auto found=program_threads_.find(allocator_id);
        if(found==program_threads_.end()||!found->second.allocation_owner_id)break;
        allocator_id=found->second.allocation_owner_id;
    }
    const Thread* allocator=find_thread_object(allocator_id);
    const auto allocator_record=program_threads_.find(allocator_id);
    const uint32_t source_base=allocator_record==program_threads_.end()?0:allocator_record->second.code_offset;
    if(!allocator||static_cast<uint64_t>(source_base)+allocator->code_capacity<=entry_ip)
        throw BpFault("Sys1:0x48 entry point outside code block status=0x80000003");
    if(parallel_worker_count_==0)return; // Core FFFFFFFF is ignored by both native wrappers.
    const ParallelDispatchGuard dispatch_guard(this);

    const uint32_t caller_id=thread_.id;
    const uint32_t saved_thread_id=thread_id_;
    const uint32_t saved_child=active_child_handle_;
    const uint32_t saved_opcode=current_opcode_ip_;
    const uint8_t saved_native=current_native_sub_;
    const uint32_t saved_dispatches=main_slice_dispatch_count_;
    const auto saved_scheduler=scheduler_saved_thread_;
    const bool caller_is_main=caller_id==2;
    const bool saved_has_resource=has_active_code_resource_;
    const uint32_t saved_resource=active_code_resource_base_;
    const std::vector<uint8_t> saved_code(memory_.code().begin(),memory_.code().end());
    const std::string saved_error=last_error_;
    // Publish the suspended caller so source arena reservations remain owned and
    // visible while thread_ temporarily holds a worker (including nested groups).
    const auto publish=[&]{
        if(caller_is_main)scheduler_saved_thread_=thread_;
        else program_threads_.at(caller_id).thread=thread_;
    };
    const auto suspended_caller=[&]() -> Thread& {
        return caller_is_main?*scheduler_saved_thread_:program_threads_.at(caller_id).thread;
    };
    publish();
    if(!scheduler_saved_thread_)scheduler_saved_thread_=thread_; // isolate dispatch accounting
    std::vector<uint32_t> workers;
    std::string failure;
    std::vector<std::string> diagnostics;
    const auto restore=[&]{
        thread_=suspended_caller();thread_id_=saved_thread_id;active_child_handle_=saved_child;
        memory_.set_code(saved_code);memory_.set_active_code({});
        has_active_code_resource_=saved_has_resource;active_code_resource_base_=saved_resource;
        if(saved_has_resource){
            const auto& segment=resource_segments_.at(saved_resource);
            memory_.set_active_code(std::span<const uint8_t>(segment.bytes).subspan(segment.code_offset,segment.code_size),saved_resource);
        }
        sync_code_fallbacks();current_opcode_ip_=saved_opcode;current_native_sub_=saved_native;
        main_slice_dispatch_count_=saved_dispatches;last_error_=saved_error;
    };
    try {
        for(uint32_t i=0;i<parallel_worker_count_;++i){
            const auto result=create_shared_program_thread_from(caller_id,operand_slots,code_bytes,data_bytes,entry_ip);
            publish();
            if(result.status!=0){
                // 0044C51F..52E / 140089265..274 consume the constructor ID
                // before initialization. The shared-thread helper allocates an
                // ID only on success; account for the destroyed failed object
                // here, without changing Sys1:44 or inventing occupied-ID skips.
                if(result.status==2||result.status==3)++next_thread_handle_;
                failure=result.status==2?"code allocation failed status=0x80000001":
                    result.status==3?"data allocation failed status=0x80000002":"worker entry point rejected status=0x80000003";
                break;
            }
            workers.push_back(result.handle);
        }
        if(!workers.empty())failure.clear(); // Original discards a later allocation failure once any worker exists.
        // A one-instruction host scheduling quantum permits shared-memory
        // communication. No original OS worker ordering or CPU speed is promised.
        diagnostics.resize(workers.size());
        std::vector<uint32_t> dispatched(workers.size());
        std::vector<bool> finished(workers.size());
        std::size_t live=workers.size();
        std::vector<uint8_t> idle_code;
        memory_.swap_code(idle_code);
        while(live){
            for(std::size_t index=0;index<workers.size();++index){
                if(finished[index])continue;
                auto& worker=program_threads_.at(workers[index]);
                thread_=worker.thread;thread_id_=thread_.id;active_child_handle_=thread_id_;
                memory_.swap_code(worker.code);memory_.set_active_code({});
                has_active_code_resource_=worker.has_code_resource;active_code_resource_base_=worker.code_resource_base;
                if(worker.has_code_resource){
                    const auto& segment=resource_segments_.at(worker.code_resource_base);
                    memory_.set_active_code(std::span<const uint8_t>(segment.bytes).subspan(segment.code_offset,segment.code_size),worker.code_resource_base);
                }
                sync_code_fallbacks();last_error_.clear();
                const StepResult result=step();
                ++dispatched[index];
                worker.thread=thread_;
                worker.has_code_resource=has_active_code_resource_;
                worker.code_resource_base=active_code_resource_base_;
                memory_.swap_code(worker.code);
                const bool stop=result!=StepResult::Continue||thread_.scheduler_signal!=NativeSignal::Continue||
                    dispatched[index]==0x10000000U;
                if(!stop)continue;
                finished[index]=true;--live;
                if(thread_.scheduler_signal!=NativeSignal::Pause||result==StepResult::Fault||!last_error_.empty()){
                    std::ostringstream out;out<<"worker="<<index<<" signal="<<static_cast<unsigned>(thread_.scheduler_signal)
                        <<" steps="<<dispatched[index];
                    if(!last_error_.empty())out<<" error="<<last_error_;
                    diagnostics[index]=out.str();
                }
            }
        }
    }catch(const std::exception& error){
        if(!failure.empty())failure+="; ";failure+=error.what();
    }
    restore();
    for(std::size_t i=0;i<workers.size();++i){
        if(i<diagnostics.size()&&!diagnostics[i].empty()){
            try{(void)show_message_box("Parallel script "+diagnostics[i],0x1010U);}
            catch(const std::exception& error){if(!failure.empty())failure+="; ";failure+=error.what();}
        }
        remove_thread_subtree(workers[i]);
    }
    // Destruction releases the caller's reservations and ownership links.
    publish();
    if(!saved_scheduler)scheduler_saved_thread_.reset();
    if(!failure.empty())throw BpFault("Sys1:0x48 "+failure);
}
}
