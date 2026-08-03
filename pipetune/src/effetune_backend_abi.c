#include "effetune_backend_abi.h"

#if !defined(PIPETUNE_EFFETUNE_BACKEND_VARIANT)
#error "PIPETUNE_EFFETUNE_BACKEND_VARIANT must identify this backend"
#endif

uint32_t pipetune_effetune_backend_variant(void) {
  return PIPETUNE_EFFETUNE_BACKEND_VARIANT;
}
