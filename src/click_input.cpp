#include "bgiplayer/click_input.hpp"
#include "bgiplayer/common.hpp"
#include "bgiplayer/host_files.hpp"
#include "bgiplayer/vm.hpp"

#include <array>
#include <charconv>
#include <iostream>
#include <string_view>

namespace bgiplayer {
namespace {
constexpr std::string_view header = "elapsed_ms,source,button,action,x,y";
const char* source_name(ClickSource source) {
    switch (source) {
        case ClickSource::Mouse: return "mouse";
        case ClickSource::Touch: return "touch";
        case ClickSource::Injected: return "injected";
        case ClickSource::Replay: return "replay";
    }
    return "mouse";
}
template<class T> bool number(std::string_view text, T& output) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), output);
    return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool split(std::string_view line, std::array<std::string_view, 6>& fields) {
    for (std::size_t n = 0; n < fields.size(); ++n) {
        const auto comma = line.find(',');
        if (n == fields.size() - 1U) {
            if (comma != line.npos) return false;
            fields[n] = line;
        } else {
            if (comma == line.npos) return false;
            fields[n] = line.substr(0, comma);
            line.remove_prefix(comma + 1U);
        }
    }
    return true;
}
}

ClickInput::ClickInput(VM& vm, bool trace,
    const std::optional<std::filesystem::path>& record_path,
    const std::optional<std::filesystem::path>& replay_path)
    : vm_(vm), trace_(trace), replaying_(replay_path.has_value()) {
    if (record_path && replay_path) {
        // Reject aliases/hard links before truncating the recording file.
        std::error_code error;
        const bool same_file = std::filesystem::equivalent(*record_path, *replay_path, error);
        if (same_file || std::filesystem::weakly_canonical(*record_path) ==
                         std::filesystem::weakly_canonical(*replay_path))
            throw Error("click recording and replay must use different files");
    }
    if (replay_path) {
        auto input = host_files::open_read(*replay_path);
        if (!input) throw Error("cannot open click replay log");
        std::string line;
        const auto next_line = [&]() {
            if (!std::getline(input, line)) return false;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        };
        if (!next_line() || line != header) throw Error("invalid click replay CSV header");
        std::size_t line_number = 1;
        while (next_line()) {
            ++line_number;
            std::array<std::string_view, 6> fields;
            Event event{};
            const bool valid = split(line, fields) &&
                number(fields[0], event.elapsed_ms) && number(fields[4], event.x) &&
                number(fields[5], event.y) &&
                (fields[1] == "mouse" || fields[1] == "injected" || fields[1] == "replay" || fields[1] == "touch");
            if (!valid) throw Error("invalid click replay row " + std::to_string(line_number));
            if (fields[2] == "none" && fields[3] == "move") event.descriptor = 0;
            else if (fields[2] == "none" && fields[3] == "position") event.descriptor = 3;
            else if ((fields[2] == "left" || fields[2] == "right") &&
                     (fields[3] == "down" || fields[3] == "up")) {
                event.descriptor = fields[2] == "left" ? 1U : 2U;
                event.down = fields[3] == "down";
            } else throw Error("invalid click replay button/action at row " + std::to_string(line_number));
            if (!replay_.empty() && event.elapsed_ms < replay_.back().elapsed_ms)
                throw Error("click replay times must be nondecreasing");
            if (replay_.size() >= 1'000'000U) throw Error("click replay log exceeds one million events");
            replay_.push_back(event);
        }
        if (input.bad()) throw Error("cannot read click replay log");
        if (replay_.empty()) throw Error("click replay log contains no events");
    }
    if (record_path) {
        output_ = host_files::open_write(*record_path);
        if (!output_) throw Error("cannot open click recording file (parent directory must exist)");
        output_ << header << '\n';
        output_.flush();
        if (!output_) throw Error("cannot write click recording header");
    }
}

void ClickInput::start(uint64_t now_ms) { started_ms_ = now_ms; }

void ClickInput::record(uint64_t now_ms, ClickSource source, uint32_t descriptor,
                        bool down, int32_t x, int32_t y) {
    if (!trace_ && !output_.is_open()) return;
    const uint64_t elapsed = now_ms >= started_ms_ ? now_ms - started_ms_ : 0;
    const char* button_name = descriptor == 1 ? "left" : descriptor == 2 ? "right" : "none";
    const char* action = descriptor == 0 ? "move" : descriptor == 3 ? "position" : down ? "down" : "up";
    if (trace_ && (descriptor == 1 || descriptor == 2))
        std::cout << "MOUSE_CLICK elapsed_ms=" << elapsed << " source=" << source_name(source)
                  << " button=" << button_name << " action=" << action
                  << " x=" << x << " y=" << y << std::endl;
    if (output_.is_open()) {
        output_ << elapsed << ',' << source_name(source) << ',' << button_name << ','
                << action << ',' << x << ',' << y << '\n';
        output_.flush(); // Keep the last input when a later VM step faults.
        if (!output_) {
            std::cerr << "warning: click recording write failed; recording disabled\n";
            output_.close();
        }
    }
}

void ClickInput::motion(uint64_t now_ms, int32_t x, int32_t y, ClickSource source) {
    vm_.set_pointer_position(x, y);
    vm_.control_pointer_motion(x, y);
    vm_.queue_icon_motion_at(x, y);
    record(now_ms, source, 0, false, x, y);
}

void ClickInput::position(uint64_t now_ms, int32_t x, int32_t y, ClickSource source) {
    const bool changed = vm_.pointer_position() != std::pair<int32_t, int32_t>{x, y};
    vm_.set_pointer_position(x, y);
    vm_.control_pointer_motion(x, y);
    // Preserve SDL's sampled cursor state without inventing an icon-motion event.
    if (changed) record(now_ms, source, 3, false, x, y);
}

bool ClickInput::button(uint64_t now_ms, uint32_t descriptor, bool down,
                       int32_t x, int32_t y, ClickSource source) {
    if (descriptor != 1 && descriptor != 2) throw Error("click input accepts left/right buttons only");
    vm_.set_pointer_position(x, y);
    vm_.set_input_descriptor(descriptor, down ? 0x8000U : 0U);
    if (down) {
        vm_.queue_input_descriptor_event(descriptor, 1);
        vm_.enqueue_system_event_record(3, descriptor);
    }
    vm_.enqueue_system_pointer_record(x, y, down);
    const bool control_hit = descriptor == 1 && vm_.control_pointer_button(x, y, down);
    const bool hit = control_hit || vm_.queue_icon_input_at(x, y, down);
    record(now_ms, source, descriptor, down, x, y);
    return hit;
}

bool ClickInput::replay_due(uint64_t now_ms) {
    const uint64_t elapsed = now_ms >= started_ms_ ? now_ms - started_ms_ : 0;
    bool delivered = false;
    while (next_ < replay_.size() && replay_[next_].elapsed_ms <= elapsed) {
        const auto& event = replay_[next_++];
        if (event.descriptor == 0) motion(now_ms, event.x, event.y, ClickSource::Replay);
        else if (event.descriptor == 3) position(now_ms, event.x, event.y, ClickSource::Replay);
        else button(now_ms, event.descriptor, event.down, event.x, event.y, ClickSource::Replay);
        delivered = true;
    }
    return delivered;
}
} // namespace bgiplayer
