/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "product_pcm_trace.h"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unistd.h>

struct PcmChannelRecord {
  std::int64_t first = -1;
  std::int64_t last = -1;
  std::uint32_t invalid = 0;
  std::uint32_t gaps = 0;
  std::uint32_t firstBits = 0;
};

struct PcmRecord {
  std::uint64_t observed = 0;
  std::uint64_t time = 0;
  std::uint64_t ticks = 0;
  std::int64_t delay = 0;
  std::uint64_t queued = 0;
  std::uint64_t buffered = 0;
  std::uint32_t queuedBuffers = 0;
  std::uint32_t frames = 0;
  std::array<PcmChannelRecord, 4> channels;
};

struct PcmStreamRecord {
  std::atomic<pw_stream *> stream = nullptr;
  pw_direction direction = PW_DIRECTION_INPUT;
  pw_stream_flags flags = PW_STREAM_FLAG_NONE;
  std::string name;
  std::string node;
  std::atomic<std::size_t> count = 0;
  std::atomic<unsigned> empty = 0;
  std::atomic<unsigned> queueErrors = 0;
  spa_hook listener{};
  struct State {
    std::uint64_t observed;
    pw_stream_state previous;
    pw_stream_state current;
  };
  std::array<State, 128> states;
  std::size_t stateCount = 0;
  std::array<PcmRecord, 4096> records;
};

// Each callback reserves a distinct slot. The observer never reads slots until
// the streams have been destroyed, so recording needs no allocation or lock.
static std::unique_ptr<std::array<PcmStreamRecord, 16>> streams;
static std::filesystem::path directory;
static bool streamOverflow = false;
static decltype(&pw_stream_connect) realConnect = nullptr;
static decltype(&pw_stream_dequeue_buffer) realDequeue = nullptr;
static decltype(&pw_stream_queue_buffer) realQueue = nullptr;
static bool injectOldOutputRate = false;
static std::atomic<bool> injectQueuedOutput = false;
static std::atomic<bool> queuedOutputInjected = false;

void injectProductQueuedOutput() { injectQueuedOutput.store(true); }
bool productQueuedOutputWasInjected() { return queuedOutputInjected.load(); }

void injectProductOutputRateMismatch() {
  injectOldOutputRate = true;
}

void prepareProductPcmTrace() {
  // Resolve once, before the callbacks run. Exported interposition also observes
  // streams inside the unmodified combine module, unlike linker --wrap alone.
  dlerror();
  realConnect = reinterpret_cast<decltype(realConnect)>(dlsym(RTLD_NEXT, "pw_stream_connect"));
  realDequeue = reinterpret_cast<decltype(realDequeue)>(dlsym(RTLD_NEXT, "pw_stream_dequeue_buffer"));
  realQueue = reinterpret_cast<decltype(realQueue)>(dlsym(RTLD_NEXT, "pw_stream_queue_buffer"));
  if (const auto *error = dlerror()) throw std::runtime_error(error);
  if (!realConnect || !realDequeue || !realQueue) throw std::runtime_error("cannot resolve PipeWire stream APIs");
  const auto *path = std::getenv("PIPETUNE_PCM_TRACE_DIRECTORY");
  if (path == nullptr || *path == '\0') return;
  directory = path;
  streams = std::make_unique<std::array<PcmStreamRecord, 16>>();
}

static void registerProductPcmTrace(pw_stream *stream, pw_direction direction, pw_stream_flags flags) {
  if (!streams) return;
  for (auto &item : *streams) {
    if (item.stream.load(std::memory_order_acquire) != nullptr) continue;
    item.direction = direction;
    item.flags = flags;
    item.name = pw_stream_get_name(stream);
    const auto *node = pw_properties_get(pw_stream_get_properties(stream), PW_KEY_NODE_NAME);
    item.node = node == nullptr ? "" : node;
    item.stream.store(stream, std::memory_order_release);
    static const auto events = [] {
      auto value = pw_stream_events{};
      value.version = PW_VERSION_STREAM_EVENTS;
      value.state_changed = [](void *data, pw_stream_state previous, pw_stream_state current, const char *) {
        auto &item = *static_cast<PcmStreamRecord *>(data);
        if (item.stateCount == item.states.size()) return;
        const auto observed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        item.states[item.stateCount++] = {static_cast<std::uint64_t>(observed), previous, current};
      };
      value.destroy = [](void *data) { spa_hook_remove(&static_cast<PcmStreamRecord *>(data)->listener); };
      return value;
    }();
    pw_stream_add_listener(stream, &item.listener, &events, &item);
    return;
  }
  streamOverflow = true;
}

