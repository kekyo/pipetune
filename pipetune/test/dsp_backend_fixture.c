#include <effetune/abi.h>

ET_EXPORT uint32_t et_abi_version(void) {
#if defined(PIPETUNE_FIXTURE_WRONG_ABI)
  return EFFETUNE_DSP_ABI_VERSION + 1u;
#else
  return EFFETUNE_DSP_ABI_VERSION;
#endif
}

#if !defined(PIPETUNE_FIXTURE_WRONG_ABI)
ET_EXPORT uint32_t et_build_flags(void) {
  return 0u;
}

ET_EXPORT uint32_t et_backend_variant(void) {
  return ET_BACKEND_VARIANT_SCALAR;
}
#endif
