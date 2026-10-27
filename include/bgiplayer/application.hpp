#pragma once

#include <filesystem>
#include <cstddef>
#include <optional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace bgiplayer {

struct Options {
    std::filesystem::path game;
    std::filesystem::path default_font_file;
    std::optional<std::filesystem::path> translation_patch;
    bool disable_translation_patch = false;
    std::map<std::string, std::filesystem::path> font_files;
    std::string backend = "sdl2";
    bool list_archives = false;
    std::optional<std::string> list_archive;
    std::optional<std::pair<std::string, std::string>> dump_resource;
    std::optional<std::filesystem::path> dump_resource_output;
    std::optional<std::pair<std::string, std::string>> dump_bitmap;
    std::optional<std::filesystem::path> dump_bitmap_output;
    std::optional<std::string> start_script;
    double runtime_seconds = 0.0;
    std::size_t startup_steps = 1'000'000;
    bool debug = false;
    std::optional<std::string> render_mode;
    bool trace_startup = false;
    bool trace_title = false;
    std::optional<std::filesystem::path> bp_trace_prefix;
    uint64_t bp_trace_limit = 1'000'000;
    std::vector<uint32_t> frame_watch_offsets;
    std::optional<std::pair<int, int>> click;
    std::vector<std::pair<int, int>> click_sequence;
    std::vector<uint32_t> click_buttons;
    uint32_t click_delay_ms = 0;
    uint32_t click_count = 1;
    uint32_t click_interval_ms = 1000;
    bool trace_clicks = false;
    std::optional<std::filesystem::path> record_clicks;
    std::optional<std::filesystem::path> replay_clicks;
    uint32_t launcher_mode = 0;
    std::optional<std::size_t> dump_code_start;
    std::size_t dump_code_length = 256;
    std::optional<std::string> dump_surfaces_prefix;
    std::optional<std::filesystem::path> screenshot;
    std::optional<std::filesystem::path> save_snapshot;
    std::optional<std::filesystem::path> load_snapshot;
};

// Maps SDL key values to the Win32 virtual-key descriptors used by the target
// engine. Zero means the key has no target-engine descriptor.
uint32_t windows_virtual_key_from_sdl(uint32_t keycode, uint32_t scancode);

class VM;
// Build the exact CPU display frame when every active producer is supported.
bool prepare_vm_display_frame(VM& vm);

int run(const Options& options);

}  // namespace bgiplayer