static PcmStreamRecord *findStream(pw_stream *stream) {
  if (streams) for (auto index = streams->size(); index > 0; --index) {
    auto &item = (*streams)[index - 1];
    if (item.stream.load(std::memory_order_acquire) == stream) return &item;
  }
  return nullptr;
}

static std::int64_t tag(float sample) {
  const auto scaled = std::abs(sample) * 268435456.0F;
  if (!std::isfinite(scaled) || scaled < 524288 || scaled >= 1572864 || std::floor(scaled) != scaled)
    return -1;
  return static_cast<std::int64_t>(scaled) % 524288;
}

static void recordPcm(PcmStreamRecord &stream, pw_buffer *buffer) {
  if (buffer == nullptr) {
    stream.empty.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const auto index = stream.count.fetch_add(1, std::memory_order_relaxed);
  if (index >= stream.records.size()) return;
  auto &record = stream.records[index];
  record.observed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  record.time = buffer->time;
  auto time = pw_time{};
  auto *owner = stream.stream.load(std::memory_order_relaxed);
  if (owner != nullptr && pw_stream_get_time_n(owner, &time, sizeof(time)) >= 0) {
    record.ticks = time.ticks;
    record.delay = time.delay;
    record.queued = time.queued;
    record.buffered = time.buffered;
    record.queuedBuffers = time.queued_buffers;
  }
  if (buffer->buffer == nullptr || buffer->buffer->datas == nullptr) return;
  const auto count = std::min<std::size_t>(record.channels.size(), buffer->buffer->n_datas);
  for (auto channel = std::size_t{0}; channel < count; ++channel) {
    const auto &plane = buffer->buffer->datas[channel];
    if (plane.chunk == nullptr || plane.data == nullptr || plane.chunk->stride != sizeof(float) ||
        plane.chunk->offset > plane.maxsize || plane.chunk->size > plane.maxsize - plane.chunk->offset) continue;
    auto &values = record.channels[channel];
    const auto frames = plane.chunk->size / sizeof(float);
    record.frames = static_cast<std::uint32_t>(frames);
    auto previous = std::int64_t{-1};
    for (auto frame = std::size_t{0}; frame < frames; ++frame) {
      auto sample = 0.0F;
      std::memcpy(&sample, static_cast<const char *>(plane.data) + plane.chunk->offset + frame * sizeof(float), sizeof(float));
      const auto current = tag(sample);
      if (frame == 0) { values.first = current; std::memcpy(&values.firstBits, &sample, sizeof(sample)); }
      if (current < 0) ++values.invalid;
      else if (previous >= 0 && current != (previous + 1) % 524287) ++values.gaps;
      values.last = current;
      previous = current;
    }
  }
}

extern "C" int pw_stream_connect(pw_stream *stream, pw_direction direction, std::uint32_t target,
    pw_stream_flags flags, const spa_pod **parameters, std::uint32_t count) {
  registerProductPcmTrace(stream, direction, flags);
  if (injectOldOutputRate && direction == PW_DIRECTION_OUTPUT && count == 1 &&
      pw_properties_get(pw_stream_get_properties(stream), "node.pipetune.output-id") != nullptr) {
    auto format = spa_audio_info_raw{};
    if (spa_format_audio_raw_parse(parameters[0], &format) < 0) return -EINVAL;
    injectOldOutputRate = false;
    // Reproduce a leaf retaining its old negotiated rate. Subsequent stream
    // generations use the real negotiation, so recovery must come from PipeTune.
    format.rate = 48000;
    const auto item = spa_dict_item{"resample.disable", "false"};
    const auto properties = SPA_DICT_INIT(&item, 1);
    const auto updated = pw_stream_update_properties(stream, &properties);
    if (updated < 0) return updated;
    auto storage = std::array<std::uint8_t, 512>{};
    auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    const auto *parameter = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format);
    return realConnect(stream, direction, target, flags, &parameter, 1);
  }
  return realConnect(stream, direction, target, flags, parameters, count);
}

