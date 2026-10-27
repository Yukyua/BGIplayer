#include "bgiplayer/movie_backend.hpp"
#include "bgiplayer/debug.hpp"
#include "bgiplayer/decode_warnings.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <cstdio>
#include <cstring>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace bgiplayer {
namespace {

struct MemoryReader {
    std::span<const uint8_t> data;
    std::size_t position = 0;
};

int read_packet(void* opaque, uint8_t* buffer, int size) {
    auto& reader = *static_cast<MemoryReader*>(opaque);
    if (size <= 0 || reader.position >= reader.data.size()) {
        return AVERROR_EOF;
    }
    const std::size_t count = std::min<std::size_t>(
        reader.data.size() - reader.position,
        static_cast<std::size_t>(size));
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
    if (whence == SEEK_SET) {
        target = offset;
    } else if (whence == SEEK_CUR) {
        target = static_cast<int64_t>(reader.position) + offset;
    } else if (whence == SEEK_END) {
        target = static_cast<int64_t>(reader.data.size()) + offset;
    } else {
        return AVERROR(EINVAL);
    }
    if (target < 0 ||
        static_cast<uint64_t>(target) > reader.data.size()) {
        return AVERROR(EINVAL);
    }
    reader.position = static_cast<std::size_t>(target);
    return target;
}

}  // namespace

