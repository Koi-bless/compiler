#pragma once
#include "toyc/opt/pass.hpp"
namespace toyc {
struct FunctionEffectAnalysis;
PassResult runDCE(IRFunction& function, bool preserveMayTrap = true,
                  const FunctionEffectAnalysis* effects = nullptr);
}
