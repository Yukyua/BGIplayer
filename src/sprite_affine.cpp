#include "bgiplayer/vm.hpp"
#include "bgiplayer/display_easing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace bgiplayer {
namespace {
// 0041FB50: perspective correction; all arithmetic uses the original signed
// divisor and unsigned Q16 scale result.
uint32_t sprite_perspective_scale(int32_t depth, uint32_t perspective) {
    if (depth == 0 || perspective == 0) return 0x10000U;
    if (depth >= 0) {
        const int64_t denominator = (static_cast<int64_t>(perspective) << 16) + depth;
        return static_cast<uint32_t>((static_cast<uint64_t>(perspective) << 32) /
                                     static_cast<uint64_t>(denominator));
    }
    return static_cast<uint32_t>(((static_cast<int64_t>(perspective) << 16) - depth) /
                                 perspective);
}
int32_t original_truncate(double value) {
    if (!std::isfinite(value) || value < -2147483648.0 || value >= 2147483648.0)
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(value);
}
int32_t round_sprite_bound(double value, bool upper) {
    // 0042E6E2..0042E76C rounds only the retained Q16 fractional bits.
    // ceil/floor would incorrectly expand a quarter turn by a pixel.
    int32_t result = original_truncate(value);
    if ((!upper && value < 0.0) || (upper && value >= 0.0)) {
        const uint32_t fraction = static_cast<uint32_t>(original_truncate(value * 65536.0)) & 0xFFFFU;
        const int32_t correction = static_cast<int32_t>((fraction + 0xFFFFU) >> 16U);
        result += upper ? correction : -correction;
    }
    return result;
}
struct AffineBounds { uint32_t width, height; int32_t x, y; };
// Direct scalar translation of 0042E390. Its source rectangle has inclusive
// endpoints and an upward Y axis. The two final subpixel corrections retain
// only the low word of the signed Q16 products, as the original MOVZX does.
AffineBounds sprite_affine_bounds(uint32_t width, uint32_t height, uint32_t stretch,
    int32_t angle, uint32_t sx, uint32_t sy, int32_t left, int32_t top,
    int32_t origin_x, int32_t origin_y) {
    const double expanded = static_cast<double>((stretch + 0x10000U) * width) / 65536.0;
    const double ox = (expanded - width) * 0.5 + origin_x / 65536.0;
    const double oy = origin_y / 65536.0;
    const double radians = static_cast<double>(angle) * 3.141592653589793 / 11796480.0;
    const double sine = std::sin(radians), cosine = std::cos(radians);
    double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
    for (double x : {-ox, expanded - ox - 1.0}) {
        for (double y : {oy, oy - height + 1.0}) {
            const double scaled_x = x * (sx / 65536.0);
            const double scaled_y = y * (sy / 65536.0);
            const double px = scaled_x * cosine - scaled_y * sine;
            const double py = scaled_y * cosine + scaled_x * sine;
            min_x = std::min(min_x, px); max_x = std::max(max_x, px);
            min_y = std::min(min_y, py); max_y = std::max(max_y, py);
        }
    }
    const auto fraction = [](uint32_t scale, int32_t position) {
        return static_cast<double>(static_cast<uint32_t>(
            (static_cast<int64_t>(scale) * position) >> 16) & 0xFFFFU) / 65536.0;
    };
    max_x += fraction(sx, left);
    min_y -= fraction(sy, top);
    const int32_t lx = round_sprite_bound(min_x, false);
    const int32_t rx = round_sprite_bound(max_x, false);
    const int32_t by = round_sprite_bound(min_y, true);
    const int32_t ty = round_sprite_bound(max_y, true);
    return {static_cast<uint32_t>(rx - lx + 1), static_cast<uint32_t>(ty - by + 1), -lx, ty};
}
}

