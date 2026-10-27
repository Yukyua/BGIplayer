#include "bgiplayer/host_dialogs.hpp"

namespace bgiplayer {

std::optional<uint32_t> HostDialogs::create(uint32_t parameter) {
    const uint32_t id = next_id_++;
    Dialog dialog;
    dialog.parameter = parameter;
    dialogs_.emplace(id, std::move(dialog));
    return id;
}

bool HostDialogs::destroy(uint32_t id) {
    return dialogs_.erase(id) != 0;
}

bool HostDialogs::set_visible(uint32_t id, bool visible) {
    const auto found = dialogs_.find(id);
    if (found == dialogs_.end() || found->second.visible == visible) {
        return false;
    }
    found->second.visible = visible;
    return true;
}

HostDialogs::DequeueResult HostDialogs::dequeue(
    uint32_t id, uint32_t& first, uint32_t& second) {
    const auto found = dialogs_.find(id);
    if (found == dialogs_.end()) {
        return DequeueResult::Unknown;
    }
    if (found->second.messages.empty()) {
        return DequeueResult::Empty;
    }
    first = found->second.messages.front().first;
    second = found->second.messages.front().second;
    found->second.messages.pop_front();
    return DequeueResult::Success;
}

void HostDialogs::enqueue(uint32_t id, uint32_t first, uint32_t second) {
    const auto found = dialogs_.find(id);
    if (found != dialogs_.end()) {
        found->second.messages.emplace_back(first, second);
    }
}

}  // namespace bgiplayer
