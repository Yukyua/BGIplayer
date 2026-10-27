#pragma once
#include <SDL.h>
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace bgiplayer {
// Host-only resources. The owner must outlive neither its SDL renderer nor
// the last-presented primary snapshot associated with it.
class DisplayFrameTextureCache {
public:
    struct CaptureKey {
        SDL_Renderer* renderer;
        SDL_Texture* target;
        int width, height, shake_x, shake_y;
        std::array<int,4> viewport;
        std::array<int,4> clip;
        bool clip_enabled;
        std::array<int,2> logical_size;
        std::array<float,2> scale;
        uint64_t generation;
        bool operator==(const CaptureKey&) const = default;
    };
    DisplayFrameTextureCache() = default;
    DisplayFrameTextureCache(const DisplayFrameTextureCache&) = delete;
    DisplayFrameTextureCache& operator=(const DisplayFrameTextureCache&) = delete;
    ~DisplayFrameTextureCache() { SDL_DestroyTexture(texture_); }

    bool matches(SDL_Renderer* renderer, int width, int height) const {
        return valid_ && renderer_ == renderer && width_ == width && height_ == height;
    }
    bool upload(SDL_Renderer* renderer, int width, int height,
                std::span<const uint8_t> rgba) {
        if (width <= 0 || height <= 0 || rgba.size() != uint64_t(width)*height*4U)
            return false;
        if (renderer_ != renderer || width_ != width || height_ != height) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }
        renderer_ = renderer; width_ = width; height_ = height;
        invalidate();
        if (!texture_) {
            texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_STATIC, width, height);
            if (!texture_) return false;
            SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
        }
        if (SDL_UpdateTexture(texture_, nullptr, rgba.data(), width*4) != 0) return false;
        ++generation_;
        valid_ = true;
        return true;
    }
    bool draw(int shake_x, int shake_y) const {
        const SDL_Rect destination{shake_x, shake_y, width_, height_};
        return valid_ && SDL_RenderCopy(renderer_, texture_, nullptr, &destination) == 0;
    }
    std::optional<CaptureKey> capture_key(int shake_x, int shake_y) const {
        if (!valid_) return std::nullopt;
        int width=0, height=0;
        auto* target = SDL_GetRenderTarget(renderer_);
        const int result = target ? SDL_QueryTexture(target,nullptr,nullptr,&width,&height)
                                  : SDL_GetRendererOutputSize(renderer_,&width,&height);
        if (result != 0 || width <= 0 || height <= 0) return std::nullopt;
        SDL_Rect viewport{};
        SDL_RenderGetViewport(renderer_,&viewport);
        SDL_Rect clip{};
        SDL_RenderGetClipRect(renderer_,&clip);
        int logical_width=0,logical_height=0;
        SDL_RenderGetLogicalSize(renderer_,&logical_width,&logical_height);
        float sx=0,sy=0;
        SDL_RenderGetScale(renderer_,&sx,&sy);
        return CaptureKey{renderer_,target,width,height,shake_x,shake_y,
            {viewport.x,viewport.y,viewport.w,viewport.h},
            {clip.x,clip.y,clip.w,clip.h},SDL_RenderIsClipEnabled(renderer_)==SDL_TRUE,
            {logical_width,logical_height},{sx,sy},generation_};
    }
    bool can_reuse_capture(const CaptureKey& key) const {
        return presented_ && *presented_ == key;
    }
    void mark_presented(const CaptureKey& key) { presented_ = key; }
    void record_capture_reuse() { ++capture_reuses_; }
    uint64_t capture_reuses() const { return capture_reuses_; }
    void invalidate() { valid_ = false; presented_.reset(); }
    void reset() { invalidate(); SDL_DestroyTexture(texture_); texture_ = nullptr; }
private:
    SDL_Texture* texture_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    int width_=0,height_=0;
    bool valid_=false;
    uint64_t generation_=0;
    uint64_t capture_reuses_=0;
    std::optional<CaptureKey> presented_;
};
}
