#pragma once
#include "toyc/backend/mir.hpp"
namespace toyc {
void verifyMIR(const MachineFunction& function, MIRStage stage);
void verifyMIR(const MachineModule& module, MIRStage stage);
}
