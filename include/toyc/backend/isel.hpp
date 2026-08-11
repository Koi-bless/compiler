#pragma once
#include "toyc/backend/mir.hpp"
#include "toyc/frontend/semantic.hpp"
#include "toyc/ir/ir.hpp"
namespace toyc {
class InstructionSelector {
public:
    explicit InstructionSelector(const SemanticResult& semantic) : semantic_(semantic) {}
    MachineModule lower(const IRModule& module) const;
private:
    const SemanticResult& semantic_;
};
}
