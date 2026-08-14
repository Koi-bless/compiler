#pragma once

#include "toyc/opt/function_effects.hpp"
#include "toyc/opt/pass.hpp"

namespace toyc {

// Keep hot global scalars in SSA form for the duration of a function when no
// call can observe their intermediate values.
PassResult runGlobalScalarLocalization(
    IRFunction& function, const FunctionEffectAnalysis& effects);

} // namespace toyc
