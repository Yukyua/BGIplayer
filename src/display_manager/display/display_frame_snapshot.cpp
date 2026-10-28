// Host frame readback is transient unless the caller explicitly requests a view.
namespace bgiplayer {
const VM::GraphicsSurface& VM::display_frame_view() const {
    if(display_frame_.pixels.empty() && display_frame_gpu_cached_ && display_frame_readback_provider_) {
        GraphicsSurface frame;
        if(display_frame_readback_provider_(frame) && frame.width==display_frame_.width &&
            frame.height==display_frame_.height && frame.format==display_frame_.format) {
            refresh_bgra_mirror(frame);display_frame_=std::move(frame);
        }
    }
    return display_frame_;
}
std::optional<DecodedBitmap> VM::display_frame_snapshot() const {
    GraphicsSurface transient;
    const GraphicsSurface* frame=&display_frame_;
    if(frame->pixels.empty() && display_frame_gpu_cached_ && display_frame_readback_provider_) {
        if(!display_frame_readback_provider_(transient) || transient.width!=frame->width ||
            transient.height!=frame->height || transient.format!=frame->format)return std::nullopt;
        refresh_bgra_mirror(transient);frame=&transient;
    }
    if(frame->rgba.empty())return std::nullopt;
    return DecodedBitmap{frame->width,frame->height,frame->format,frame->rgba};
}
} // namespace bgiplayer
