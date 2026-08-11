#pragma once

#include <iosfwd>

#include "toyc/backend/mir.hpp"
#include "toyc/frontend/semantic.hpp"

namespace toyc {

class AsmPrinter {
public:
    explicit AsmPrinter(std::ostream& output) : output_(output) {}
    void print(const MachineModule& module, const SemanticResult& semantic);

private:
    std::ostream& output_;
};

} // namespace toyc
