#include "bgiplayer/audio_decoder.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <system_error>

namespace bgiplayer {
struct ProgressiveAudio::State {
    static constexpr uint64_t chunk_frames = 4096;
    // Retaining immutable chunks permits exact resume, loops and debug saves.
    // The host result cache separately bounds retained allocations to 64 MiB.
    // Sparse pages cover the full uint32 millisecond host position domain.
    // Only pages actually reached by decoding are allocated.
    static constexpr size_t chunks_per_page = 4096;
    static constexpr size_t max_pages = 16384;
    struct Chunk { std::array<int16_t, chunk_frames * 2> pcm; };
    struct Page { std::array<std::unique_ptr<Chunk>, chunks_per_page> chunks; };
    std::array<std::unique_ptr<Page>, max_pages> pages;
    Chunk* get_chunk(uint64_t frame) const noexcept {
        const auto index = static_cast<size_t>(frame / chunk_frames);
        return pages[index / chunks_per_page]->chunks[index % chunks_per_page].get();
    }
    const size_t encoded_capacity;
    std::atomic<uint64_t> retained;
    explicit State(size_t capacity) : encoded_capacity(capacity),
        retained(sizeof(State) + sizeof(ProgressiveAudio) + 64U + capacity) {}
    std::atomic<uint64_t> available{0};
    std::atomic<bool> done{false}, failure{false}, cancelled{false};
    std::mutex mutex;
    std::condition_variable event;
    std::thread worker;
    std::string error;
    std::shared_ptr<const DecodedAudio> materialized;

