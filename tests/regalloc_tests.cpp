#include <algorithm>

#include "test_support.hpp"

int main() {
    std::string source = "int keep(){return 1;}int main(){";
    for (int index = 0; index < 16; ++index)
        source += "int v" + std::to_string(index) + "=" + std::to_string(index + 1) + ";";
    source += "int z=keep();return z";
    for (int index = 0; index < 16; ++index) source += "+v" + std::to_string(index);
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

    TestPipeline optimizedPipeline(source, true);
    auto withoutRematerialization = optimizedPipeline.machine.functions[1];
    toyc::LinearScanRegisterAllocator().run(withoutRematerialization);
    auto rematerialized = optimizedPipeline.machine.functions[1];
    const auto optimizedAllocation = toyc::LinearScanRegisterAllocator(
        toyc::RegAllocOptions{false, true}).run(rematerialized);
    check(rematerialized.spillSlotCount < withoutRematerialization.spillSlotCount,
          "spilled constants did not reduce spill-slot usage through rematerialization");
    check(std::any_of(optimizedAllocation.rematerializations.begin(),
                      optimizedAllocation.rematerializations.end(),
                      [](const auto& value) { return value.has_value(); }),
          "allocator did not record LI rematerialization recipes");

    toyc::MachineFunction copyFunction;
    copyFunction.vregCount = 2;
    copyFunction.blocks.resize(1);
    copyFunction.blocks[0].id = 0;
    copyFunction.blocks[0].instructions = {
        {toyc::MOpcode::LI, {toyc::vreg(0)}, {toyc::Immediate{7}}, {}, {}, {}},
        {toyc::MOpcode::COPY, {toyc::vreg(1)}, {toyc::vreg(0)}, {}, {}, {}},
        {toyc::MOpcode::COPY, {toyc::PhysReg::A0}, {toyc::vreg(1)}, {}, {}, {}},
        {toyc::MOpcode::RET, {}, {}, {}, {}, {}}
    };
    toyc::LinearScanRegisterAllocator(toyc::RegAllocOptions{true, false})
        .run(copyFunction);
    const auto copyCount = std::count_if(
        copyFunction.blocks[0].instructions.begin(),
        copyFunction.blocks[0].instructions.end(),
        [](const toyc::MInstruction& instruction) {
            return instruction.opcode == toyc::MOpcode::COPY;
        });
    check(copyCount == 1, "copy hint did not eliminate a safe virtual copy");

    std::string spillPressure = "int bump(int x){return x*3+1;}int main(){";
    for (int index = 0; index < 16; ++index)
        spillPressure += "int v" + std::to_string(index) + "=" +
                         std::to_string(index + 1) + ";";
    spillPressure += "int i=0;while(i<7){i=i+1;";
    for (int index = 0; index < 16; ++index)
        spillPressure += "v" + std::to_string(index) + "=v" +
                         std::to_string(index) + "+i+" +
                         std::to_string(index % 3) + ";";
    spillPressure += "v0=bump(v0);}return v0";
    for (int index = 1; index < 16; ++index)
        spillPressure += "+v" + std::to_string(index);
    spillPressure += ";}";
    TestPipeline pressurePipeline(spillPressure, true);
    auto pressureFunction = pressurePipeline.machine.functions[1];
    toyc::LinearScanRegisterAllocator(toyc::RegAllocOptions{true, true})
        .run(pressureFunction);
    for (const auto& block : pressureFunction.blocks)
        for (const auto& instruction : block.instructions) {
            for (const auto& operand : instruction.defs)
                check(!std::holds_alternative<toyc::VirtualReg>(operand),
                      "three-way spill left a virtual definition");
            for (const auto& operand : instruction.uses)
                check(!std::holds_alternative<toyc::VirtualReg>(operand),
                      "three-way spill left a virtual use");
        }
}
