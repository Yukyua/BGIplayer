namespace bgiplayer {
std::span<uint8_t> Memory::try_unobserved_writable_span(
    uint32_t address, std::size_t size, uint32_t base) {
    // Ext2 addresses are slot aliases, not byte-offset pointers. Reject
    // namespace crossings and watched frames before considering a bulk view.
    if (size == 0 || size > 0x04000000U ||
        uint64_t(address) + size - 1U > UINT32_MAX ||
        (address >> 26U) != ((address + static_cast<uint32_t>(size - 1U)) >> 26U) ||
        (address < 0x10000000U && uint64_t(address) + size - 1U >= 0x0FFF0000U)) {
        return {};
    }
    try {
        const auto location = resolve(address, base, size);
        if (location.space == Space::Heap) {
            auto& bytes = heap_allocations_.at(location.allocation);
            return std::span<uint8_t>(bytes).subspan(location.offset, size);
        }
        if (!frame_byte_watches_.empty()) return {};
        if (location.space == Space::GlobalFrame)
            return std::span<uint8_t>(frame_).subspan(location.offset, size);
        if (location.space == Space::ThreadFrame)
            return std::span<uint8_t>(local_frame_).subspan(location.offset, size);
    } catch (const BpFault&) {
        // Let the original scalar sequence report its first failing access
        // and retain any writes which preceded that failure.
    }
    return {};
}
} // namespace bgiplayer
