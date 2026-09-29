/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "crosstalk_fir.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <numeric>

namespace pipetune {

// Implements the v2.8.0 measurement-to-filter contract, independently of the
// browser's workers/storage. Keep numerical behavior covered by the upstream
// JS oracle: https://github.com/Frieve-A/effetune/blob/v2.8.0/js/crosstalk-cancellation/design-core.js
using Complex = std::complex<double>;
static constexpr double pi = std::numbers::pi;
static constexpr double epsilon = 1e-12;

static double parameter(yyjson_val *parameters, const char *key,
                        double low, double high, double fallback) {
  auto *value = yyjson_obj_get(parameters, key);
  if (!yyjson_is_num(value)) return fallback;
  const auto number = yyjson_get_num(value);
  return std::isfinite(number) ? std::clamp(number, low, high) : fallback;
}

// Double precision is intentional: inverse plant design is more sensitive than
// audio convolution. The radix-2 transform uses the same normalization as the
// upstream Float64 FFT, without crossing a float32 native FFT boundary.
static void transform(std::vector<Complex> &values, bool inverse) {
  const auto n = values.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    auto bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(values[i], values[j]);
  }
  for (std::size_t length = 2; length <= n; length *= 2) {
    const auto step = std::polar(1.0, (inverse ? 2 : -2) * pi / length);
    for (std::size_t begin = 0; begin < n; begin += length) {
      auto weight = Complex{1, 0};
      for (std::size_t j = 0; j < length / 2; ++j) {
        const auto even = values[begin + j];
        const auto odd = values[begin + j + length / 2] * weight;
        values[begin + j] = even + odd;
        values[begin + j + length / 2] = even - odd;
        weight *= step;
      }
    }
  }
  if (inverse) for (auto &value : values) value /= static_cast<double>(n);
}

static double bessel(double value) {
  double sum = 1, term = 1;
  for (int index = 1; index < 20; ++index) {
    term *= value * value / (4 * index * index);
    sum += term;
    if (term < sum * epsilon) break;
  }
  return sum;
}

static std::vector<float> resample(const std::vector<float> &input,
                                   unsigned sourceRate, unsigned targetRate,
                                   unsigned radius) {
  if (sourceRate == targetRate) return input;
  const auto divisor = std::gcd(sourceRate, targetRate);
  const auto sourceStep = sourceRate / divisor, phaseCount = targetRate / divisor;
  const auto cutoff = std::min(1.0, double(targetRate) / sourceRate) * 0.95;
  const auto beta = 0.1102 * (100 - 8.7), normalizer = bessel(beta);
  auto phases = std::vector<std::vector<double>>(phaseCount);
  auto output = std::vector<float>(std::max<std::size_t>(1, std::llround(double(input.size()) * targetRate / sourceRate)));
  for (std::size_t i = 0; i < output.size(); ++i) {
    const auto center = static_cast<std::int64_t>(i * sourceStep / phaseCount);
    const auto phase = i * sourceStep % phaseCount;
    auto &weights = phases[phase];
    if (weights.empty()) {
      weights.resize(2u * radius);
      double total = 0;
      for (std::size_t tap = 0; tap < weights.size(); ++tap) {
        const auto distance = double(phase) / phaseCount - (double(tap) - radius + 1);
        const auto normalized = distance / radius;
        if (normalized <= -1 || normalized >= 1) continue;
        const auto x = pi * distance * cutoff;
        weights[tap] = cutoff * (x == 0 ? 1 : std::sin(x) / x) *
            bessel(beta * std::sqrt(1 - normalized * normalized)) / normalizer;
        total += weights[tap];
      }
      if (total != 0) for (auto &weight : weights) weight /= total;
    }
    const auto first = center - radius + 1;
    double weighted = 0, total = 0;
    for (std::size_t tap = 0; tap < weights.size(); ++tap) {
      const auto index = first + static_cast<std::int64_t>(tap);
      if (index < 0 || index >= static_cast<std::int64_t>(input.size())) continue;
      weighted += input[index] * weights[tap];
      total += weights[tap];
    }
    const auto interior = first >= 0 && first + weights.size() <= input.size();
    output[i] = static_cast<float>(interior ? weighted : total == 0 ? 0 : weighted / total);
  }
  return output;
}

static std::vector<Complex> smooth(const std::vector<Complex> &spectrum,
                                   std::size_t fftSize, double sampleRate,
                                   double delaySeconds) {
  const auto count = fftSize / 2 + 1;
  auto prefix = std::vector<Complex>(count + 1);
  for (std::size_t bin = 0; bin < count; ++bin)
    prefix[bin + 1] = prefix[bin] + spectrum[bin] * std::polar(1.0, 2 * pi * bin * sampleRate / fftSize * delaySeconds);
  auto output = std::vector<Complex>(count);
  const auto lower = std::exp2(-1.0 / 12), upper = std::exp2(1.0 / 12);
  for (std::size_t bin = 0; bin < count; ++bin) {
    const auto first = bin == 0 ? 0 : std::max<std::size_t>(1, std::ceil(bin * lower));
    const auto last = bin == 0 ? 0 : std::min<std::size_t>(count - 1, std::floor(bin * upper));
    output[bin] = (prefix[last + 1] - prefix[first]) / double(last - first + 1) *
        std::polar(1.0, -2 * pi * bin * sampleRate / fftSize * delaySeconds);
  }
  output.front().imag(0); output.back().imag(0);
  return output;
}

