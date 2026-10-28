#pragma once

#include <SDL.h>
#include <cstdint>
#include <vector>

namespace bgiplayer {
// Host presentation owns a logical primary surface independently of desktop
// resolution. SDL restores the window's viewport/scale when leaving a target.
class SdlPrimarySurface {
public:
    SdlPrimarySurface() = default;
    SdlPrimarySurface(const SdlPrimarySurface&) = delete;
    SdlPrimarySurface& operator=(const SdlPrimarySurface&) = delete;
    ~SdlPrimarySurface() { SDL_DestroyTexture(texture_); }

    bool begin(SDL_Renderer* renderer, int width, int height, uint32_t filter_mode = 2) {
        if (renderer_ != renderer || width_ != width || height_ != height) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }
        if (!texture_) {
            renderer_ = renderer;
            width_ = width;
            height_ = height;
            texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_TARGET, width, height);
            if (!texture_) return false;
            SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
        }
        // PE64 1400B0E77: mode0 uses D3DTEXF_LINEAR; mode2 POINT.
        // Shader-dependent modes1/3/4 remain outside this host mapping.
        if ((filter_mode == 0 || filter_mode == 2) &&
            SDL_SetTextureScaleMode(texture_, filter_mode == 0
                ? SDL_ScaleModeLinear : SDL_ScaleModeNearest) != 0) return false;
        return SDL_SetRenderTarget(renderer, texture_) == 0;
    }

    bool copy_to_window() {
        if (SDL_SetRenderTarget(renderer_, nullptr) != 0) return false;
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        if (SDL_RenderClear(renderer_) != 0) return false;
        const SDL_Rect destination{0, 0, width_, height_};
        return SDL_RenderCopy(renderer_, texture_, nullptr, &destination) == 0;
    }

    // Read the retained GPU primary only when a native capture asks for it.
    // Selecting the texture also avoids undefined window back-buffer contents
    // after SDL_RenderPresent. Restore the caller's rendering state on failure.
    bool read_pixels(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) const {
        if(!texture_ || !renderer_)return false;
        auto* previous=SDL_GetRenderTarget(renderer_);
        if(SDL_SetRenderTarget(renderer_,texture_)!=0)return false;
        std::vector<uint8_t> pixels(static_cast<size_t>(width_)*static_cast<size_t>(height_)*4U);
        const int result=SDL_RenderReadPixels(renderer_,nullptr,SDL_PIXELFORMAT_RGBA32,pixels.data(),width_*4);
        const int restored=SDL_SetRenderTarget(renderer_,previous);
        if(result!=0 || restored!=0)return false;
        rgba=std::move(pixels);width=static_cast<uint32_t>(width_);height=static_cast<uint32_t>(height_);return true;
    }

    // Some accelerated sdl2-compat backends report window output dimensions
    // even while an engine-sized target is selected. ReadPixels reads that
    // target, so its allocation and pitch must come from the texture itself.
    static int readback_dimensions(SDL_Renderer* renderer, int* width, int* height) {
        if (SDL_Texture* target = SDL_GetRenderTarget(renderer))
            return SDL_QueryTexture(target, nullptr, nullptr, width, height);
        return SDL_GetRendererOutputSize(renderer, width, height);
    }

    // sdl2-compat applies logical presentation to ReadPixels on a window.
    // Read the physical output with that transform temporarily disabled.
    static int read_window_pixels(SDL_Renderer* renderer, void* pixels, int pitch) {
        int width = 0, height = 0;
        SDL_RenderGetLogicalSize(renderer, &width, &height);
        if (SDL_RenderFlush(renderer) != 0) return -1;
        if (SDL_RenderSetLogicalSize(renderer, 0, 0) != 0) return -1;
        const int result = SDL_RenderReadPixels(renderer, nullptr,
            SDL_PIXELFORMAT_RGBA32, pixels, pitch);
        if (SDL_RenderSetLogicalSize(renderer, width, height) != 0) return -1;
        return result;
    }

private:
    SDL_Texture* texture_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    int width_ = 0, height_ = 0;
};
} // namespace bgiplayer