extern "C" pw_buffer *pw_stream_dequeue_buffer(pw_stream *stream) {
  auto *buffer = realDequeue(stream);
  if (auto *record = findStream(stream); record != nullptr && record->direction == PW_DIRECTION_INPUT)
    recordPcm(*record, buffer);
  return buffer;
}

extern "C" int pw_stream_queue_buffer(pw_stream *stream, pw_buffer *buffer) {
  auto *record = findStream(stream);
  if (record != nullptr && record->direction == PW_DIRECTION_OUTPUT) recordPcm(*record, buffer);
  auto *injected = static_cast<pw_buffer *>(nullptr);
  auto validInjection = false;
  if (injectQueuedOutput.load(std::memory_order_relaxed) && buffer->buffer &&
      buffer->buffer->n_datas == 2) {
    const auto *output = pw_properties_get(pw_stream_get_properties(stream), "node.pipetune.output-id");
    const auto &first = buffer->buffer->datas[0];
    if (output && std::string_view(output) == "pipetune_product_device_1" && first.data && first.chunk &&
        first.chunk->offset + sizeof(float) <= first.maxsize && first.chunk->size >= sizeof(float)) {
      auto sample = 0.0F;
      std::memcpy(&sample, static_cast<const char *>(first.data) + first.chunk->offset, sizeof(sample));
      if (sample > 0.24F && injectQueuedOutput.exchange(false)) {
        auto *extra = realDequeue(stream);
        if (extra) {
          injected = extra;
          auto valid = extra->buffer && extra->buffer->n_datas == 2;
          for (auto channel = 0U; valid && channel < 2; ++channel) {
            const auto &source = buffer->buffer->datas[channel];
            const auto &destination = extra->buffer->datas[channel];
            valid = source.data && source.chunk && destination.data && destination.chunk &&
                source.chunk->offset <= source.maxsize && source.chunk->size <= source.maxsize - source.chunk->offset &&
                source.chunk->size <= destination.maxsize;
          }
          if (valid) {
            for (auto channel = 0U; channel < 2; ++channel) {
              const auto &source = buffer->buffer->datas[channel];
              auto &destination = extra->buffer->datas[channel];
              std::memcpy(destination.data, static_cast<const char *>(source.data) + source.chunk->offset, source.chunk->size);
              *destination.chunk = *source.chunk;
              destination.chunk->offset = 0;
            }
            extra->size = buffer->size;
          } else {
            extra->size = 0;
            if (extra->buffer) for (auto channel = 0U; channel < extra->buffer->n_datas; ++channel)
              if (auto *chunk = extra->buffer->datas[channel].chunk) chunk->size = 0;
          }
          validInjection = valid;
        }
      }
    }
  }
  const auto result = realQueue(stream, buffer);
  if (record != nullptr && result < 0) record->queueErrors.fetch_add(1, std::memory_order_relaxed);
  if (injected) queuedOutputInjected.store(realQueue(stream, injected) >= 0 && validInjection);
  return result;
}

