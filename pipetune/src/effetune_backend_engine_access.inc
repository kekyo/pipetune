/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "effetune_backend_engine_access.h"

effetune::Engine *
pipetuneEffetuneBackendResolveEngine(et_engine engine) noexcept {
  return findEngine(engine);
}
