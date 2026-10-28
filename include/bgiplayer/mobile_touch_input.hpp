#pragma once
#include <cstdint>
#include <deque>
#include <vector>

namespace bgiplayer {
// Host gesture mapping only. Coordinates are already in the game's logical canvas.
// Deferring a stationary first finger prevents a later second finger from sending
// an irreversible left click. A drag starts as soon as its movement exceeds slop.
class MobileTouchInput {
public:
    enum class Phase { Down, Move, Up };
    struct Action {
        uint32_t descriptor; // zero: pointer motion; one/two: left/right button
        bool down;
        int32_t x, y;
        uint64_t gesture;
    };
    explicit MobileTouchInput(int32_t slop = 16, uint64_t chord_timeout_ms = 450);
    void finger(Phase phase, int64_t id, int32_t x, int32_t y, uint64_t now_ms);
    void expire(uint64_t now_ms);
    void cancel();
    // Each call represents one VM scheduler pass, with at most one button edge.
    std::vector<Action> next_frame();
private:
    struct Finger { int64_t id; int32_t x, y, origin_x, origin_y; };
    void cancel_gesture();
    void append(uint32_t descriptor, bool down, int32_t x, int32_t y);
    bool moved(const Finger& finger) const;
    std::vector<Finger> fingers_;
    std::deque<Action> actions_;
    int32_t slop_;
    uint64_t timeout_, started_ = 0, generation_ = 0, held_generation_ = 0;
    uint32_t held_ = 0;
    int32_t last_x_ = 0, last_y_ = 0, chord_x_ = 0, chord_y_ = 0;
    bool chord_ = false, cancelled_ = false, dragging_ = false;
};
} // namespace bgiplayer
