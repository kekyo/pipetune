/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_CROSSTALK_FIR_H
#define PIPETUNE_CROSSTALK_FIR_H
#include "measurement_store.h"
namespace pipetune {
struct CrosstalkFir {
  // Input-major true stereo: C11, C21, C12, C22.
  std::vector<std::vector<float>> channels;
  std::string error;
};
CrosstalkFir designCrosstalkFir(const CrosstalkMeasurements &measurements,
                               yyjson_val *parameters, double sampleRate);
}
#endif
