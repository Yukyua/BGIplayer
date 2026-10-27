#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <utility>

namespace bgiplayer {

class HostDialogs {
    friend struct SnapshotCodec;
public:
    enum class DequeueResult { Success, Empty, Unknown };

    std::optional<uint32_t> create(uint32_t parameter);
    bool destroy(uint32_t id);
    bool set_visible(uint32_t id, bool visible);
    DequeueResult dequeue(uint32_t id, uint32_t& first, uint32_t& second);
    void enqueue(uint32_t id, uint32_t first, uint32_t second);

private:
    struct Dialog {
        bool visible = false;
        uint32_t parameter = 0;
        std::deque<std::pair<uint32_t, uint32_t>> messages;
    };

    uint32_t next_id_ = 1;
    std::map<uint32_t, Dialog> dialogs_;
};

}  // namespace bgiplayer