static std::array<double, 2> singularSquared(Complex ll, Complex lr, Complex rl, Complex rr) {
  const auto trace = std::norm(ll) + std::norm(lr) + std::norm(rl) + std::norm(rr);
  const auto root = std::sqrt(std::max(0.0, trace * trace - 4 * std::norm(ll * rr - rl * lr)));
  return {(trace + root) / 2, std::max(0.0, (trace - root) / 2)};
}

static double bandWeight(double frequency, double low, double high) {
  if (frequency <= 0 || frequency < low / 2 || frequency > high * 2) return 0;
  if (frequency < low) return 0.5 - 0.5 * std::cos(pi * std::log2(frequency / (low / 2)));
  if (frequency > high) return 0.5 + 0.5 * std::cos(pi * std::log2(frequency / high));
  return 1;
}

CrosstalkFir designCrosstalkFir(const CrosstalkMeasurements &measurements,
                               yyjson_val *parameters, double sampleRate) {
  auto result = CrosstalkFir{};
  if (!measurements.error.empty()) { result.error = measurements.error; return result; }
  constexpr auto allowed = std::array{1024u, 2048u, 4096u, 8192u, 16384u};
  const auto requested = parameter(parameters, "tp", 0, 65536, 4096);
  const auto taps = std::ranges::find(allowed, requested) == allowed.end() ? 4096u : static_cast<unsigned>(requested);
  sampleRate = std::round(std::clamp(sampleRate, 8000.0, 768000.0));
  const auto directMs = parameter(parameters, "wl", 2, 50, 8);
  const auto low = std::max(parameter(parameters, "fl", 20, 2000, 200), 1000 / directMs);
  const auto sourceRate = measurements.sources[0].sampleRate;
  const auto high = std::min({std::max(parameter(parameters, "fl", 20, 2000, 200), parameter(parameters, "fh", 1000, 20000, 6000)), sourceRate * 0.475, sampleRate * 0.475});
  if (sourceRate > 768000 || sourceRate < 8000) { result.error = "measurement sample rate is outside design limits"; return result; }
  const auto radius = sourceRate == sampleRate ? 0u : static_cast<unsigned>(std::ceil(92 / (4.57 * pi * std::min(1.0, sampleRate / sourceRate) * 0.1)));
  const auto guard = radius * 2u;
  auto signals = std::array<std::vector<float>, 4>{};
  auto delays = std::array<double, 4>{};
  auto fftSize = std::size_t{taps * 4u};
  // Bound temporary design storage before converting untrusted timing metadata
  // to allocation sizes. Normal direct windows are only a few thousand samples.
  constexpr auto maximumSamples = std::size_t{16u * 1024u * 1024u};
  for (const auto pair : {std::array<std::size_t, 2>{0, 2}, std::array<std::size_t, 2>{1, 3}}) {
    const auto &a = measurements.sources[pair[0]], &b = measurements.sources[pair[1]];
    const auto onsetA = a.trimStartSamples + a.onsetIndex, onsetB = b.trimStartSamples + b.onsetIndex;
    const auto common = std::max<std::int64_t>(0, std::min(onsetA, onsetB) - std::llround(sourceRate * 0.001));
    const auto direct = std::max<std::int64_t>(1, std::llround(sourceRate * directMs / 1000));
    const auto length = std::max(onsetA, onsetB) - common + direct + 1;
    if (std::min(onsetA, onsetB) < common || length <= 0 || static_cast<std::uint64_t>(length) + guard * 2u > maximumSamples ||
        (length + guard * 2.0) * sampleRate / sourceRate > maximumSamples) {
      result.error = "measurement timing span exceeds design capacity"; return result;
    }
    for (const auto slot : pair) {
      const auto &record = measurements.sources[slot];
      const auto onset = record.trimStartSamples + record.onsetIndex - common;
      const auto start = record.trimStartSamples - common, end = onset + direct;
      const auto fade = std::max<std::int64_t>(1, direct / 2), fadeStart = end - fade;
      auto aligned = std::vector<float>(static_cast<std::size_t>(length) + guard * 2u);
      for (std::size_t i = 0; i < record.samples.size(); ++i) {
        const auto index = start + static_cast<std::int64_t>(i);
        if (index < 0 || index >= length || index >= end) continue;
        const auto gain = index >= fadeStart ? 0.5 + 0.5 * std::cos(pi * (index - fadeStart) / fade) : 1;
        aligned[index + guard] = static_cast<float>(record.samples[i] * gain / record.referenceScale);
      }
      signals[slot] = resample(aligned, static_cast<unsigned>(sourceRate), static_cast<unsigned>(sampleRate), radius);
      delays[slot] = (onset + double(guard)) / sourceRate;
      while (fftSize < signals[slot].size()) fftSize *= 2;
    }
  }
  auto spectra = std::array<std::vector<Complex>, 4>{};
  for (std::size_t slot = 0; slot < 4; ++slot) {
    spectra[slot].resize(fftSize);
    std::copy(signals[slot].begin(), signals[slot].end(), spectra[slot].begin());
    transform(spectra[slot], false);
  }
  const auto first = std::max<std::size_t>(1, std::ceil(low * fftSize / sampleRate));
  const auto last = std::min<std::size_t>(fftSize / 2, std::floor(high * fftSize / sampleRate));
  double magnitude = 0;
  for (auto bin = first; bin <= last; ++bin) magnitude += std::abs(spectra[0][bin]) + std::abs(spectra[3][bin]);
  if (first > last || !(magnitude > epsilon)) { result.error = "selected frequency range cannot be designed"; return result; }
  const auto scale = 2.0 * (last - first + 1) / magnitude;
  for (std::size_t slot = 0; slot < 4; ++slot) {
    for (auto &value : spectra[slot]) value *= scale;
    spectra[slot] = smooth(spectra[slot], fftSize, sampleRate, delays[slot]);
  }
  auto output = std::array<std::vector<Complex>, 4>{};
  for (auto &channel : output) channel.resize(fftSize);
  const auto betaMid = std::pow(10, (-60 + 0.4 * parameter(parameters, "rg", 0, 100, 50)) / 10);
  const auto gainLimit = std::pow(10, parameter(parameters, "mg", 0, 24, 12) / 20);
  for (std::size_t bin = 0; bin <= fftSize / 2; ++bin) {
    const auto frequency = bin * sampleRate / fftSize;
    const auto ll = spectra[0][bin], lr = spectra[1][bin], rl = spectra[2][bin], rr = spectra[3][bin];
    const auto delay = std::polar(1.0, -2 * pi * bin * (taps / 2) / fftSize);
    const auto lowRatio = frequency > 0 ? low / frequency : 100.0;
    const auto highRatio = frequency / high;
    auto beta = betaMid * std::min(100.0, std::max({1.0, lowRatio * lowRatio, highRatio * highRatio}));
    const auto targetMagnitude = std::max(std::abs(ll), std::abs(rr));
    for (const auto squared : singularSquared(ll, lr, rl, rr)) beta = std::max(beta, std::sqrt(squared) * targetMagnitude / gainLimit - squared);
    const auto a11 = std::norm(ll) + std::norm(lr) + beta, a22 = std::norm(rl) + std::norm(rr) + beta;
    const auto a12 = std::conj(ll) * rl + std::conj(lr) * rr;
    const auto determinant = a11 * a22 - std::norm(a12);
    const auto b11 = std::conj(ll) * ll * delay, b21 = std::conj(rl) * ll * delay;
    const auto b12 = std::conj(lr) * rr * delay, b22 = std::conj(rr) * rr * delay;
    const auto weight = bandWeight(frequency, low, high);
    const auto inverse = 1.0 / determinant;
    const auto values = std::array{
      (a22 * b11 - a12 * b21) * inverse * weight + delay * (1 - weight),
      (a11 * b21 - std::conj(a12) * b11) * inverse * weight,
      (a22 * b12 - a12 * b22) * inverse * weight,
      (a11 * b22 - std::conj(a12) * b12) * inverse * weight + delay * (1 - weight)};
    for (std::size_t c = 0; c < 4; ++c) {
      output[c][bin] = values[c];
      if (bin > 0 && bin < fftSize / 2) output[c][fftSize - bin] = std::conj(values[c]);
    }
  }
  const auto edge = std::max<long long>(2, std::llround(taps * 0.01));
  for (auto &spectrum : output) {
    spectrum.front().imag(0); spectrum[fftSize / 2].imag(0);
    transform(spectrum, true);
    auto channel = std::vector<float>(taps);
    for (unsigned i = 0; i < taps; ++i) {
      const auto distance = std::min<long long>(i, taps - 1 - i);
      const auto window = distance < edge ? 0.5 - 0.5 * std::cos(pi * distance / edge) : 1;
      channel[i] = static_cast<float>(spectrum[i].real() * window);
      if (!std::isfinite(channel[i])) { result.error = "non-finite cancellation filter"; return result; }
    }
    result.channels.push_back(std::move(channel));
  }
  return result;
}
} // namespace pipetune
