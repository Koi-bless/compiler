#pragma once
#include <iosfwd>
#include "toyc/backend/mir.hpp"
namespace toyc {
struct SemanticResult;
void printMIR(std::ostream& output, const MachineModule& module,
              const SemanticResult& semantic);
}
