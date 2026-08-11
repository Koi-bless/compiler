#include "test_support.hpp"

int main() {
    std::string source = "int keep(){return 1;}int main(){";
    for (int index = 0; index < 12; ++index)
        source += "int v" + std::to_string(index) + "=" + std::to_string(index + 1) + ";";
    source += "int z=keep();return z";
    for (int index = 0; index < 12; ++index) source += "+v" + std::to_string(index);
    source += ";}";
    TestPipeline pipeline(source);
    auto function = pipeline.machine.functions[1];
    const auto allocation = toyc::LinearScanRegisterAllocator().run(function);
    check(function.stage == toyc::MIRStage::PostRegisterAllocation, "allocator did not advance MIR stage");
    check(function.spillSlotCount > 0, "cross-call pressure did not trigger spill");
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
        for (const auto& operand : instruction.defs) check(!std::holds_alternative<toyc::VirtualReg>(operand), "post-RA def is virtual");
        for (const auto& operand : instruction.uses) check(!std::holds_alternative<toyc::VirtualReg>(operand), "post-RA use is virtual");
    }
    check(!allocation.usedCalleeSaved.empty(), "cross-call values did not use callee-saved registers");
}
