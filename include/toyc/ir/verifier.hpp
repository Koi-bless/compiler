#pragma once

#include "toyc/frontend/semantic.hpp"
#include "toyc/ir/cfg.hpp"

namespace toyc {

void verify(const ModuleIR& module, const SemanticResult& semantic);

} // namespace toyc