void VM::initialize_affine_sprite_mode5(uint32_t id, std::span<const uint32_t> p) {
    // 0042C180 wrapper followed by 0042CAC0. A failed source lookup leaves
    // the previous raster unchanged; the wrapper still resets modulation.
    for (uint32_t offset : {0x2C4U, 0x2C8U, 0x2CCU, 0x2DCU, 0x2E0U, 0x2E4U, 0x2E8U})
        object_raw_fields_[{id, offset}] = 0;
    set_object_absolute_position(id, p[1], p[2], p[3]);
    object_raw_fields_[{id, 0xB0U}] = p[14];
    set_raw_field_recursive(id, 0xB4U, p[15]);
    set_object_priority_bias(id, p[16]);
    const auto first = graphics_surfaces_.find(p[4]);
    const auto second = graphics_surfaces_.find(p[5]);
    if (first == graphics_surfaces_.end() ||
        (p[5] != 0xFFFFFFFFU && (second == graphics_surfaces_.end() ||
         second->second.width != first->second.width || second->second.height != first->second.height ||
         second->second.format != first->second.format))) return;
    object_raw_fields_[{id, 0x144U}] = 5;
    object_raw_fields_[{id, 0x160U}] = p[4];
    object_raw_fields_[{id, 0x164U}] = p[5];
    // 0042CAC0 / PE64 140064BEA..140064C07 retain allocation identities.
    object_raw_fields_[{id, 0x168U}] = graphics_bitmap_token(p[4]);
    object_raw_fields_[{id, 0x16CU}] = p[5] == 0xFFFFFFFFU
        ? 0U : graphics_bitmap_token(p[5]);
    object_raw_fields_[{id, 0x250U}] = p[5] == 0xFFFFFFFFU ? 0 : p[6];
    object_raw_fields_[{id, 0x248U}] = object_raw_field(id, 0x250U);
    object_raw_fields_[{id, 0x254U}] = p[5] == 0xFFFFFFFFU ? 0xFFFFFFFFU : p[7];
    object_raw_fields_[{id, 0x258U}] = p[8] << 16U;
    object_raw_fields_[{id, 0x25CU}] = p[9] << 16U;
    object_raw_fields_[{id, 0x260U}] = p[10];
    object_raw_fields_[{id, 0x27CU}] = 0x10000U;
    object_raw_fields_[{id, 0x280U}] = 0x10000U;
    object_raw_fields_[{id, 0x284U}] = 0;
    object_raw_fields_[{id, 0x288U}] = p[11];
    object_raw_fields_[{id, 0x294U}] = p[12];
    object_raw_fields_[{id, 0x298U}] = p[13];
    sprites_[id].bitmap = p[4];
    sprites_[id].blend_mode = p[14];
    sprites_[id].alpha = p[15];
    sprites_[id].configured = true;
    refresh_affine_sprite_mode5(id);
    object_raw_fields_[{id, 0x274U}] = object_raw_field(id, 0x2B4U);
    object_raw_fields_[{id, 0x278U}] = object_raw_field(id, 0x2B8U);
}

