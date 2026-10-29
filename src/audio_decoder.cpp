#include "bgiplayer/audio_decoder.hpp"
#include "bgiplayer/audio_decode_controls.hpp"
#include "bgiplayer/decode_warnings.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace bgiplayer {
namespace {

struct ScopedAudioLayout {
    AVChannelLayout value{};
    ~ScopedAudioLayout() { av_channel_layout_uninit(&value); }
};

struct MemoryReader {
    std::span<const uint8_t> data;
    std::size_t position = 0;
};

int read_packet(void* opaque, uint8_t* buffer, int size) {
    auto& reader = *static_cast<MemoryReader*>(opaque);
    if (size <= 0 || reader.position >= reader.data.size()) {
        return AVERROR_EOF;
    }
    const std::size_t remaining = reader.data.size() - reader.position;
    const std::size_t count =
        std::min<std::size_t>(remaining, static_cast<std::size_t>(size));
    std::memcpy(buffer, reader.data.data() + reader.position, count);
    reader.position += count;
    return static_cast<int>(count);
}

int64_t seek_packet(void* opaque, int64_t offset, int whence) {
    auto& reader = *static_cast<MemoryReader*>(opaque);
    if ((whence & AVSEEK_SIZE) != 0) {
        return static_cast<int64_t>(reader.data.size());
    }
    whence &= ~AVSEEK_FORCE;
    int64_t target = 0;
    switch (whence) {
        case SEEK_SET:
            target = offset;
            break;
        case SEEK_CUR:
            target = static_cast<int64_t>(reader.position) + offset;
            break;
        case SEEK_END:
            target = static_cast<int64_t>(reader.data.size()) + offset;
            break;
        default:
            return AVERROR(EINVAL);
    }
    if (target < 0 ||
        static_cast<uint64_t>(target) > reader.data.size()) {
        return AVERROR(EINVAL);
    }
    reader.position = static_cast<std::size_t>(target);
    return target;
}

struct FormatContextDeleter {
    void operator()(AVFormatContext* context) const {
        if (context == nullptr) {
            return;
        }
        if ((context->flags & AVFMT_FLAG_CUSTOM_IO) != 0 &&
            context->pb != nullptr) {
            av_freep(&context->pb->buffer);
            avio_context_free(&context->pb);
        }
        avformat_close_input(&context);
    }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const {
        avcodec_free_context(&context);
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};

struct ResamplerDeleter {
    void operator()(SwrContext* context) const {
        swr_free(&context);
    }
};

uint32_t read_u32_le(std::span<const uint8_t> data,
                     std::size_t offset) {
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1U]) << 8U) |
           (static_cast<uint32_t>(data[offset + 2U]) << 16U) |
           (static_cast<uint32_t>(data[offset + 3U]) << 24U);
}

void write_u16_le(std::vector<uint8_t>& data, std::size_t offset,
                  uint16_t value) {
    data[offset] = static_cast<uint8_t>(value);
    data[offset + 1U] = static_cast<uint8_t>(value >> 8U);
}

void write_u32_le(std::vector<uint8_t>& data, std::size_t offset,
                  uint32_t value) {
    data[offset] = static_cast<uint8_t>(value);
    data[offset + 1U] = static_cast<uint8_t>(value >> 8U);
    data[offset + 2U] = static_cast<uint8_t>(value >> 16U);
    data[offset + 3U] = static_cast<uint8_t>(value >> 24U);
}

