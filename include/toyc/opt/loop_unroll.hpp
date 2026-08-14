#pragma once

#include <cstddef>
#include <cstdint>

#include "toyc/opt/pass.hpp"

namespace toyc {

PassResult runSmallLoopUnroll(IRFunction& function,
                              std::uint64_t maxTrips = 4,
                              std::size_t growthBudget = 4096);

} // namespace toyc
