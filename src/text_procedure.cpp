#include "bgiplayer/text_procedure.hpp"
#include <algorithm>

namespace bgiplayer {
bool text_procedure_nodes_complete(const std::vector<TextProcedureNode>& nodes) {
    return std::all_of(nodes.begin(),nodes.end(),[](const auto& node){return node.complete!=0;});
}
std::vector<TextProcedureAction> tick_text_procedure(
    std::vector<TextProcedureNode>& nodes,const TextProcedureTick& tick) {
    std::vector<TextProcedureAction> actions;
    const auto action=[&](TextProcedureActionKind kind,std::size_t node=0,uint32_t value=0) {
        actions.push_back({kind,node,value});
    };
    if(tick.flush_pending_event)action(TextProcedureActionKind::FlushEvent);
    if(!tick.elapsed_time_mode && !tick.redraw_whole_plane) {
        uint32_t remaining=tick.counter_steps;
        while(remaining!=0 || tick.force_completion) {
            --remaining; // Original wraps when forced with zero accumulated steps.
            for(std::size_t i=0;i<nodes.size();++i) {
                auto& node=nodes[i];
                if(node.complete!=0)continue;
                if(node.delay!=0 && !tick.force_completion) {
                    --node.delay;
                    if(node.delay==0) {node.elapsed=0;node.duration=tick.current_fade_duration;}
                    continue;
                }
                const bool final=node.duration<=node.elapsed || tick.force_completion;
                if(!final && remaining!=0) {++node.elapsed;continue;}
                action(TextProcedureActionKind::ClearNode,i,1);
                action(TextProcedureActionKind::BlendNode,i,
                       final?0U:0x100U-((node.elapsed<<8U)/node.duration));
                action(TextProcedureActionKind::DirtyNode,i);
                if(final)node.complete=1;
                else ++node.elapsed;
            }
            if(tick.force_completion)break;
        }
        action(TextProcedureActionKind::ResetClock);
        return actions;
    }
    if(tick.redraw_whole_plane) {
        action(TextProcedureActionKind::ClearWholePlane);
        action(TextProcedureActionKind::DirtyWholePlane);
    }
    for(std::size_t i=0;i<nodes.size();++i) {
        auto& node=nodes[i];
        if(tick.elapsed_time<node.delay && !tick.force_completion)continue;
        if(node.kind==0x80000000U) {
            if(node.complete==0) {action(TextProcedureActionKind::EventNode,i);node.complete=1;}
            continue;
        }
        if(node.complete!=0 && !tick.redraw_whole_plane)continue;
        node.elapsed=tick.elapsed_time-node.delay;
        const bool final=node.duration<=node.elapsed || tick.force_completion;
        if(!tick.redraw_whole_plane)action(TextProcedureActionKind::ClearNode,i,1);
        action(TextProcedureActionKind::BlendNode,i,
               final?0U:0x100U-((node.elapsed<<8U)/node.duration));
        if(!tick.redraw_whole_plane)action(TextProcedureActionKind::DirtyNode,i);
        if(final)node.complete=1;
    }
    return actions;
}
} // namespace bgiplayer
