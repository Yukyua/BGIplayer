// Original PE32 004C2AF0, PE64 1401164F0.
namespace bgiplayer {
uint32_t SoundSubsystem::set_effect_primary_volume(uint32_t channel, uint32_t volume) {
    if (!device_ready_) return kDeviceUnavailable;
    if (channel >= effect_channels_.size()) return kChannelOutOfRange;
    effect_channels_[channel].primary_volume = std::min(volume, 0x80U);
    return kOk;
}
} // namespace bgiplayer
