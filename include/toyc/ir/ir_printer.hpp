#pragma once

#include <iosfwd>

#include "toyc/ir/ir.hpp"

namespace toyc {
struct SemanticResult;
void printIR(std::ostream& output, const IRModule& module,
             const SemanticResult& semantic);
} // namespace toyc