void VM::refresh_affine_sprite_mode5(uint32_t id) {
    // 0042EA50 exits without modifying geometry when source160 is absent.
    const auto source = graphics_surfaces_.find(object_raw_field(id, 0x160U));
    if (source == graphics_surfaces_.end()) return;
    const auto position = object_absolute_positions_.find(id);
    std::array<uint32_t, 3> absolute{};
    if (position != object_absolute_positions_.end()) absolute = position->second;
    // 00420610 adds both inherited fixed-position offsets before projection.
    for (const auto* offsets : {&object_pair_3c_40_, &object_pair_44_48_}) {
        if (const auto found = offsets->find(id); found != offsets->end()) {
            absolute[0] += static_cast<uint32_t>(found->second.first) << 16U;
            absolute[1] += static_cast<uint32_t>(found->second.second) << 16U;
        }
    }
    const uint32_t scale = sprite_perspective_scale(static_cast<int32_t>(absolute[2]),
        object_raw_field(id, 0x90U) != 0 ? 0U : object_raw_field(id, 0x288U));
    const uint32_t time = object_raw_field(id, 0xC0U);
    const auto modulation = [time](uint32_t value) {
        return (static_cast<int64_t>(static_cast<int32_t>(value)) * time) >> 24;
    };
    // PE64 140062CD9..62D39 (legacy0041F6A0): angle modulation
    // uses the same signed Q24 -> Q16 easing dispatcher as display motion.
    const uint32_t selector=object_raw_field(id,0x2E8U);
    const auto eased=evaluate_display_easing(static_cast<int32_t>(time),selector);
    if (!eased) throw std::runtime_error("sprite affine angle easing unresolved object="+
        std::to_string(id)+" time="+std::to_string(time)+" coefficient="+
        std::to_string(object_raw_field(id,0x2CCU))+" selector="+std::to_string(selector));
    const int32_t animation=*eased;
    const int32_t angle = static_cast<int32_t>(object_raw_field(id, 0x260U) +
        static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(object_raw_field(id, 0x2CCU))) * animation) >> 16));
    const uint32_t ox = object_raw_field(id, 0x258U) + static_cast<uint32_t>(modulation(object_raw_field(id, 0x2C4U)));
    const uint32_t oy = object_raw_field(id, 0x25CU) + static_cast<uint32_t>(modulation(object_raw_field(id, 0x2C8U)));
    uint32_t sx, sy;
    const int64_t common = modulation(object_raw_field(id, 0x2E4U));
    if (object_raw_field(id, 0x284U) == 0) {
        sx = static_cast<uint32_t>(((scale + modulation(object_raw_field(id, 0x2DCU))) *
            (object_raw_field(id, 0x27CU) + common)) >> 16);
        sy = static_cast<uint32_t>(((scale + modulation(object_raw_field(id, 0x2E0U))) *
            (object_raw_field(id, 0x280U) + common)) >> 16);
    } else {
        const auto delta = [time](uint32_t a, uint32_t b) {
            return ((static_cast<int64_t>(static_cast<int32_t>(a)) + static_cast<int32_t>(b)) * time) >> 24;
        };
        sx = static_cast<uint32_t>(((object_raw_field(id, 0x27CU) + delta(object_raw_field(id, 0x2DCU), object_raw_field(id, 0x2E4U))) * scale) >> 16);
        sy = static_cast<uint32_t>(((object_raw_field(id, 0x280U) + delta(object_raw_field(id, 0x2E0U), object_raw_field(id, 0x2E4U))) * scale) >> 16);
    }
    const auto bounds = sprite_affine_bounds(source->second.width, source->second.height,
        object_raw_field(id, 0x318U) != 0 ? object_raw_field(id, 0x320U) : 0,
        angle, sx, sy, static_cast<int32_t>(absolute[0]), static_cast<int32_t>(absolute[1]),
        static_cast<int32_t>(ox), static_cast<int32_t>(oy));
    if (bounds.width < 2 || bounds.height < 2) return;
    for (const auto [offset, value] : std::array<std::pair<uint32_t, uint32_t>, 9>{{
        {0x29CU, ox}, {0x2A0U, oy}, {0x2A4U, static_cast<uint32_t>(angle)},
        {0x2B4U, sx}, {0x2B8U, sy}, {0x2ECU, bounds.width}, {0x2F0U, bounds.height},
        {0x2F4U, static_cast<uint32_t>(bounds.x)}, {0x2F8U, static_cast<uint32_t>(bounds.y)}}})
        object_raw_fields_[{id, offset}] = value;
    object_raw_fields_[{id, 0xA0U}] = bounds.width;
    object_raw_fields_[{id, 0xA4U}] = bounds.height;
    // 0042EBB0 uses the main-surface center unless108 enables a valid
    // script-specified display pair (004212D0→00449F30).
    const auto [view_width, view_height] = current_mode_dimensions();
    uint32_t center_x = view_width >> 1U, center_y = view_height >> 1U;
    if (object_raw_field(id, 0x108U) != 0 &&
        static_cast<int32_t>(display_pair_first_) >= 0 && display_pair_first_ < view_width &&
        static_cast<int32_t>(display_pair_second_) >= 0 && display_pair_second_ < view_height) {
        center_x = display_pair_first_; center_y = display_pair_second_;
    }
    uint32_t projected_x = absolute[0], projected_y = absolute[1];
    if (object_raw_field(id, 0x294U) != 0) {
        const uint32_t px = static_cast<uint32_t>((static_cast<uint64_t>(
            object_raw_field(id, 0x28CU)) * object_raw_field(id, 0x288U)) >> 16U);
        const uint32_t py = static_cast<uint32_t>((static_cast<uint64_t>(
            object_raw_field(id, 0x290U)) * object_raw_field(id, 0x288U)) >> 16U);
        projected_x = static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(absolute[0])) *
            sprite_perspective_scale(static_cast<int32_t>(absolute[2]), px)) >> 16);
        projected_y = static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(absolute[1])) *
            sprite_perspective_scale(static_cast<int32_t>(absolute[2]), py)) >> 16);
    }
    projected_x += center_x << 16U;
    projected_y += center_y << 16U;
    const int32_t anchor_x = static_cast<int32_t>(projected_x) >> 16;
    const int32_t anchor_y = static_cast<int32_t>(projected_y) >> 16;
    object_positions_[id] = {anchor_x, anchor_y};
    object_raw_fields_[{id, 0x34U}] = static_cast<uint32_t>(anchor_x);
    object_raw_fields_[{id, 0x38U}] = static_cast<uint32_t>(anchor_y);
    const bool rounded = object_raw_field(id, 0x88U) != 0 &&
        (absolute[2] == 0 || object_raw_field(id, 0x8CU) == 1);
    object_raw_fields_[{id, 0x2BCU}] = rounded ? 0U : projected_x & 0xFFFFU;
    object_raw_fields_[{id, 0x2C0U}] = rounded ? 0U : projected_y & 0xFFFFU;
    // Sprite's0042D2B0 origin query subtracts the raster-local origin.
    sprites_[id].x = anchor_x - bounds.x;
    sprites_[id].y = anchor_y - bounds.y;
    update_display_object_geometry(id, sprites_[id].x, sprites_[id].y,
        bounds.width, bounds.height, object_draw_visible(id));
    display_dirty_ = true;
}
} // namespace bgiplayer
