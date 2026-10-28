#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

namespace bgiplayer {
class VM;

enum class ClickSource { Mouse, Injected, Replay, Touch };

// Host diagnostics only: recorded times start at the backend input loop.
// VM queues and waits retain their existing implementations and serialization.
class ClickInput {
public:
    ClickInput(VM& vm, bool trace,
               const std::optional<std::filesystem::path>& record,
               const std::optional<std::filesystem::path>& replay);
    void start(uint64_t now_ms);
    bool replaying() const { return replaying_; }
    void position(uint64_t now_ms, int32_t x, int32_t y, ClickSource source);
    void motion(uint64_t now_ms, int32_t x, int32_t y, ClickSource source);
    bool button(uint64_t now_ms, uint32_t descriptor, bool down,
                int32_t x, int32_t y, ClickSource source);
    bool replay_due(uint64_t now_ms);

private:
    struct Event {
        uint64_t elapsed_ms;
        uint32_t descriptor; // 0: motion, 1: left, 2: right, 3: polled position
        bool down;
        int32_t x;
        int32_t y;
    };
    void record(uint64_t now_ms, ClickSource source, uint32_t descriptor,
                bool down, int32_t x, int32_t y);
    VM& vm_;
    bool trace_;
    bool replaying_;
    uint64_t started_ms_ = 0;
    std::ofstream output_;
    std::vector<Event> replay_;
    std::size_t next_ = 0;
};
} // namespace bgiplayer
