namespace bgiplayer {
uint32_t SdlAudioBackend::playback_state(bool bgm, uint32_t channel) const {
    if ((bgm && channel >= bgm_tracks_.size()) ||
        (!bgm && channel >= effect_tracks_.size())) return 0;
    std::scoped_lock lock(mutex_);
    const auto& slot = track(bgm, channel);
    // Original CDSStreamSpeaker::GetState returns GetStatus() & PLAYING.
    return slot.active && (slot.clip || (slot.progressive && !slot.progressive->failed())) && !slot.paused ? 1U : 0U;
}
} // namespace bgiplayer
