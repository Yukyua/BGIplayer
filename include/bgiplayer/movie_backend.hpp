#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace bgiplayer {

class MoviePlayback {
public:
    MoviePlayback();
    ~MoviePlayback();

    MoviePlayback(const MoviePlayback&) = delete;
    MoviePlayback& operator=(const MoviePlayback&) = delete;
    MoviePlayback(MoviePlayback&&) noexcept;
    MoviePlayback& operator=(MoviePlayback&&) noexcept;

    struct Statistics {
        uint64_t stream_id = 0;
        uint64_t decoded_frames = 0;
        uint64_t seeks = 0;
        bool hardware_frames = false;
    };
    [[nodiscard]] Statistics statistics() const;
    bool open(std::span<const uint8_t> encoded);
    [[nodiscard]] std::optional<int64_t> frame_at(int64_t position_ms);
    [[nodiscard]] int64_t duration_ms() const;
    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;
    [[nodiscard]] const std::vector<uint8_t>& rgba() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bgiplayer
