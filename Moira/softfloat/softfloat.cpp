// Berkeley SoftFloat 3e — amalgamation build for Moira
// License: BSD (see COPYING.txt in this directory)
//
// This file compiles all required SoftFloat sources as a single translation unit.
// We wrap in extern "C" since SoftFloat is pure C.

#include "platform.h"

extern "C" {

// Global state definitions
#include <stdint.h>
uint_fast8_t softfloat_detectTininess = 1; // afterRounding (68k behavior)
uint_fast8_t softfloat_roundingMode = 0;   // near_even
uint_fast8_t softfloat_exceptionFlags = 0;
uint_fast8_t extF80_roundingPrecision = 80;

// Internal helpers
#include "s_countLeadingZeros8.c"
#include "s_countLeadingZeros16.c"
#include "s_countLeadingZeros32.c"
#include "s_countLeadingZeros64.c"
#include "s_shortShiftRightJam64.c"
#include "s_shortShiftRightJam64Extra.c"
#include "s_shiftRightJam32.c"
#include "s_shiftRightJam64.c"
#include "s_shiftRightJam64Extra.c"
#include "s_shiftRightJam128.c"
#include "s_shiftRightJam128Extra.c"
#include "s_shortShiftLeft128.c"
#include "s_shortShiftRight128.c"
#include "s_shortShiftRightJam128.c"
#include "s_shortShiftRightJam128Extra.c"
#include "s_add128.c"
#include "s_sub128.c"
#include "s_mul64To128.c"
#include "s_mul128By32.c"
#include "s_add256M.c"
#include "s_sub256M.c"
#include "s_compare96M.c"
#include "s_compare128M.c"
#include "s_le128.c"
#include "s_lt128.c"
#include "s_mul64ByShifted32To128.c"
#include "s_approxRecip_1Ks.c"
#include "s_approxRecip32_1.c"
#include "s_approxRecipSqrt_1Ks.c"
#include "s_approxRecipSqrt32_1.c"
#include "s_normSubnormalF32Sig.c"
#include "s_normSubnormalF64Sig.c"
#include "s_roundToI32.c"
#include "s_roundToI64.c"
#include "s_roundToUI32.c"
#include "s_roundToUI64.c"

// Rounding and packing
#include "s_roundPackToF32.c"
#include "s_roundPackToF64.c"
#include "s_roundPackToExtF80.c"
#include "s_normRoundPackToExtF80.c"
#include "s_normSubnormalExtF80Sig.c"

// NaN handling (8086-SSE specialization)
#include "softfloat_raiseFlags.c"
#include "s_extF80UIToCommonNaN.c"
#include "s_commonNaNToExtF80UI.c"
#include "s_propagateNaNExtF80UI.c"
#include "s_f32UIToCommonNaN.c"
#include "s_commonNaNToF32UI.c"
#include "s_propagateNaNF32UI.c"
#include "s_f64UIToCommonNaN.c"
#include "s_commonNaNToF64UI.c"
#include "s_propagateNaNF64UI.c"

// ExtF80 core operations
#include "s_addMagsExtF80.c"
#include "s_subMagsExtF80.c"
#include "extF80_add.c"
#include "extF80_sub.c"
#include "extF80_mul.c"
#include "extF80_div.c"
#include "extF80_rem.c"
#include "extF80_sqrt.c"
#include "extF80_roundToInt.c"

// ExtF80 comparisons
#include "extF80_eq.c"
#include "extF80_le.c"
#include "extF80_lt.c"
#include "extF80_eq_signaling.c"
#include "extF80_le_quiet.c"
#include "extF80_lt_quiet.c"
#include "extF80_isSignalingNaN.c"

// ExtF80 conversions
#include "extF80_to_f32.c"
#include "extF80_to_f64.c"
#include "extF80_to_i32.c"
#include "extF80_to_i64.c"
#include "extF80_to_i32_r_minMag.c"
#include "extF80_to_i64_r_minMag.c"
#include "extF80_to_ui32.c"
#include "extF80_to_ui64.c"
#include "extF80_to_ui32_r_minMag.c"
#include "extF80_to_ui64_r_minMag.c"

// Conversions to ExtF80
#include "f32_to_extF80.c"
#include "f64_to_extF80.c"
#include "i32_to_extF80.c"
#include "i64_to_extF80.c"
#include "ui32_to_extF80.c"
#include "ui64_to_extF80.c"

} // extern "C"
