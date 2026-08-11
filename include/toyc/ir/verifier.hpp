#pragma once

#include "toyc/frontend/semantic.hpp"
#include "toyc/ir/cfg.hpp"
#include "toyc/ir/ir.hpp"

namespace toyc {

void verifyCFG(const CFGModule& module, const SemanticResult& semantic);
void verifyIR(const IRModule& module, const SemanticResult& semantic);

} // namespace toyc
