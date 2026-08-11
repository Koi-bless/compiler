#pragma once

#include <iosfwd>

#include "toyc/frontend/semantic.hpp"
#include "toyc/ir/cfg.hpp"

namespace toyc {

class AsmPrinter {
public:
    explicit AsmPrinter(std::ostream& output) : output_(output) {}
    void print(const ModuleIR& module, const SemanticResult& semantic);

private:
    std::ostream& output_;
};

} // namespace toyc
