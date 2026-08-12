#pragma once
#include "toyc/backend/mir.hpp"
#include "toyc/frontend/semantic.hpp"
#include "toyc/ir/ir.hpp"
namespace toyc {
struct ISelOptions { bool fuseCompareBranches = false; };
class InstructionSelector {
public:
    explicit InstructionSelector(const SemanticResult& semantic,
                                 ISelOptions options = {})
        : semantic_(semantic), options_(options) {}
    MachineModule lower(const IRModule& module) const;
private:
    const SemanticResult& semantic_;
    ISelOptions options_;
};
}