void finishProductPcmTrace() {
  if (!streams) return;
  auto output = std::ofstream(directory / (std::to_string(getpid()) + ".json"));
  output << "{\"streamOverflow\":" << (streamOverflow ? "true" : "false") << ",\"streams\":[";
  auto separator = "";
  for (const auto &stream : *streams) {
    if (stream.stream.load() == nullptr) continue;
    const auto count = stream.count.load();
    output << separator << "{\"name\":" << std::quoted(stream.name) << ",\"node\":" << std::quoted(stream.node) << ",\"direction\":" << stream.direction
           << ",\"flags\":" << stream.flags << ",\"count\":" << count << ",\"empty\":" << stream.empty.load()
           << ",\"queueErrors\":" << stream.queueErrors.load() << ",\"overflow\":"
           << (count > stream.records.size() ? "true" : "false") << ",\"states\":[";
    for (auto index = std::size_t{0}; index < stream.stateCount; ++index) {
      if (index) output << ',';
      const auto &state = stream.states[index];
      output << '[' << state.observed << ',' << state.previous << ',' << state.current << ']';
    }
    output << "],\"records\":[";
    for (auto index = std::size_t{0}; index < std::min(count, stream.records.size()); ++index) {
      const auto &record = stream.records[index];
      if (index != 0) output << ',';
      output << "{\"observed\":" << record.observed << ",\"time\":" << record.time << ",\"ticks\":" << record.ticks
             << ",\"delay\":" << record.delay << ",\"queued\":" << record.queued << ",\"buffered\":" << record.buffered
             << ",\"queuedBuffers\":" << record.queuedBuffers << ",\"frames\":" << record.frames << ",\"channels\":[";
      for (auto channel = std::size_t{0}; channel < record.channels.size(); ++channel) {
        const auto &values = record.channels[channel];
        if (channel != 0) output << ',';
        output << '[' << values.first << ',' << values.last << ',' << values.invalid << ',' << values.gaps << ',' << values.firstBits << ']';
      }
      output << "]}";
    }
    output << "]}";
    separator = ",";
  }
  output << "]}\n";
  if (!output) throw std::runtime_error("cannot write the PCM trace");
}

int verifyProductPcmTrace() {
  auto trace = std::make_unique<PcmStreamRecord>();
  auto samples = std::array<float, 512>{};
  auto chunk = spa_chunk{.offset = 0, .size = sizeof(samples), .stride = sizeof(float), .flags = 0};
  auto data = spa_data{.type = SPA_DATA_MemPtr, .flags = 0, .fd = -1, .mapoffset = 0,
      .maxsize = sizeof(samples), .data = samples.data(), .chunk = &chunk};
  auto storage = spa_buffer{.n_metas = 0, .n_datas = 1, .metas = nullptr, .datas = &data};
  auto buffer = pw_buffer{.buffer = &storage, .user_data = nullptr, .size = samples.size(), .requested = 0, .time = 123};
  for (auto frame = 0U; frame < samples.size(); ++frame)
    samples[frame] = static_cast<float>(524288 + frame) / 268435456.0F;
  recordPcm(*trace, &buffer);
  const auto &continuous = trace->records[0].channels[0];
  if (trace->records[0].frames != 512 || continuous.first != 0 || continuous.last != 511 ||
      continuous.invalid != 0 || continuous.gaps != 0) return 1;
  // Drop precisely one quantum in the middle of this boundary's PCM.
  for (auto frame = 256U; frame < samples.size(); ++frame)
    samples[frame] = static_cast<float>(524288 + frame + 256) / 268435456.0F;
  recordPcm(*trace, &buffer);
  const auto &missing = trace->records[1].channels[0];
  if (missing.first != 0 || missing.last != 767 || missing.invalid != 0 || missing.gaps != 1) return 2;
  samples[0] = std::numeric_limits<float>::quiet_NaN();
  recordPcm(*trace, &buffer);
  if (trace->records[2].channels[0].first != -1 || trace->records[2].channels[0].invalid != 1) return 3;
  recordPcm(*trace, nullptr);
  if (trace->empty.load() != 1) return 4;
  trace->count.store(trace->records.size());
  recordPcm(*trace, &buffer);
  if (trace->count.load() != trace->records.size() + 1 || trace->records[0].channels[0].first != 0) return 5;
  return 0;
}
