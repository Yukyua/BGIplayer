#include "bgiplayer/host_files.hpp"
#include "bgiplayer/application.hpp"
#include "bgiplayer/common.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

uint32_t parse_launcher_mode(std::string_view value) {
    if (value == "Execute as a launcher.") {
        return 1;
    }
    std::size_t consumed = 0;
    const std::string text(value);
    const unsigned long mode = std::stoul(text, &consumed, 10);
    if (consumed != text.size() || mode > 1) {
        throw bgiplayer::Error(
            "launcher mode must be 0 or 1");
    }
    return static_cast<uint32_t>(mode);
}

bgiplayer::Options parse_options(int argc, char** argv) {
    bgiplayer::Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto require_value = [&]() -> std::string {
            if (index + 1 >= argc) {
                throw bgiplayer::Error("missing value after " + argument);
            }
            return argv[++index];
        };
        if (argument == "--game") {
            options.game = require_value();
        } else if (argument == "--no-translation-patch") {
            options.disable_translation_patch = true;
        } else if (argument == "--translation-patch") {
            options.translation_patch = require_value();
        } else if (argument == "--default-font") {
            options.default_font_file = require_value();
            if (!bgiplayer::host_files::is_regular_file(options.default_font_file))
                throw bgiplayer::Error("--default-font file is unavailable");
        } else if (argument == "--font") {
            const std::string mapping = require_value();
            const std::size_t separator = mapping.find('=');
            if (separator == std::string::npos || separator == 0 || separator + 1U == mapping.size()) {
                throw bgiplayer::Error("--font expects FACE=FILE");
            }
            const std::filesystem::path path = mapping.substr(separator + 1U);
            if (!bgiplayer::host_files::is_regular_file(path)) {
                throw bgiplayer::Error("--font file is unavailable: " + path.string());
            }
            options.font_files[mapping.substr(0, separator)] = path;
        } else if (argument == "--backend") {
            options.backend = require_value();
        } else if (argument == "--list-archives") {
            options.list_archives = true;
        } else if (argument == "--list-archive") {
            options.list_archive = require_value();
        } else if (argument == "--dump-resource") {
            const std::string source = require_value();
            const std::size_t separator = source.find(':');
            if (separator == std::string::npos) {
                throw bgiplayer::Error(
                    "--dump-resource expects ARCHIVE:RESOURCE");
            }
            options.dump_resource = std::pair{
                source.substr(0, separator),
                source.substr(separator + 1)};
        } else if (argument == "--dump-resource-output") {
            options.dump_resource_output = require_value();
        } else if (argument == "--dump-bitmap") {
            const std::string source = require_value();
            const std::size_t separator = source.find(':');
            if (separator == std::string::npos) {
                throw bgiplayer::Error(
                    "--dump-bitmap expects ARCHIVE:RESOURCE");
            }
            options.dump_bitmap = std::pair{
                source.substr(0, separator),
                source.substr(separator + 1)};
        } else if (argument == "--dump-bitmap-output") {
            options.dump_bitmap_output = require_value();
        } else if (argument == "--script") {
            options.start_script = require_value();
        } else if (argument == "--seconds") {
            options.runtime_seconds = std::stod(require_value());
        } else if (argument == "--steps") {
            options.startup_steps = std::stoull(require_value());
        } else if (argument == "--debug") {
            options.debug = true;
        } else if (argument == "--render-mode") {
            options.render_mode = require_value();
        } else if (argument == "--trace-startup") {
            options.trace_startup = true;
        } else if (argument == "--trace-title") {
            options.trace_title = true;
        } else if (argument == "--trace-bp") {
            options.bp_trace_prefix = require_value();
        } else if (argument == "--trace-bp-limit") {
            options.bp_trace_limit = std::stoull(require_value());
        } else if (argument == "--watch-frame") {
            const auto offset = std::stoull(require_value(), nullptr, 0);
            if (offset > 0xFFFFFFFFULL) throw bgiplayer::Error("frame watch offset exceeds 32 bits");
            options.frame_watch_offsets.push_back(static_cast<uint32_t>(offset));
        } else if (argument == "--click" || argument == "--right-click") {
            const std::string point = require_value();
            const std::size_t separator = point.find(',');
            if (separator == std::string::npos) {
                throw bgiplayer::Error(
                    "click expects LOGICAL_X,LOGICAL_Y");
            }
            options.click_sequence.emplace_back(
                std::stoi(point.substr(0, separator)),
                std::stoi(point.substr(separator + 1)));
            options.click = options.click_sequence.front();
            options.click_buttons.push_back(argument == "--right-click" ? 2U : 1U);
        } else if (argument == "--trace-clicks") {
            options.trace_clicks = true;
        } else if (argument == "--record-clicks") {
            options.record_clicks = require_value();
        } else if (argument == "--replay-clicks") {
            options.replay_clicks = require_value();
        } else if (argument == "--click-delay-ms") {
            options.click_delay_ms = static_cast<uint32_t>(
                std::stoul(require_value()));
        } else if (argument == "--click-count") {
            options.click_count = static_cast<uint32_t>(
                std::stoul(require_value()));
            if (options.click_count == 0) {
                throw bgiplayer::Error(
                    "--click-count must be at least one");
            }
        } else if (argument == "--click-interval-ms") {
            options.click_interval_ms = static_cast<uint32_t>(
                std::stoul(require_value()));
        } else if (argument == "--launcher-mode") {
            options.launcher_mode = parse_launcher_mode(require_value());
        } else if (argument == "--dump-code") {
            const std::string range = require_value();
            const std::size_t separator = range.find(':');
            options.dump_code_start =
                static_cast<std::size_t>(std::stoul(range.substr(0, separator), nullptr, 0));
            if (separator != std::string::npos) {
                options.dump_code_length =
                    static_cast<std::size_t>(std::stoul(range.substr(separator + 1), nullptr, 0));
            }
        } else if (argument == "--save-snapshot") {
            options.save_snapshot = require_value();
        } else if (argument == "--load-snapshot") {
            options.load_snapshot = require_value();
        } else if (argument == "--screenshot") {
            options.screenshot = require_value();
        } else if (argument == "--dump-surfaces") {
            options.dump_surfaces_prefix = require_value();
        } else if (argument == "Execute as a launcher.") {
            options.launcher_mode = parse_launcher_mode(argument);
        } else if (argument == "--help" || argument == "-h") {
            std::cout
            << "usage: bgiplayer --game DIR [--backend null|sdl2]\n"
               "       bgiplayer --game DIR [--font FACE=FILE ...] [--default-font FILE]\n"
               "       bgiplayer --game DIR [--translation-patch FILE|--no-translation-patch]\n"
               "       bgiplayer --game DIR --click X,Y [--click X,Y|--right-click X,Y ...] "
               "[--click-delay-ms N] [--click-count N] "
               "[--click-interval-ms N] [--seconds N]\n"
               "       bgiplayer --game DIR [--debug] [--render-mode fast|exact]\n"
               "       bgiplayer --game DIR [--trace-clicks] [--record-clicks FILE] [--replay-clicks FILE]\n"
               "       bgiplayer --game DIR [--load-snapshot FILE] [--save-snapshot FILE]\n"
               "       bgiplayer --game DIR --launcher-mode 0|1\n"
               "       bgiplayer --game DIR --trace-bp PREFIX [--trace-bp-limit N]\n"
               "       bgiplayer --game DIR --watch-frame OFFSET\n"
                   "       bgiplayer --game DIR --list-archives\n"
                   "       bgiplayer --game DIR --list-archive NAME\n"
                   "       bgiplayer --game DIR --dump-resource "
                   "ARCHIVE:RESOURCE --dump-resource-output PATH\n"
                   "       bgiplayer --game DIR --dump-bitmap "
                   "ARCHIVE:RESOURCE --dump-bitmap-output PATH\n"
                   "       bgiplayer --game DIR --dump-surfaces PREFIX\n";
            std::exit(0);
        } else {
            throw bgiplayer::Error("unknown argument: " + argument);
        }
    }
    if (options.game.empty()) {
        throw bgiplayer::Error("--game is required");
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return bgiplayer::run(parse_options(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "BGIplayer: " << error.what() << '\n';
        return 1;
    }
}