struct MoviePlayback::Impl {
    std::vector<uint8_t> encoded_bytes;
    std::string failure_reason;
    MemoryReader reader;
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    AVCodecContext* codec = nullptr;
    SwsContext* scaler = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* transferred_frame = nullptr;
    AVBufferRef* hardware_device = nullptr;
    AVPixelFormat hardware_format = AV_PIX_FMT_NONE;
    Statistics statistics;
    int64_t last_requested_ms = -1;
    static AVPixelFormat choose_format(
        AVCodecContext* context, const AVPixelFormat* formats) {
        const auto* self = static_cast<const Impl*>(context->opaque);
        if (debug_trace(DebugTrace::Movie)) {
            std::cout << "MOVIE_FORMATS";
            for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p)
                std::cout << ' ' << av_get_pix_fmt_name(*p);
            std::cout << '\n';
        }
        for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == self->hardware_format) return *p;
        for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p) {
            const auto* description = av_pix_fmt_desc_get(*p);
            if (description && !(description->flags & AV_PIX_FMT_FLAG_HWACCEL))
                return *p;
        }
        return AV_PIX_FMT_NONE;
    }
    AVPacket* packet = nullptr;
    int stream_index = -1;
    int width = 0;
    int height = 0;
    int64_t duration_ms = 0;
    int64_t current_ms = -1;
    bool demux_eof = false;
    bool decoder_eof = false;
    std::vector<uint8_t> rgba;

    ~Impl() { close(); }

    void close() {
        av_frame_free(&transferred_frame);
        av_buffer_unref(&hardware_device);
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
        if (scaler != nullptr) {
            sws_freeContext(scaler);
            scaler = nullptr;
        }
        if (codec != nullptr) {
            avcodec_free_context(&codec);
        }
        if (format != nullptr) {
            avformat_close_input(&format);
        }
        // avformat_open_input may free its format context on failure, but
        // custom AVIO remains the caller's responsibility in either case.
        if (io != nullptr) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }

    bool initialize(std::span<const uint8_t> encoded, bool allow_hardware = true) {
        failure_reason.clear();
        const auto fail = [&](std::string_view reason) { failure_reason = reason; return false; };
        // AVIO reads lazily after open() returns. Own the encoded bytes for
        // the entire decoder lifetime, including seeks and delayed packets.
        encoded_bytes.assign(encoded.begin(), encoded.end());
        reader.data = encoded_bytes;
        reader.position = 0;
        constexpr int kIoBufferSize = 32768;
        auto* buffer = static_cast<uint8_t*>(av_malloc(kIoBufferSize));
        if (buffer == nullptr) {
            return fail("movie AVIO buffer allocation failed");
        }
        io = avio_alloc_context(
            buffer, kIoBufferSize, 0, &reader, read_packet,
            nullptr, seek_packet);
        if (io == nullptr) {
            av_free(buffer);
            return fail("movie AVIO context allocation failed");
        }
        format = avformat_alloc_context();
        if (format == nullptr) {
            av_freep(&io->buffer);
            avio_context_free(&io);
            return fail("movie format context allocation failed");
        }
        format->pb = io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (avformat_open_input(&format, nullptr, nullptr, nullptr) < 0 ||
            avformat_find_stream_info(format, nullptr) < 0) {
            close();
            return fail("movie container could not be opened or its streams could not be read");
        }
        stream_index = av_find_best_stream(
            format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream_index < 0) {
            close();
            return fail("movie container has no decodable video stream");
        }
        AVStream* stream = format->streams[stream_index];
        const AVCodec* decoder =
            avcodec_find_decoder(stream->codecpar->codec_id);
        if (decoder == nullptr) {
            close();
            return fail("movie video codec is not supported by the available FFmpeg decoders");
        }
        codec = avcodec_alloc_context3(decoder);
        if (codec == nullptr || avcodec_parameters_to_context(codec, stream->codecpar) < 0) {
            close(); return fail("movie codec parameters could not be initialized");
        }
        codec->thread_count = 0;
        if (allow_hardware && std::getenv("BGI_MOVIE_SOFTWARE") == nullptr) {
            for (int index = 0;; ++index) {
                const auto* config = avcodec_get_hw_config(decoder, index);
                if (!config) break;
                if (config->device_type != AV_HWDEVICE_TYPE_VIDEOTOOLBOX ||
                    (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) == 0)
                    continue;
                const int result = av_hwdevice_ctx_create(
                    &hardware_device, config->device_type, nullptr, nullptr, 0);
                if (result < 0) {
                    if (debug_trace(DebugTrace::Movie)) {
                        char error[AV_ERROR_MAX_STRING_SIZE]{};
                        av_strerror(result, error, sizeof(error));
                        std::cout << "MOVIE_ACCELERATION_UNAVAILABLE " << error << '\n';
                    }
                    continue;
                }
                hardware_format = config->pix_fmt;
                codec->hw_device_ctx = av_buffer_ref(hardware_device);
                codec->opaque = this;
                codec->get_format = &choose_format;
                break;
            }
        }
        if (avcodec_open2(codec, decoder, nullptr) < 0) {
            close();
            return fail("movie video codec could not be opened");
        }
        width = codec->width;
        height = codec->height;
        duration_ms = format->duration != AV_NOPTS_VALUE
                          ? format->duration * 1000 /
                                AV_TIME_BASE
                          : 0;
        if (duration_ms <= 0 && stream->duration != AV_NOPTS_VALUE) {
            duration_ms = av_rescale_q(
                stream->duration, stream->time_base,
                AVRational{1, 1000});
        }
        frame = av_frame_alloc();
        transferred_frame = av_frame_alloc();
        packet = av_packet_alloc();
        if (width <= 0 || height <= 0 || frame == nullptr || packet == nullptr || transferred_frame == nullptr)
            return fail("movie dimensions are invalid or frame/packet allocation failed");
        return true;
    }

    bool accept_frame() {
        AVFrame* source = frame;
        if (frame->format == hardware_format && hardware_format != AV_PIX_FMT_NONE) {
            av_frame_unref(transferred_frame);
            if (av_hwframe_transfer_data(transferred_frame, frame, 0) < 0) {
                failure_reason = "movie hardware frame transfer failed";
                return false;
            }
            source = transferred_frame;
            statistics.hardware_frames = true;
        }
        scaler = sws_getCachedContext(
            scaler, width, height, static_cast<AVPixelFormat>(source->format),
            width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler) {
            failure_reason = "movie decoded pixel format cannot be converted to RGBA";
            return false;
        }
        rgba.resize(
            static_cast<std::size_t>(width) * height * 4U);
        uint8_t* destination[4] = {rgba.data(), nullptr, nullptr, nullptr};
        const int destination_stride[4] = {width * 4, 0, 0, 0};
        sws_scale(
            scaler, source->data, source->linesize, 0, height,
            destination, destination_stride);
        ++statistics.decoded_frames;
        current_ms = frame->best_effort_timestamp == AV_NOPTS_VALUE
                         ? current_ms
                         : av_rescale_q(
                               frame->best_effort_timestamp,
                               format->streams[stream_index]->time_base,
                               AVRational{1, 1000});
        return true;
    }

    bool decode_next() {
        failure_reason.clear();
        while (!decoder_eof) {
            const int receive =
                avcodec_receive_frame(codec, frame);
            if (receive == 0) {
                const bool accepted = accept_frame();
                av_frame_unref(frame);
                return accepted;
            }
            if (receive == AVERROR_EOF) {
                decoder_eof = true;
                return false;
            }
            if (receive != AVERROR(EAGAIN)) {
                failure_reason = "movie video decoder rejected a frame, status=" + std::to_string(receive);
                return false;
            }
            if (demux_eof) {
                if (avcodec_send_packet(codec, nullptr) < 0) {
                    decoder_eof = true;
                }
                continue;
            }
            int read = av_read_frame(format, packet);
            if (read < 0) {
                demux_eof = true;
                (void)avcodec_send_packet(codec, nullptr);
                continue;
            }
            if (packet->stream_index == stream_index) {
                read = avcodec_send_packet(codec, packet);
            }
            av_packet_unref(packet);
            if (read < 0 && read != AVERROR(EAGAIN)) {
                failure_reason = "movie video decoder rejected a packet, status=" + std::to_string(read);
                return false;
            }
        }
        return false;
    }

    bool seek_zero() {
        if (av_seek_frame(format, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
            failure_reason = "movie decoder could not seek to the first frame";
            return false;
        }
        ++statistics.seeks;
        avcodec_flush_buffers(codec);
        demux_eof = false;
        decoder_eof = false;
        current_ms = -1;
        rgba.clear();
        return true;
    }
};

