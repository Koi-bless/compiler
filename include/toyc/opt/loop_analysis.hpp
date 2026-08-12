#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "toyc/ir/ir.hpp"

namespace toyc {

struct Loop {
    BlockId header{};
    std::vector<BlockId> blocks;
    std::vector<BlockId> latches;
    std::optional<BlockId> preheader;
    std::optional<std::size_t> parent;
    unsigned depth = 1;
};

std::vector<Loop> analyzeLoops(const IRFunction& function);

} // namespace toyc
