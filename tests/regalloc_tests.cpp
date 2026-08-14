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

    TestPipeline rematerializationPipeline(source);
    auto withoutRematerialization = rematerializationPipeline.machine.functions[1];
    toyc::LinearScanRegisterAllocator().run(withoutRematerialization);
    auto rematerialized = rematerializationPipeline.machine.functions[1];
    const auto optimizedAllocation = toyc::LinearScanRegisterAllocator(
        toyc::RegAllocOptions{false, true}).run(rematerialized);
    check(rematerialized.spillSlotCount <= withoutRematerialization.spillSlotCount,
          "spilled-constant rematerialization increased spill-slot usage");
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

    // Loop-carried phi copies on a back edge: v0/v1 are the phi results,
    // v2/v3 die into the back-edge copies, so each source may share its
    // destination's register and both copies become no-ops.
    const auto buildLoop = [](bool readDestinationInBody) {
        toyc::MachineFunction function;
        function.vregCount = readDestinationInBody ? 6 : 5;
        function.blocks.resize(4);
        function.blocks[0].id = 0;
        function.blocks[0].successors = {1};
        function.blocks[0].instructions = {
            {toyc::MOpcode::LI, {toyc::vreg(0)}, {toyc::Immediate{0}}, {}, {}, {}},
            {toyc::MOpcode::LI, {toyc::vreg(1)}, {toyc::Immediate{0}}, {}, {}, {}},
            {toyc::MOpcode::LI, {toyc::vreg(4)}, {toyc::Immediate{100}}, {}, {}, {}},
            {toyc::MOpcode::JUMP, {}, {toyc::MachineBlockRef{1}}, {}, {}, {}}
        };
        function.blocks[1].id = 1;
        function.blocks[1].predecessors = {0, 2};
        function.blocks[1].successors = {3, 2};
        function.blocks[1].instructions = {
            {toyc::MOpcode::BGE, {},
             {toyc::vreg(1), toyc::vreg(4), toyc::MachineBlockRef{3}, toyc::MachineBlockRef{2}},
             {}, {}, {}}
        };
        function.blocks[2].id = 2;
        function.blocks[2].predecessors = {1};
        function.blocks[2].successors = {1};
        function.blocks[2].instructions = {
            {toyc::MOpcode::ADD, {toyc::vreg(2)}, {toyc::vreg(0), toyc::vreg(1)}, {}, {}, {}},
            {toyc::MOpcode::ADDI, {toyc::vreg(3)}, {toyc::vreg(1), toyc::Immediate{1}}, {}, {}, {}}
        };
        if (readDestinationInBody)
            function.blocks[2].instructions.push_back(
                {toyc::MOpcode::ADD, {toyc::vreg(5)}, {toyc::vreg(0), toyc::vreg(4)}, {}, {}, {}});
        function.blocks[2].instructions.push_back(
            {toyc::MOpcode::COPY, {toyc::vreg(0)}, {toyc::vreg(2)}, {}, {}, {}});
        function.blocks[2].instructions.push_back(
            {toyc::MOpcode::COPY, {toyc::vreg(1)}, {toyc::vreg(3)}, {}, {}, {}});
        function.blocks[2].instructions.push_back(
            {toyc::MOpcode::JUMP, {}, {toyc::MachineBlockRef{1}}, {}, {}, {}});
        function.blocks[3].id = 3;
        function.blocks[3].predecessors = {1};
        function.blocks[3].instructions = {
            {toyc::MOpcode::COPY, {toyc::PhysReg::A0}, {toyc::vreg(0)}, {}, {}, {}},
            {toyc::MOpcode::RET, {}, {}, {}, {}, {}}
        };
        return function;
    };

    auto loopFunction = buildLoop(false);
    const auto loopAllocation = toyc::LinearScanRegisterAllocator(
        toyc::RegAllocOptions{true, false}).run(loopFunction);
    check(loopAllocation.registers[2] == loopAllocation.registers[0],
          "back-edge copy source did not share the destination's register");
    check(loopAllocation.registers[3] == loopAllocation.registers[1],
          "second back-edge copy source did not share the destination's register");
    for (const auto& instruction : loopFunction.blocks[2].instructions)
        check(instruction.opcode != toyc::MOpcode::COPY,
              "coalesced back-edge copy survived register allocation");

    // Same shape, but the destination is read by a third instruction while
    // the source is live: sharing would corrupt that read, so no merge.
    auto conflictFunction = buildLoop(true);
    const auto conflictAllocation = toyc::LinearScanRegisterAllocator(
        toyc::RegAllocOptions{true, false}).run(conflictFunction);
    check(conflictAllocation.registers[2] != conflictAllocation.registers[0],
          "copy hint merged despite a conflicting read of the destination");
    check(conflictAllocation.registers[3] == conflictAllocation.registers[1],
          "unconflicted back-edge copy was not coalesced");

    toyc::MachineFunction coloredSpills;
    coloredSpills.vregCount = 34;
    coloredSpills.hasCalls = true;
    coloredSpills.blocks.resize(1);
    coloredSpills.blocks[0].id = 0;
    for (int group = 0; group < 2; ++group) {
        const int first = group * 17;
        for (int index = 0; index < 17; ++index)
            coloredSpills.blocks[0].instructions.push_back(
                {toyc::MOpcode::LI, {toyc::vreg(first + index)},
                 {toyc::Immediate{index + 1}}, {}, {}, {}});
        for (int index = 0; index < 17; ++index)
            coloredSpills.blocks[0].instructions.push_back(
                {toyc::MOpcode::COPY, {toyc::PhysReg::A0},
                 {toyc::vreg(first + index)}, {}, {}, {}});
    }
    coloredSpills.blocks[0].instructions.push_back(
        {toyc::MOpcode::RET, {}, {}, {}, {}, {}});
    toyc::LinearScanRegisterAllocator().run(coloredSpills);
    check(coloredSpills.spillSlotCount == 1,
          "non-overlapping spill intervals did not reuse a stack slot");

    toyc::MachineFunction leafRegisters;
    leafRegisters.vregCount = 20;
    leafRegisters.blocks.resize(1);
    leafRegisters.blocks[0].id = 0;
    for (int index = 0; index < 20; ++index)
        leafRegisters.blocks[0].instructions.push_back(
            {toyc::MOpcode::LI, {toyc::vreg(index)},
             {toyc::Immediate{index + 1}}, {}, {}, {}});
    for (int index = 0; index < 20; ++index)
        leafRegisters.blocks[0].instructions.push_back(
            {toyc::MOpcode::COPY, {toyc::PhysReg::A0},
             {toyc::vreg(index)}, {}, {}, {}});
    leafRegisters.blocks[0].instructions.push_back(
        {toyc::MOpcode::RET, {}, {}, {}, {}, {}});
    toyc::LinearScanRegisterAllocator().run(leafRegisters);
    check(leafRegisters.spillSlotCount == 0,
          "argument registers were not made available to an argument-free leaf");

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
