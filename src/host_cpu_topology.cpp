#include "bgiplayer/host_cpu_topology.hpp"
#include <bit>
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#endif
namespace bgiplayer {
uint32_t host_first_core_logical_processors(){
#if defined(_WIN32)
    DWORD bytes=0;GetLogicalProcessorInformation(nullptr,&bytes);
    std::vector<unsigned char> storage(bytes);
    if(!GetLogicalProcessorInformation(reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(storage.data()),&bytes))return 0;
    const auto* records=reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION*>(storage.data());
    for(std::size_t i=0;i<bytes/sizeof(*records);++i)
        if(records[i].Relationship==RelationProcessorCore)return std::popcount(records[i].ProcessorMask);
    return 0;
#elif defined(__linux__)
    std::ifstream file("/sys/devices/system/cpu/cpu0/topology/thread_siblings_list");
    std::string list;if(!(file>>list))return 0;
    try{
    uint32_t count=0;std::size_t begin=0;
    while(begin<list.size()){
        const auto end=list.find(',',begin);const std::string item=list.substr(begin,end-begin);
        const auto dash=item.find('-');const auto first=std::stoul(item.substr(0,dash));
        const auto last=dash==std::string::npos?first:std::stoul(item.substr(dash+1));
        if(last<first)throw std::runtime_error("invalid CPU sibling topology");
        count+=static_cast<uint32_t>(last-first+1);if(end==std::string::npos)break;begin=end+1;
    }return count;
    }catch(const std::exception&){return 0;}
#elif defined(__APPLE__)
    // Darwin exposes logical/physical counts for a CPU class, not Windows core
    // records. Apple CPU classes have uniform SMT width (ARM=1, Intel=1/2).
    uint32_t logical=0,physical=0;std::size_t bytes=sizeof(uint32_t);
    const int l=sysctlbyname("hw.logicalcpu_max",&logical,&bytes,nullptr,0);bytes=sizeof(uint32_t);
    const int p=sysctlbyname("hw.physicalcpu_max",&physical,&bytes,nullptr,0);
    if(l||p||!physical||logical%physical)return 0;
    return logical/physical;
#else
    return 0;
#endif
}
}
