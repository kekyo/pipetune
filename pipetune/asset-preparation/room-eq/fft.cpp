/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
/* Port of EffeTune v2.13.0 js/utils/measurement-dsp/fft.js.
 * Copyright (c) 2025-2026 Yoshiyuki Kobayashi. MIT; see LICENSE.effetune.
 */
#include "fft.h"

#include <bit>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace pipetune::assets::room_eq {

struct FftPlan {
  std::vector<float> cosines;
  std::vector<float> sines;
  std::vector<std::size_t> reverse;
  std::vector<unsigned> radices;
};

static FftPlan makePlan(std::size_t size) {
  if (size < 2 || !std::has_single_bit(size)) throw std::invalid_argument("invalid Room EQ FFT length");
  auto plan = FftPlan{std::vector<float>(size), std::vector<float>(size),
                      std::vector<std::size_t>(size), {}};
  auto bits = std::countr_zero(size);
  if (bits % 2) { plan.radices.push_back(2); --bits; }
  while (bits > 0) { plan.radices.push_back(4); bits -= 2; }
  auto digits = std::vector<unsigned>(plan.radices.size());
  for (auto index = std::size_t{0}; index < size; ++index) {
    auto value = index;
    for (auto digit = std::size_t{0}; digit < digits.size(); ++digit) {
      digits[digit] = value % plan.radices[digit];
      value /= plan.radices[digit];
    }
    auto reversed = std::size_t{0}, multiplier = std::size_t{1};
    for (auto digit = digits.size(); digit-- > 0;) {
      reversed += digits[digit] * multiplier;
      multiplier *= plan.radices[digit];
    }
    plan.reverse[index] = reversed;
    const auto angle = -2 * std::numbers::pi * index / size;
    plan.cosines[index] = std::cos(angle);
    plan.sines[index] = std::sin(angle);
  }
  return plan;
}

static Spectrum transform(const FftPlan &plan, const Spectrum &input) {
  const auto size = plan.reverse.size();
  auto output = Spectrum{std::vector<double>(size), std::vector<double>(size)};
  auto &real = output.real;
  auto &imag = output.imag;
  for (auto index = std::size_t{0}; index < size; ++index) {
    real[index] = input.real[plan.reverse[index]];
    imag[index] = input.imag[plan.reverse[index]];
  }
  auto completed = std::size_t{1};
  for (const auto radix : plan.radices) {
    const auto length = completed * radix, step = size / length;
    for (auto start = std::size_t{0}; start < size; start += length) {
      for (auto offset = std::size_t{0}; offset < completed; ++offset) {
        const auto first = start + offset, second = first + completed;
        const auto twiddle = offset * step;
        const auto ar = real[first], ai = imag[first];
        const auto br = real[second] * plan.cosines[twiddle] - imag[second] * plan.sines[twiddle];
        const auto bi = real[second] * plan.sines[twiddle] + imag[second] * plan.cosines[twiddle];
        if (radix == 2) {
          real[first] = ar + br; imag[first] = ai + bi;
          real[second] = ar - br; imag[second] = ai - bi;
          continue;
        }
        const auto third = second + completed, fourth = third + completed;
        const auto cr = real[third] * plan.cosines[twiddle * 2] - imag[third] * plan.sines[twiddle * 2];
        const auto ci = real[third] * plan.sines[twiddle * 2] + imag[third] * plan.cosines[twiddle * 2];
        const auto dr = real[fourth] * plan.cosines[twiddle * 3] - imag[fourth] * plan.sines[twiddle * 3];
        const auto di = real[fourth] * plan.sines[twiddle * 3] + imag[fourth] * plan.cosines[twiddle * 3];
        real[first] = ar + br + cr + dr; imag[first] = ai + bi + ci + di;
        real[second] = ar + bi - cr - di; imag[second] = ai - br - ci + dr;
        real[third] = ar - br + cr - dr; imag[third] = ai - bi + ci - di;
        real[fourth] = ar - bi - cr + di; imag[fourth] = ai + br - ci - dr;
      }
    }
    completed = length;
  }
  return output;
}

Spectrum realTransform(std::span<const double> input) {
  if (input.size() < 4) throw std::invalid_argument("Room EQ real FFT requires four samples");
  const auto plan = makePlan(input.size());
  const auto half = input.size() / 2;
  auto packed = Spectrum{std::vector<double>(half), std::vector<double>(half)};
  for (auto index = std::size_t{0}; index < half; ++index) {
    packed.real[index] = input[index * 2]; packed.imag[index] = input[index * 2 + 1];
  }
  const auto transformed = transform(makePlan(half), packed);
  auto output = Spectrum{std::vector<double>(half + 1), std::vector<double>(half + 1)};
  for (auto index = std::size_t{0}; index <= half; ++index) {
    const auto wrapped = index % half, mirrored = (half - wrapped) % half;
    const auto ar = transformed.real[wrapped], ai = transformed.imag[wrapped];
    const auto br = transformed.real[mirrored], bi = -transformed.imag[mirrored];
    const auto dr = ar - br, di = ai - bi;
    const auto rr = dr * plan.cosines[index] - di * plan.sines[index];
    const auto ri = dr * plan.sines[index] + di * plan.cosines[index];
    output.real[index] = 0.5 * (ar + br + ri);
    output.imag[index] = 0.5 * (ai + bi - rr);
  }
  return output;
}

std::vector<double> inverseRealTransform(const Spectrum &spectrum) {
  if (spectrum.real.size() < 3 || spectrum.real.size() != spectrum.imag.size())
    throw std::invalid_argument("invalid Room EQ inverse spectrum");
  const auto half = spectrum.real.size() - 1, size = half * 2;
  const auto plan = makePlan(size);
  auto conjugated = Spectrum{std::vector<double>(size), std::vector<double>(size)};
  for (auto index = std::size_t{0}; index <= half; ++index) {
    conjugated.real[index] = spectrum.real[index]; conjugated.imag[index] = -spectrum.imag[index];
  }
  for (auto index = std::size_t{1}; index < half; ++index) {
    conjugated.real[size - index] = spectrum.real[index]; conjugated.imag[size - index] = spectrum.imag[index];
  }
  auto output = transform(plan, conjugated).real;
  for (auto &value : output) value /= size;
  return output;
}

} // namespace pipetune::assets::room_eq
