#pragma once

#include "toyc/opt/pass.hpp"

namespace toyc {

// Replaces live-out values of finite canonical loops with their exact final
// values, then removes loops whose remaining work is unobservable.
PassResult runLoopFinalValueAndDeletion(IRFunction& function,
                                        const IRModule& module);

} // namespace toyc
