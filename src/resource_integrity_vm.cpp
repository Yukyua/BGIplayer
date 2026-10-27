#include "bgiplayer/vm.hpp"
namespace bgiplayer {
void VM::install_resource_integrity_wait(Thread& thread,ResourceIntegrityData data) {
    const uint32_t index=next_procedure_id_++;
    resource_integrity_procedures_.emplace(index,std::move(data));
    thread.snapshot_wait_kind=SnapshotWaitKind::ResourceIntegrity;
    thread.snapshot_wait_index=index;
    thread.wait_procedure=[this,index]{return advance_resource_integrity_procedure(index);};
    thread.status_flags|=1U;
    thread.state=ThreadState::Waiting;
}
WaitProcedureResult VM::advance_resource_integrity_procedure(uint32_t index) {
    const auto found=resource_integrity_procedures_.find(index);
    if(found==resource_integrity_procedures_.end())throw BpFault("resource integrity wait request is missing");
    const uint32_t result=check_resource_integrity(found->second);
    resource_integrity_procedures_.erase(found);
    return {1,{result}};
}
}
