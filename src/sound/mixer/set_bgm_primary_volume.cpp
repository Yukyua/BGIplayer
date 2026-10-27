// Original PE32 004C2A90, PE64 140114F30.
namespace bgiplayer {
uint32_t SoundSubsystem::set_bgm_primary_volume(uint32_t channel, uint32_t volume) {
    if (!device_ready_) return kDeviceUnavailable;
    if (channel >= bgm_channels_.size()) return kChannelOutOfRange;
    bgm_channels_[channel].primary_volume = volume;
    return kOk;
}
} // namespace bgiplayer
