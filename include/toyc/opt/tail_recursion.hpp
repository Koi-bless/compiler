#pragma once

#include "toyc/opt/pass.hpp"

namespace toyc {

// Converts direct calls in true tail position into SSA loop backedges.
PassResult runTailRecursionElimination(IRFunction& function);

} // namespace toyc