std::vector<uint8_t> decode_wavebox_adpcm4(
    std::span<const uint8_t> payload, uint32_t sample_count,
    uint32_t channels) {
    constexpr std::array<uint32_t, 8> step_table{
        57, 57, 57, 57, 77, 102, 128, 153};
    std::array<int32_t, 2> predictor{};
    std::array<uint32_t, 2> step{0x7FU, 0x7FU};
    std::vector<uint8_t> pcm(
        static_cast<std::size_t>(sample_count) * 2U);
    for (uint32_t sample = 0; sample < sample_count; ++sample) {
        const uint8_t packed = payload[sample >> 1U];
        const uint32_t code =
            (sample & 1U) == 0U ? packed & 0x0FU
                               : packed >> 4U;
        const std::size_t channel =
            channels == 2U ? sample & 1U : 0U;
        const int32_t delta =
            static_cast<int32_t>(
                1 - ((code >> 2U) & 2U)) *
                ((static_cast<int32_t>(
                      (code & 7U) * 2U + 1U) *
                  static_cast<int32_t>(step[channel])) >>
                 3) +
            predictor[channel];
        uint32_t next_step =
            (step_table[code & 7U] * step[channel]) >> 6U;
        if (next_step < 0x7FU) {
            next_step = 0x7FU;
        } else if (next_step > 0x6000U) {
            next_step = 0x6000U;
        }
        predictor[channel] = delta;
        step[channel] = next_step;
        const int32_t value =
            delta > 0x7FFF
                ? 0x7FFF
                : (delta < -0x8000 ? -0x8000 : delta);
        write_u16_le(
            pcm, static_cast<std::size_t>(sample) * 2U,
            static_cast<uint16_t>(value));
    }
    return pcm;
}

std::optional<std::vector<uint8_t>> decode_wavebox_hfadpcm8(
    std::span<const uint8_t> payload, uint32_t sample_count,
    uint32_t channels, uint32_t adaptation, std::string* error) {
    constexpr std::size_t kTreePrefix = 8;
    constexpr std::size_t kTreeBytes = 0x400;
    constexpr std::size_t kBitstreamOffset =
        kTreePrefix + kTreeBytes;
    const auto fail =
        [error](std::string_view reason)
            -> std::optional<std::vector<uint8_t>> {
        if (error != nullptr) {
            *error = std::string(reason);
        }
        return std::nullopt;
    };
    if (payload.size() < kBitstreamOffset) {
        return fail("truncated WaveBox HF ADPCM8 header");
    }
    const std::span<const uint8_t> tree =
        payload.subspan(kTreePrefix, kTreeBytes);
    const std::span<const uint8_t> bitstream =
        payload.subspan(kBitstreamOffset);
    const uint32_t root = read_u32_le(tree, 0);
    std::size_t bit_position = 0;
    const auto decode_symbol = [&]() -> std::optional<uint8_t> {
        uint32_t node = root;
        for (uint32_t depth = 0; node > 0xFFU; ++depth) {
            if (depth >= 0x100U ||
                bit_position >= bitstream.size() * 8U) {
                return std::nullopt;
            }
            const uint8_t packed =
                bitstream[bit_position >> 3U];
            const uint32_t branch =
                (packed &
                 static_cast<uint8_t>(
                     0x80U >> (bit_position & 7U))) != 0
                    ? 1U
                    : 0U;
            const std::size_t offset =
                static_cast<std::size_t>(node - 0xFFU) * 4U +
                branch * 2U;
            if (offset + 2U > tree.size()) {
                return std::nullopt;
            }
            node = static_cast<uint32_t>(tree[offset]) |
                   (static_cast<uint32_t>(tree[offset + 1U]) << 8U);
            ++bit_position;
        }
        return static_cast<uint8_t>(node & 0xFFU);
    };

    const uint32_t scale =
        1U << (((adaptation + 1U) >> 1U) & 0x1FU);
    std::array<int32_t, 2> predictor{};
    std::array<uint32_t, 2> step{0x7FU, 0x7FU};
    std::vector<uint8_t> pcm(
        static_cast<std::size_t>(sample_count) * 2U);
    for (uint32_t sample = 0; sample < sample_count; ++sample) {
        const auto symbol = decode_symbol();
        if (!symbol) {
            return fail("truncated WaveBox HF ADPCM8 bitstream");
        }
        const uint32_t code = *symbol;
        const uint32_t magnitude = code & 0x7FU;
        const std::size_t channel =
            channels == 2U ? sample & 1U : 0U;
        const int64_t weighted =
            static_cast<int64_t>(magnitude * 2U + 1U) *
            step[channel];
        const int32_t quantized = static_cast<int32_t>(
            weighted / (static_cast<uint64_t>(scale) * 2U));
        const int32_t delta =
            ((code & 0x80U) == 0 ? quantized : -quantized) +
            predictor[channel];
        int32_t next_step = 0;
        if (magnitude < scale) {
            next_step =
                static_cast<int32_t>(step[channel] * 0x39U);
        } else {
            const int32_t difference =
                static_cast<int32_t>(magnitude - scale);
            const double correction =
                static_cast<double>(difference) * -25.6;
            next_step =
                (0x4D - static_cast<int32_t>(correction)) *
                static_cast<int32_t>(step[channel]);
        }
        next_step =
            (next_step + (next_step < 0 ? 0x3F : 0)) >> 6;
        if (next_step < 0x7F) {
            next_step = 0x7F;
        } else if (next_step > 0x6000) {
            next_step = 0x6000;
        }
        predictor[channel] = delta;
        step[channel] = static_cast<uint32_t>(next_step);
        const int32_t value =
            delta > 0x7FFF
                ? 0x7FFF
                : (delta < -0x8000 ? -0x8000 : delta);
        write_u16_le(
            pcm, static_cast<std::size_t>(sample) * 2U,
            static_cast<uint16_t>(value));
    }
    return pcm;
}

}  // namespace

