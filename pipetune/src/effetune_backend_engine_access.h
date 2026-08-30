/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_EFFETUNE_BACKEND_ENGINE_ACCESS_H
#define PIPETUNE_EFFETUNE_BACKEND_ENGINE_ACCESS_H

#include <effetune/abi.h>

namespace effetune {
class Engine;
}

// This internal accessor is defined inside the patched abi.cpp translation
// unit because EffeTune intentionally keeps its engine table file-local.
effetune::Engine *
pipetuneEffetuneBackendResolveEngine(et_engine engine) noexcept;

#endif
