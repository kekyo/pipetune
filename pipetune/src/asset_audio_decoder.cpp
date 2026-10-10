/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "asset_audio_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <samplerate.h>

namespace pipetune {

struct AudioDecodeSession {
  std::span<const std::uint8_t> bytes;
  std::int64_t position = 0;
  AVIOContext *io = nullptr;
  AVFormatContext *format = nullptr;
  AVCodecContext *codec = nullptr;
  AVPacket *packet = nullptr;
  AVFrame *frame = nullptr;
  SwrContext *conversion = nullptr;

  ~AudioDecodeSession() {
    swr_free(&conversion);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
    if (io != nullptr) {
      // avio may replace its buffer; release the current buffer before the context.
      av_freep(&io->buffer);
      avio_context_free(&io);
    }
  }
};

static int readAudioMemory(void *opaque, std::uint8_t *buffer, int capacity) {
  auto &state = *static_cast<AudioDecodeSession *>(opaque);
  const auto count = std::min<std::int64_t>(capacity, state.bytes.size() - state.position);
  if (count <= 0) return AVERROR_EOF;
  std::memcpy(buffer, state.bytes.data() + state.position, count);
  state.position += count;
  return static_cast<int>(count);
}

static std::int64_t seekAudioMemory(void *opaque, std::int64_t offset, int whence) {
  auto &state = *static_cast<AudioDecodeSession *>(opaque);
  if (whence == AVSEEK_SIZE) return static_cast<std::int64_t>(state.bytes.size());
  const auto mode = whence & ~AVSEEK_FORCE;
  const auto base = mode == SEEK_SET ? 0 : mode == SEEK_CUR ? state.position :
                    mode == SEEK_END ? static_cast<std::int64_t>(state.bytes.size()) : -1;
  if (base < 0 || offset < -base ||
      offset > static_cast<std::int64_t>(state.bytes.size()) - base) return AVERROR(EINVAL);
  state.position = base + offset;
  return state.position;
}

static int rejectAudioUrl(AVFormatContext *, AVIOContext **, const char *, int, AVDictionary **) {
  return AVERROR(EACCES);
}

static void requireDecoded(int result, const char *operation) {
  if (result >= 0) return;
  auto message = std::array<char, AV_ERROR_MAX_STRING_SIZE>{};
  av_strerror(result, message.data(), message.size());
  throw std::runtime_error(std::string(operation) + ": " + message.data());
}

assets::Audio decodeAssetAudio(std::span<const std::uint8_t> bytes,
                              std::size_t maximumPcmBytes) {
  if (bytes.empty() || bytes.size() > 64u * 1024u * 1024u)
    throw std::runtime_error("audio original exceeds the 64 MiB limit or is empty");
  auto state = AudioDecodeSession{.bytes = bytes};
  auto *buffer = static_cast<std::uint8_t *>(av_malloc(32768));
  if (buffer == nullptr) throw std::bad_alloc();
  state.io = avio_alloc_context(buffer, 32768, 0, &state, readAudioMemory, nullptr, seekAudioMemory);
  if (state.io == nullptr) {
    av_free(buffer);
    throw std::bad_alloc();
  }
  state.format = avformat_alloc_context();
  if (state.format == nullptr) throw std::bad_alloc();
  state.format->pb = state.io;
  state.format->flags |= AVFMT_FLAG_CUSTOM_IO;
  state.format->io_open = rejectAudioUrl;
  state.format->format_whitelist = av_strdup("wav,aiff,flac,mp3,ogg,mov");
  if (state.format->format_whitelist == nullptr) throw std::bad_alloc();
  requireDecoded(avformat_open_input(&state.format, nullptr, nullptr, nullptr), "cannot open audio");
  requireDecoded(avformat_find_stream_info(state.format, nullptr), "cannot inspect audio");
  const auto stream = av_find_best_stream(state.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  requireDecoded(stream, "audio stream is unavailable");
  const auto *parameters = state.format->streams[stream]->codecpar;
  const auto channels = parameters->ch_layout.nb_channels;
  const auto rate = parameters->sample_rate;
  if (channels < 1 || channels > 16 || rate < 1000 || rate > 999999)
    throw std::runtime_error("audio channel count or sample rate is unsupported");
  const auto *decoder = avcodec_find_decoder(parameters->codec_id);
  if (decoder == nullptr) throw std::runtime_error("audio decoder is unavailable");
  state.codec = avcodec_alloc_context3(decoder);
  if (state.codec == nullptr) throw std::bad_alloc();
  requireDecoded(avcodec_parameters_to_context(state.codec, parameters), "cannot configure audio decoder");
  state.codec->thread_count = 1;
  state.codec->max_samples = static_cast<std::int64_t>(maximumPcmBytes / sizeof(float));
  requireDecoded(avcodec_open2(state.codec, decoder, nullptr), "cannot prepare audio decoder");
  state.packet = av_packet_alloc();
  state.frame = av_frame_alloc();
  if (state.packet == nullptr || state.frame == nullptr) throw std::bad_alloc();
  auto audio = assets::Audio{.sampleRate = static_cast<std::uint32_t>(rate),
                            .channels = std::vector<std::vector<float>>(channels)};
  const auto maximumFrames = maximumPcmBytes / sizeof(float) / channels;
  auto format = -1;
  const auto receive = [&]() {
    for (;;) {
      const auto status = avcodec_receive_frame(state.codec, state.frame);
      if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) break;
      requireDecoded(status, "cannot decode audio frame");
      if (state.frame->ch_layout.nb_channels != channels || state.frame->sample_rate != rate ||
          state.frame->nb_samples < 0 || (format >= 0 && state.frame->format != format))
        throw std::runtime_error("audio format changes within the original");
      const auto count = static_cast<std::size_t>(state.frame->nb_samples);
      const auto offset = audio.channels.front().size();
      if (count > maximumFrames || offset > maximumFrames - count)
        throw std::runtime_error("decoded audio exceeds its PCM budget");
      if (state.conversion == nullptr) {
        format = state.frame->format;
        requireDecoded(swr_alloc_set_opts2(&state.conversion, &state.frame->ch_layout,
            AV_SAMPLE_FMT_FLTP, rate, &state.frame->ch_layout,
            static_cast<AVSampleFormat>(format), rate, 0, nullptr), "cannot configure PCM conversion");
        requireDecoded(swr_init(state.conversion), "cannot initialize PCM conversion");
      }
      auto output = std::array<std::uint8_t *, 16>{};
      auto input = std::array<const std::uint8_t *, 16>{};
      const auto planes = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(format)) ? channels : 1;
      for (auto channel = 0; channel < channels; ++channel) {
        audio.channels[channel].resize(offset + count);
        output[channel] = reinterpret_cast<std::uint8_t *>(audio.channels[channel].data() + offset);
      }
      for (auto plane = 0; plane < planes; ++plane) input[plane] = state.frame->extended_data[plane];
      // Input and output layouts/rates are identical: this only converts sample format.
      const auto converted = swr_convert(state.conversion, output.data(), state.frame->nb_samples,
                                         input.data(), state.frame->nb_samples);
      requireDecoded(converted, "cannot convert PCM");
      if (converted != state.frame->nb_samples)
        throw std::runtime_error("PCM conversion changed the original frame count");
      for (const auto &channel : audio.channels)
        for (auto frame = offset; frame < offset + count; ++frame)
          if (!std::isfinite(channel[frame])) throw std::runtime_error("audio contains non-finite PCM");
    }
  };
  for (;;) {
    const auto status = av_read_frame(state.format, state.packet);
    if (status == AVERROR_EOF) break;
    requireDecoded(status, "cannot read audio packet");
    if (state.packet->stream_index == stream) {
      requireDecoded(avcodec_send_packet(state.codec, state.packet), "cannot submit audio packet");
      receive();
    }
    av_packet_unref(state.packet);
  }
  requireDecoded(avcodec_send_packet(state.codec, nullptr), "cannot finish audio decoding");
  receive();
  if (audio.channels.front().empty()) throw std::runtime_error("audio original has no frames");
  return audio;
}

