#include "bgiplayer/mobile_touch_input.hpp"
#include <algorithm>
#include <cstdlib>

namespace bgiplayer {
MobileTouchInput::MobileTouchInput(int32_t slop, uint64_t timeout)
    : slop_(std::max(1, slop)), timeout_(timeout) {}
bool MobileTouchInput::moved(const Finger& f) const {
    return std::llabs(int64_t(f.x) - f.origin_x) > slop_ ||
           std::llabs(int64_t(f.y) - f.origin_y) > slop_;
}
void MobileTouchInput::append(uint32_t descriptor, bool down, int32_t x, int32_t y) {
    Action a{descriptor, down, x, y, generation_};
    if (!descriptor && !actions_.empty() && !actions_.back().descriptor &&
        actions_.back().gesture == generation_) actions_.back() = a;
    else actions_.push_back(a);
}
void MobileTouchInput::cancel_gesture() {
    cancelled_ = true;
    std::erase_if(actions_, [&](const Action& a) { return a.gesture == generation_; });
    if (held_ && held_generation_ == generation_)
        append(held_, false, last_x_, last_y_);
}
void MobileTouchInput::cancel() {
    actions_.clear();
    fingers_.clear();
    cancelled_ = true;
    if (held_) actions_.push_back({held_, false, last_x_, last_y_, held_generation_});
}
void MobileTouchInput::expire(uint64_t now) {
    if (chord_ && !cancelled_ && now - started_ > timeout_) cancel_gesture();
}
void MobileTouchInput::finger(Phase phase, int64_t id, int32_t x, int32_t y, uint64_t now) {
    expire(now);
    auto it = std::find_if(fingers_.begin(), fingers_.end(),
        [&](const Finger& f) { return f.id == id; });
    if (phase == Phase::Down) {
        if (it != fingers_.end()) { cancel_gesture(); return; }
        if (fingers_.empty()) {
            ++generation_; started_ = now; chord_ = cancelled_ = dragging_ = false;
        }
        fingers_.push_back({id, x, y, x, y});
        if (fingers_.size() == 2) {
            chord_ = true;
            chord_x_ = int32_t((int64_t(fingers_[0].x) + x) / 2);
            chord_y_ = int32_t((int64_t(fingers_[0].y) + y) / 2);
            if (dragging_ || now - started_ > timeout_) cancel_gesture();
        } else if (fingers_.size() > 2) cancel_gesture();
        return;
    }
    if (it == fingers_.end()) return;
    it->x = x; it->y = y;
    if (!cancelled_) {
        if (chord_) {
            if (moved(*it)) cancel_gesture();
        } else if (phase == Phase::Move && (dragging_ || moved(*it))) {
            if (!dragging_) {
                append(1, true, it->origin_x, it->origin_y);
                dragging_ = true;
            }
            append(0, false, x, y);
        }
    }
    if (phase != Phase::Up) return;
    if (!cancelled_ && !chord_) {
        if (!dragging_) append(1, true, x, y);
        append(1, false, x, y);
    }
    fingers_.erase(it);
    if (fingers_.empty() && chord_ && !cancelled_) {
        append(2, true, chord_x_, chord_y_);
        append(2, false, chord_x_, chord_y_);
    }
}
std::vector<MobileTouchInput::Action> MobileTouchInput::next_frame() {
    std::vector<Action> result;
    while (!actions_.empty()) {
        Action a = actions_.front(); actions_.pop_front();
        result.push_back(a); last_x_ = a.x; last_y_ = a.y;
        if (a.descriptor) {
            held_ = a.down ? a.descriptor : 0;
            held_generation_ = a.gesture;
            break;
        }
    }
    return result;
}
} // namespace bgiplayer
