#pragma once
#include "bgiplayer/text.hpp"
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace bgiplayer {
struct MediaSourceRequest {
    std::string path;
    std::string message;
    std::string caption;
    uint32_t prompt = 0;
    TextEncoding encoding = TextEncoding::ShiftJis;
};
// Original 00471B40 / 1400BC1C0 marker parent-prefix behavior.
std::string media_marker_prefix(std::string_view path,TextEncoding encoding);
std::vector<std::filesystem::path> host_removable_volumes();
using MediaVolumeProbe=std::function<std::vector<std::filesystem::path>()>;
using MediaProbeDelay=std::function<bool()>;
using MediaProbePrompt=std::function<int(bool,const MediaSourceRequest&)>;
std::optional<std::string> run_media_source_probe(const MediaSourceRequest&,
    const MediaVolumeProbe&,const MediaProbeDelay&,const MediaProbePrompt&);
std::optional<std::string> probe_host_media_source(const MediaSourceRequest&,bool allow_dialog);
std::optional<std::string> find_media_source(
    const MediaSourceRequest& request,
    const std::vector<std::filesystem::path>& volumes);
}