std::string assetDecoderVersion() {
  return std::to_string(avformat_version()) + "/" + std::to_string(avcodec_version()) +
         "/" + std::to_string(avutil_version()) + "/" + std::to_string(swresample_version()) +
         "/" + src_get_version();
}

assets::Audio resampleIrAudio(assets::Audio audio, std::uint32_t sampleRate,
                            std::size_t maximumPcmBytes) {
  if (audio.sampleRate == 0 || sampleRate == 0 || audio.channels.empty() ||
      audio.channels.size() > 16 || audio.channels.front().empty())
    throw std::runtime_error("invalid IR sample-rate conversion input");
  const auto sourceFrames = audio.channels.front().size();
  const auto ratio = static_cast<double>(sampleRate) / audio.sampleRate;
  const auto outputFrames = std::max(1.0, std::floor(sourceFrames * ratio + 0.5));
  if (outputFrames > maximumPcmBytes / sizeof(float) / audio.channels.size())
    throw std::runtime_error("resampled IR exceeds its PCM budget");
  if (audio.sampleRate == sampleRate) return audio;
  // libsamplerate supports ratios through 256. Rare lower-rate originals use
  // a bounded intermediate stage, whose output cannot exceed the final budget.
  if (!src_is_valid_ratio(ratio)) {
    const auto intermediate = ratio > 1 ? audio.sampleRate * 64u : std::max(1u, audio.sampleRate / 64u);
    return resampleIrAudio(resampleIrAudio(std::move(audio), intermediate, maximumPcmBytes),
                           sampleRate, maximumPcmBytes);
  }
  auto output = assets::Audio{.sampleRate = sampleRate, .channels = {}};
  for (const auto &source : audio.channels) {
    if (source.size() != sourceFrames) throw std::runtime_error("IR channel lengths differ");
    auto &channel = output.channels.emplace_back(static_cast<std::size_t>(outputFrames), 0.0F);
    auto data = SRC_DATA{.data_in = source.data(), .data_out = channel.data(),
        .input_frames = static_cast<long>(sourceFrames), .output_frames = static_cast<long>(channel.size()),
        .input_frames_used = 0, .output_frames_gen = 0, .end_of_input = 1, .src_ratio = ratio};
    const auto status = src_simple(&data, SRC_SINC_BEST_QUALITY, 1);
    if (status != 0) throw std::runtime_error(std::string("cannot resample IR: ") + src_strerror(status));
    if (data.output_frames_gen + 1 < data.output_frames || data.output_frames_gen > data.output_frames)
      throw std::runtime_error("IR resampler returned an unexpected duration");
    for (const auto sample : channel)
      if (!std::isfinite(sample)) throw std::runtime_error("IR resampling produced non-finite PCM");
  }
  return output;
}

} // namespace pipetune