    ~State() {
        cancelled.store(true, std::memory_order_relaxed);
        if (worker.joinable()) worker.join();
    }
    void run(AudioPcmProducer producer) noexcept {
        uint64_t written = 0;
        try {
            const bool ok = producer([&](std::span<const int16_t> pcm) {
                if (cancelled.load(std::memory_order_relaxed) || (pcm.size() & 1U)) return false;
                size_t consumed = 0;
                while (consumed < pcm.size()) {
                    const size_t index = static_cast<size_t>(written / chunk_frames);
                    const size_t page = index / chunks_per_page;
                    if (page >= pages.size()) { error = "progressive audio exceeds host position domain"; return false; }
                    if (!pages[page]) {
                        pages[page] = std::make_unique<Page>();
                        retained.fetch_add(sizeof(Page), std::memory_order_relaxed);
                    }
                    auto& chunk = pages[page]->chunks[index % chunks_per_page];
                    if (!chunk) {
                        chunk.reset(new Chunk);
                        retained.fetch_add(sizeof(Chunk), std::memory_order_relaxed);
                    }
                    const size_t offset = static_cast<size_t>(written % chunk_frames) * 2;
                    const size_t count = std::min(pcm.size() - consumed, chunk->pcm.size() - offset);
                    std::copy_n(pcm.data() + consumed, count, chunk->pcm.data() + offset);
                    consumed += count;
                    written += count / 2;
                    // Only immutable complete chunks become callback-visible.
                    if (written % chunk_frames == 0) {
                        if (written <= 3 * chunk_frames) {
                            { std::scoped_lock lock(mutex); available.store(written, std::memory_order_release); }
                            event.notify_all();
                        } else available.store(written, std::memory_order_release);
                    }
                }
                return !cancelled.load(std::memory_order_relaxed);
            }, &error);
            if (!ok) failure.store(true, std::memory_order_release);
            // EOF's final short chunk becomes immutable before done publication.
            if (ok) available.store(written, std::memory_order_release);
        } catch (const std::exception& e) {
            error = e.what();
            failure.store(true, std::memory_order_release);
        } catch (...) {
            error = "progressive audio worker exception";
            failure.store(true, std::memory_order_release);
        }
        // Destroy the producer's encoded vector before publishing completion.
        producer = {};
        retained.fetch_sub(encoded_capacity, std::memory_order_relaxed);
        { std::scoped_lock lock(mutex); done.store(true, std::memory_order_release); }
        event.notify_all();
    }
};

ProgressiveAudio::ProgressiveAudio(std::unique_ptr<State> state) : state_(std::move(state)) {}
ProgressiveAudio::~ProgressiveAudio() = default;
std::shared_ptr<ProgressiveAudio> ProgressiveAudio::open(std::vector<uint8_t> encoded, std::string* error) {
    // Proprietary PCM/ADPCM models still use the verified full decode route.
    if (encoded.size() >= 64 && std::memcmp(encoded.data() + 4, "bw  ", 4) == 0 &&
        (encoded[48] != 3 || encoded[49] || encoded[50] || encoded[51])) return {};
    const size_t capacity = encoded.capacity();
    try {
        return start_impl([bytes = std::move(encoded)](const AudioPcmSink& sink, std::string* why) {
            return decode_audio_incremental(bytes, sink, why);
        }, capacity, error);
    } catch (const std::system_error& e) {
        // Thread creation is an optional host optimization. Keep the verified
        // synchronous decoder available when the host cannot start a worker.
        if (error) *error = e.what();
        return {};
    }
}
std::shared_ptr<ProgressiveAudio> ProgressiveAudio::start(AudioPcmProducer producer, std::string* error) {
    return start_impl(std::move(producer), 0, error);
}
std::shared_ptr<ProgressiveAudio> ProgressiveAudio::start_impl(AudioPcmProducer producer,
    size_t encoded_capacity, std::string* error) {
    if (!producer) return {};
    auto result = std::shared_ptr<ProgressiveAudio>(new ProgressiveAudio(std::make_unique<State>(encoded_capacity)));
    auto& state = *result->state_;
    state.worker = std::thread([&state, producer = std::move(producer)]() mutable { state.run(std::move(producer)); });
    // Three immutable chunks = 256 ms of audio, a few ms of decoding wall time.
    // Empty, malformed headers and early decoder errors must fail synchronously.
    std::unique_lock lock(state.mutex);
    state.event.wait(lock, [&] {
        return state.available.load(std::memory_order_acquire) >= 3 * State::chunk_frames ||
               state.done.load(std::memory_order_acquire);
    });
    if (state.failure.load(std::memory_order_acquire) || !state.available.load(std::memory_order_acquire)) {
        if (error) *error = state.error;
        return {};
    }
    return result;
}
uint64_t ProgressiveAudio::available_frames() const noexcept { return state_->available.load(std::memory_order_acquire); }
bool ProgressiveAudio::complete() const noexcept { return state_->done.load(std::memory_order_acquire); }
bool ProgressiveAudio::failed() const noexcept { return state_->failure.load(std::memory_order_acquire); }
uint64_t ProgressiveAudio::retained_bytes() const noexcept { return state_->retained.load(std::memory_order_relaxed); }
std::string ProgressiveAudio::error_text() const {
    return complete() ? state_->error : std::string{};
}
bool ProgressiveAudio::sample(uint64_t frame, int16_t& left, int16_t& right) const noexcept {
    if (frame >= available_frames()) return false;
    const auto& chunk = *state_->get_chunk(frame);
    const size_t offset = static_cast<size_t>(frame % State::chunk_frames) * 2;
    left = chunk.pcm[offset]; right = chunk.pcm[offset + 1];
    return true;
}
std::shared_ptr<const DecodedAudio> ProgressiveAudio::completed_clip() {
    std::unique_lock lock(state_->mutex);
    state_->event.wait(lock, [&] { return state_->done.load(std::memory_order_acquire); });
    if (failed()) return {};
    if (!state_->materialized) {
        auto result = std::make_shared<DecodedAudio>();
        const uint64_t frames = available_frames();
        result->samples.resize(static_cast<size_t>(frames) * 2);
        for (uint64_t offset = 0; offset < frames; offset += State::chunk_frames) {
            const size_t count = static_cast<size_t>(std::min(State::chunk_frames, frames - offset)) * 2;
            std::copy_n(state_->get_chunk(offset)->pcm.data(), count,
                        result->samples.data() + static_cast<size_t>(offset) * 2);
        }
        state_->retained.fetch_add(result->samples.capacity() * sizeof(int16_t) + sizeof(DecodedAudio) + 64U,
                                  std::memory_order_relaxed);
        state_->materialized = result;
    }
    return state_->materialized;
}
} // namespace bgiplayer
