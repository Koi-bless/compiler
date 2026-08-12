#pragma once

#include <iosfwd>

#include "toyc/backend/mir.hpp"
#include "toyc/frontend/semantic.hpp"

namespace toyc {

struct AsmPrinterOptions {
    bool enableFallthrough = false;
};

class AsmPrinter {
public:
    explicit AsmPrinter(std::ostream& output, AsmPrinterOptions options = {})
        : output_(output), options_(options) {}
    void print(const MachineModule& module, const SemanticResult& semantic);

private:
    std::ostream& output_;
    AsmPrinterOptions options_;
};

} // namespace toyc
