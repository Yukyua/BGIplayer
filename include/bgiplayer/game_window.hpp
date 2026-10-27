#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>
struct SDL_Window;
namespace bgiplayer {
struct GameWindowIcon { uint32_t width=0, height=0; std::vector<uint8_t> rgba; };
std::optional<GameWindowIcon> game_icon_from_pe(std::span<const uint8_t> image);
void configure_game_window(SDL_Window* window, const std::filesystem::path& game);
#ifdef __APPLE__
void set_game_dock_icon(const GameWindowIcon& icon);
#endif
}
