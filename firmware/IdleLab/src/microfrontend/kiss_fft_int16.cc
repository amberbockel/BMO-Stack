#include <cstdint>

#include "kiss_fft_common.h"

#define FIXED_POINT 16
namespace kissfft_fixed16 {
#include "kiss_fft_impl.h"
#include "tools/kiss_fftr_impl.h"
}  // namespace kissfft_fixed16
#undef FIXED_POINT
