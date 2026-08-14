#pragma once
#include "toyc/opt/pass.hpp"
namespace toyc {
struct FunctionEffectAnalysis;
PassResult runGlobalPromotion(IRFunction& function,
                              const FunctionEffectAnalysis& effects);
}
