#pragma once

#include <cstddef>
#include <iosfwd>

#include "toyc/frontend/semantic.hpp"
#include "toyc/ir/ir.hpp"

namespace toyc {

struct PassResult {
    bool changed = false;
    std::size_t instructionsRemoved = 0;
    std::size_t instructionsReplaced = 0;
    std::size_t blocksRemoved = 0;

    PassResult& operator+=(const PassResult& other) {
        changed = changed || other.changed;
        instructionsRemoved += other.instructionsRemoved;
        instructionsReplaced += other.instructionsReplaced;
        blocksRemoved += other.blocksRemoved;
        return *this;
    }
};

struct OptimizationOptions {
    bool enabled = false;
    bool verifyEach = false;
    bool dumpBefore = false;
    bool dumpAfterEach = false;
    bool printStats = false;
    unsigned maxFixpointIterations = 4;
};

void runOptimizationPipeline(IRModule& module, const SemanticResult& semantic,
                             const OptimizationOptions& options,
                             std::ostream& diagnostics);

} // namespace toyc
