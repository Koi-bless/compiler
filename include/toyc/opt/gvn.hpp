#pragma once
#include "toyc/opt/pass.hpp"
namespace toyc {
struct FunctionEffectAnalysis;
PassResult runGVN(IRFunction& function,
                  const FunctionEffectAnalysis* effects = nullptr);
}