MoviePlayback::MoviePlayback()
    : impl_(std::make_unique<Impl>()) {}

MoviePlayback::~MoviePlayback() = default;
MoviePlayback::MoviePlayback(MoviePlayback&&) noexcept = default;
MoviePlayback& MoviePlayback::operator=(MoviePlayback&&) noexcept =
    default;

bool MoviePlayback::open(std::span<const uint8_t> encoded) {
    auto next = std::make_unique<Impl>();
    if (!next->initialize(encoded) ||
        (next->hardware_device && !next->decode_next())) {
        next = std::make_unique<Impl>();
        if (!next->initialize(encoded, false)) {
            warn_decode_failure("movie", next->failure_reason, encoded);
            return false;
        }
    }
    static std::atomic<uint64_t> next_stream{0};
    next->statistics.stream_id = ++next_stream;
    if (debug_trace(DebugTrace::Movie))
        std::cout << "MOVIE_DECODER codec=" << next->codec->codec->name
                  << " acceleration=" << (next->statistics.hardware_frames ? "videotoolbox" : "software") << '\n';
    impl_ = std::move(next);
    return true;
}

std::optional<int64_t> MoviePlayback::frame_at(
    int64_t position_ms) {
    if (!impl_ || impl_->format == nullptr || impl_->codec == nullptr ||
        impl_->frame == nullptr || impl_->packet == nullptr || position_ms < 0) {
        return std::nullopt;
    }
    if (impl_->last_requested_ms >= 0 &&
        position_ms < impl_->last_requested_ms &&
        !impl_->seek_zero()) {
        warn_decode_failure("movie", impl_->failure_reason, impl_->encoded_bytes);
        return std::nullopt;
    }
    impl_->last_requested_ms = position_ms;
    while (impl_->current_ms < 0 ||
           impl_->current_ms < position_ms) {
        const int64_t previous = impl_->current_ms;
        if (!impl_->decode_next()) {
            break;
        }
        if (impl_->current_ms < 0) {
            impl_->current_ms = previous;
        }
        if (impl_->current_ms > position_ms) {
            break;
        }
    }
    if (!impl_->failure_reason.empty())
        warn_decode_failure("movie", impl_->failure_reason, impl_->encoded_bytes);
    return impl_->current_ms;
}

MoviePlayback::Statistics MoviePlayback::statistics() const {
    return impl_ ? impl_->statistics : Statistics{};
}

int64_t MoviePlayback::duration_ms() const {
    return impl_ ? impl_->duration_ms : 0;
}

int MoviePlayback::width() const {
    return impl_ ? impl_->width : 0;
}

int MoviePlayback::height() const {
    return impl_ ? impl_->height : 0;
}

const std::vector<uint8_t>& MoviePlayback::rgba() const {
    static const std::vector<uint8_t> empty;
    return impl_ ? impl_->rgba : empty;
}

}  // namespace bgiplayer