static std::optional<DecodedAudio> decode_audio_impl(
    std::span<const uint8_t> encoded, std::string* error,
    const AudioDecodeControls* controls, const AudioPcmSink* sink) {
    bool cancelled = false;
    const auto fail =
        [error, encoded, &cancelled](std::string_view reason) -> std::optional<DecodedAudio> {
        if (error != nullptr) {
            *error = cancelled ? "incremental sink cancelled" : std::string(reason);
        }
        if (!cancelled) warn_decode_failure("audio", reason, encoded);
        return std::nullopt;
    };
    if (encoded.empty()) {
        return fail("empty");
    }
    // The WaveBox OGG reader consumes one 0x40-byte header before handing
    // the remaining stream to its decoder. 0x004C8420 checks this header's
    // "bw  " magic and model selector 3 at +0x30; 0x004CA0B0 is the OGG
    // selector method. The following stream begins at the fixed 0x40 offset.
    constexpr std::size_t kWaveBoxHeaderSize = 0x40;
    constexpr std::size_t kWaveBoxModelOffset = 0x30;
    constexpr uint32_t kWaveBoxOggModel = 3;
    constexpr uint32_t kWaveBoxPcm16Model = 1;
    constexpr std::size_t kWaveBoxPayloadSizeOffset = 0x08;
    constexpr std::size_t kWaveBoxFrameCountOffset = 0x0C;
    constexpr std::size_t kWaveBoxSampleRateOffset = 0x10;
    constexpr std::size_t kWaveBoxChannelsOffset = 0x14;
    bool is_wavebox_ogg = false;
    bool is_wavebox_pcm16 = false;
    bool is_wavebox_adpcm4 = false;
    bool is_wavebox_hfadpcm8 = false;
    if (encoded.size() >= kWaveBoxHeaderSize &&
        std::memcmp(encoded.data() + 4, "bw  ", 4) == 0) {
        const uint32_t model =
            read_u32_le(encoded, kWaveBoxModelOffset);
        is_wavebox_ogg = model == kWaveBoxOggModel;
        is_wavebox_pcm16 = model == kWaveBoxPcm16Model;
        is_wavebox_adpcm4 = model == 0;
        is_wavebox_hfadpcm8 = model == 2;
    }
    if (is_wavebox_ogg) {
        encoded = encoded.subspan(kWaveBoxHeaderSize);
        if (encoded.empty()) {
            return fail("empty WaveBox OGG payload");
        }
    } else if (is_wavebox_pcm16 || is_wavebox_adpcm4 ||
               is_wavebox_hfadpcm8) {
        const uint32_t payload_size =
            read_u32_le(encoded, kWaveBoxPayloadSizeOffset);
        const uint32_t frame_count =
            read_u32_le(encoded, kWaveBoxFrameCountOffset);
        const uint32_t sample_rate =
            read_u32_le(encoded, kWaveBoxSampleRateOffset);
        const uint32_t channels =
            read_u32_le(encoded, kWaveBoxChannelsOffset);
        if (sample_rate == 0 || channels == 0 ||
            channels > 0xFFFFU) {
            return fail("invalid WaveBox PCM16 format");
        }
        const uint64_t sample_count =
            static_cast<uint64_t>(frame_count) * channels;
        const uint64_t expected_bytes =
            is_wavebox_pcm16
                ? sample_count * 2U
                : (is_wavebox_adpcm4
                       ? (sample_count + 1U) / 2U
                       : 0U);
        const uint64_t byte_rate =
            static_cast<uint64_t>(sample_rate) * channels * 2U;
        const std::size_t available =
            encoded.size() - kWaveBoxHeaderSize;
        if (sample_count == 0 ||
            (is_wavebox_hfadpcm8 &&
             (payload_size < 0x408U || payload_size > available)) ||
            (!is_wavebox_hfadpcm8 &&
             (payload_size < expected_bytes ||
              expected_bytes > available ||
              expected_bytes > 0xFFFFFFFFULL - 36ULL)) ||
            sample_count > 0xFFFFFFFFULL ||
            byte_rate > 0xFFFFFFFFULL) {
            return fail("truncated WaveBox audio payload");
        }
        const std::span<const uint8_t> payload =
            encoded.subspan(kWaveBoxHeaderSize);
        std::vector<uint8_t> pcm;
        if (is_wavebox_pcm16) {
            pcm.assign(
                payload.begin(),
                payload.begin() +
                    static_cast<std::ptrdiff_t>(expected_bytes));
        } else if (is_wavebox_adpcm4) {
            pcm = decode_wavebox_adpcm4(
                payload.first(
                    static_cast<std::size_t>(expected_bytes)),
                static_cast<uint32_t>(sample_count), channels);
        } else {
            const auto decoded = decode_wavebox_hfadpcm8(
                payload.first(payload_size),
                static_cast<uint32_t>(sample_count), channels,
                read_u32_le(encoded, 0x3C), error);
            if (!decoded) {
                return std::nullopt;
            }
            pcm = *decoded;
        }
        const uint32_t data_size =
            static_cast<uint32_t>(pcm.size());
        std::vector<uint8_t> wav(44U + data_size, 0);
        std::copy_n("RIFF", 4, wav.begin());
        write_u32_le(wav, 4, 36U + data_size);
        std::copy_n("WAVEfmt ", 8, wav.begin() + 8);
        write_u32_le(wav, 16, 16U);
        write_u16_le(wav, 20, 1U);
        write_u16_le(wav, 22, static_cast<uint16_t>(channels));
        write_u32_le(wav, 24, sample_rate);
        write_u32_le(wav, 28, static_cast<uint32_t>(byte_rate));
        write_u16_le(wav, 32, static_cast<uint16_t>(channels * 2U));
        write_u16_le(wav, 34, 16U);
        std::copy_n("data", 4, wav.begin() + 36);
        write_u32_le(wav, 40, data_size);
        std::copy(pcm.begin(), pcm.end(), wav.begin() + 44);
        return decode_audio_impl(wav, error, controls, sink);
    }
    MemoryReader reader{encoded};
    constexpr int kIoBufferSize = 32768;
    auto* io_buffer = static_cast<uint8_t*>(
        av_malloc(kIoBufferSize));
    if (io_buffer == nullptr) {
        return fail("io buffer");
    }
    AVIOContext* io = avio_alloc_context(
        io_buffer, kIoBufferSize, 0, &reader, read_packet,
        nullptr, seek_packet);
    if (io == nullptr) {
        av_free(io_buffer);
        return fail("io context");
    }
    AVFormatContext* raw_format = avformat_alloc_context();
    if (raw_format == nullptr) {
        av_freep(&io->buffer);
        avio_context_free(&io);
        return fail("format context");
    }
    raw_format->pb = io;
    raw_format->flags |= AVFMT_FLAG_CUSTOM_IO;
    std::unique_ptr<AVFormatContext, FormatContextDeleter> format(
        raw_format);
    int result =
        avformat_open_input(&raw_format, nullptr, nullptr, nullptr);
    format.release();
    format.reset(raw_format);
    if (result < 0 ||
        avformat_find_stream_info(format.get(), nullptr) < 0) {
        return fail("format open");
    }
    const int stream_index = av_find_best_stream(
        format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (stream_index < 0) {
        return fail("audio stream");
    }
    AVStream* stream = format->streams[stream_index];
    const AVCodec* codec =
        avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        return fail("decoder");
    }
    std::unique_ptr<AVCodecContext, CodecContextDeleter> codec_context(
        avcodec_alloc_context3(codec));
    if (!codec_context ||
        avcodec_parameters_to_context(
            codec_context.get(), stream->codecpar) < 0) {
        return fail("codec parameters");
    }
    // Packet PTS/DTS use the demuxer's stream timebase. Vorbis end trimming
    // updates frame timestamps and requires this decoding-side contract.
    codec_context->pkt_timebase = stream->time_base;
    if (avcodec_open2(codec_context.get(), codec, nullptr) < 0) {
        return fail("codec open");
    }
    AVChannelLayout input_layout{};
    if (codec_context->ch_layout.nb_channels != 0) {
        av_channel_layout_copy(
            &input_layout, &codec_context->ch_layout);
    } else {
        av_channel_layout_default(&input_layout, 2);
    }
    const bool transform_pcm = controls &&
        (controls->sample_gain != 1.0 || controls->fade_in_ms != 0);
    ScopedAudioLayout source_layout;
    if (transform_pcm) av_channel_layout_copy(&source_layout.value, &input_layout);
    ScopedAudioLayout output_layout;
    if (transform_pcm) av_channel_layout_copy(&output_layout.value, &input_layout);
    else av_channel_layout_default(&output_layout.value, DecodedAudio::kChannels);
    const uint32_t output_channels = static_cast<uint32_t>(output_layout.value.nb_channels);
    const uint32_t output_rate = transform_pcm ? static_cast<uint32_t>(codec_context->sample_rate) : DecodedAudio::kSampleRate;
    SwrContext* raw_resampler = nullptr;
    result = swr_alloc_set_opts2(
        &raw_resampler, &output_layout.value, AV_SAMPLE_FMT_S16,
        static_cast<int>(output_rate), &input_layout,
        codec_context->sample_fmt, codec_context->sample_rate, 0,
        nullptr);
    av_channel_layout_uninit(&input_layout);
    std::unique_ptr<SwrContext, ResamplerDeleter> resampler(
        raw_resampler);
    if (result < 0 || !resampler ||
        swr_init(resampler.get()) < 0) {
        return fail("resampler");
    }
    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    if (!frame || !packet) {
        return fail("frame allocation");
    }
    DecodedAudio output;
    bool emitted = false;
    size_t published_prefix = 0;
    size_t published_alignment = 0;
    const auto publish = [&]() -> bool {
        if (!sink || output.samples.size() == published_prefix) return true;
        emitted = true;
        const auto pcm = std::span<const int16_t>(output.samples).subspan(published_prefix);
        if (!(*sink)(pcm)) { cancelled = true; return false; }
        // swresample selects scalar/SIMD conversion by destination alignment.
        // A full decode appends at the cumulative PCM offset. Preserve that
        // address residue when reusing bounded scratch, rather than forcing
        // every new packet to a newly aligned beginning (which changes rounding).
        published_alignment = (published_alignment + pcm.size()) % 16U;
        output.samples.resize(published_alignment);
        published_prefix = published_alignment;
        return true;
    };
    const auto consume_frames = [&]() -> bool {
        while (true) {
            result = avcodec_receive_frame(
                codec_context.get(), frame.get());
            if (result == AVERROR(EAGAIN) ||
                result == AVERROR_EOF) {
                return true;
            }
            if (result < 0) {
                return false;
            }
            const int capacity = swr_get_out_samples(
                resampler.get(), frame->nb_samples);
            if (capacity < 0) {
                return false;
            }
            const std::size_t old_size = output.samples.size();
            output.samples.resize(
                old_size +
                static_cast<std::size_t>(capacity) *
                    output_channels);
            auto* destination = reinterpret_cast<uint8_t*>(
                output.samples.data() + old_size);
            const int converted = swr_convert(
                resampler.get(), &destination, capacity,
                const_cast<const uint8_t**>(frame->extended_data),
                frame->nb_samples);
            if (converted < 0) {
                return false;
            }
            output.samples.resize(
                old_size +
                static_cast<std::size_t>(converted) *
                    output_channels);
            av_frame_unref(frame.get());
            if (!publish()) return false;
        }
    };
    while (av_read_frame(format.get(), packet.get()) >= 0) {
        if (packet->stream_index == stream_index) {
            result = avcodec_send_packet(
                codec_context.get(), packet.get());
            av_packet_unref(packet.get());
            if (result < 0 || !consume_frames()) {
                return fail("packet decode");
            }
        } else {
            av_packet_unref(packet.get());
        }
    }
    result = avcodec_send_packet(codec_context.get(), nullptr);
    if (result < 0 || !consume_frames()) {
        return fail("flush decode");
    }
    while (true) {
        const int capacity =
            swr_get_out_samples(resampler.get(), 0);
        if (capacity <= 0) {
            break;
        }
        const std::size_t old_size = output.samples.size();
        output.samples.resize(
            old_size +
            static_cast<std::size_t>(capacity) *
                output_channels);
        auto* destination = reinterpret_cast<uint8_t*>(
            output.samples.data() + old_size);
        const int converted = swr_convert(
            resampler.get(), &destination, capacity, nullptr, 0);
        if (converted <= 0) {
            output.samples.resize(old_size);
            break;
        }
        output.samples.resize(
            old_size +
            static_cast<std::size_t>(converted) *
                output_channels);
    }
    if (!publish()) return fail("incremental sink cancelled");
    if (output.samples.empty() && !emitted) return fail("empty pcm");
    if (transform_pcm) {
        try {
            apply_audio_decode_controls(output.samples, output_rate, output_channels, *controls);
        } catch (const std::exception& e) { return fail(e.what()); }
        // Engine transformations precede the host's channel/rate conversion.
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        SwrContext* converted_raw = nullptr;
        if (swr_alloc_set_opts2(&converted_raw, &stereo, AV_SAMPLE_FMT_S16,
                DecodedAudio::kSampleRate, &source_layout.value, AV_SAMPLE_FMT_S16,
                static_cast<int>(output_rate), 0, nullptr) < 0)
            return fail("controlled PCM output resampler");
        std::unique_ptr<SwrContext, ResamplerDeleter> converted(converted_raw);
        if (!converted || swr_init(converted.get()) < 0) return fail("controlled PCM output init");
        const auto native_frames = output.samples.size() / output_channels;
        if (native_frames > INT32_MAX) return fail("controlled PCM source too large");
        const int capacity = swr_get_out_samples(converted.get(), static_cast<int>(native_frames));
        if (capacity < 0) return fail("controlled PCM capacity");
        DecodedAudio host;
        host.samples.resize(static_cast<std::size_t>(capacity) * DecodedAudio::kChannels);
        const uint8_t* source = reinterpret_cast<const uint8_t*>(output.samples.data());
        uint8_t* destination = reinterpret_cast<uint8_t*>(host.samples.data());
        const int count = swr_convert(converted.get(), &destination, capacity, &source, static_cast<int>(native_frames));
        if (count < 0) return fail("controlled PCM convert");
        host.samples.resize(static_cast<std::size_t>(count) * DecodedAudio::kChannels);
        for (;;) {
            const int remaining = swr_get_out_samples(converted.get(), 0);
            if (remaining <= 0) break;
            const auto old = host.samples.size();
            host.samples.resize(old + static_cast<std::size_t>(remaining) * DecodedAudio::kChannels);
            destination = reinterpret_cast<uint8_t*>(host.samples.data() + old);
            const int written = swr_convert(converted.get(), &destination, remaining, nullptr, 0);
            if (written < 0) return fail("controlled PCM flush");
            host.samples.resize(old + static_cast<std::size_t>(written) * DecodedAudio::kChannels);
            if (!written) break;
        }
        return host;
    }
    return output;
}

std::optional<DecodedAudio> decode_audio(std::span<const uint8_t> encoded,
    std::string* error, const AudioDecodeControls* controls) {
    return decode_audio_impl(encoded, error, controls, nullptr);
}
bool decode_audio_incremental(std::span<const uint8_t> encoded,
    const AudioPcmSink& sink, std::string* error) {
    if (!sink) return false;
    return decode_audio_impl(encoded, error, nullptr, &sink).has_value();
}
}  // namespace bgiplayer
