#pragma once
#include "toyc/backend/mir.hpp"
#include "toyc/ir/ir.hpp"
namespace toyc {
void splitCriticalEdges(IRModule& module);
void resolveParallelCopies(MachineFunction& function);
void resolveParallelCopies(MachineModule& module);
}
