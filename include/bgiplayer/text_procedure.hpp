#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bgiplayer {
// Mutable fields of original 100-byte layout node consumed by 0043E070.
struct TextProcedureNode {
    uint32_t complete = 0;
    uint32_t delay = 0;
    uint32_t elapsed = 0;
    uint32_t duration = 0;
    int32_t x = 0;
    int32_t y = 0;
    uint32_t kind = 0;
};
struct TextProcedureTick {
    bool force_completion;
    bool elapsed_time_mode; // 005331D8 != 0
    bool redraw_whole_plane; // 00533210 != 0
    bool flush_pending_event; // procedure+110
    uint32_t counter_steps; // 00436DC0/00436D90 loop count
    uint32_t elapsed_time; // 00436DB0
    uint32_t current_fade_duration; // 00529BEC
};
enum class TextProcedureActionKind {
    FlushEvent, ClearWholePlane, ClearNode, BlendNode, DirtyNode,
    DirtyWholePlane, EventNode, ResetClock
};
struct TextProcedureAction {
    TextProcedureActionKind kind;
    std::size_t node;
    uint32_t parameter;
};
// Original 0043E070 ordering, with clock/input and bitmap APIs explicit at boundary.
// BlendNode parameter is original mode1 transparency (0 opaque,256 transparent).
std::vector<TextProcedureAction> tick_text_procedure(
    std::vector<TextProcedureNode>& nodes,const TextProcedureTick& tick);
bool text_procedure_nodes_complete(const std::vector<TextProcedureNode>& nodes);
} // namespace bgiplayer
