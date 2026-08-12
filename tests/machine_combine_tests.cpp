#include <algorithm>

#include "test_support.hpp"
#include "toyc/backend/machine_combine.hpp"

namespace {

std::size_t countOpcode(const toyc::MachineFunction& function, toyc::MOpcode opcode) {
    std::size_t count = 0;
    for (const auto& block : function.blocks)
        count += static_cast<std::size_t>(std::count_if(
            block.instructions.begin(), block.instructions.end(),
            [&](const auto& instruction) { return instruction.opcode == opcode; }));
    return count;
}

toyc::MachineFunction arithmeticFunction(toyc::MOpcode opcode, std::int32_t constant) {
    toyc::MachineFunction function;
    function.vregCount = 3;
    function.blocks.resize(1);
    function.blocks[0].id = 0;
    function.blocks[0].instructions = {
        {toyc::MOpcode::COPY, {toyc::vreg(0)}, {toyc::PhysReg::A0}, {}, {}, {}},
        {toyc::MOpcode::LI, {toyc::vreg(1)}, {toyc::Immediate{constant}}, {}, {}, {}},
        {opcode, {toyc::vreg(2)}, {toyc::vreg(0), toyc::vreg(1)}, {}, {}, {}},
        {toyc::MOpcode::COPY, {toyc::PhysReg::A0}, {toyc::vreg(2)}, {}, {}, {}},
        {toyc::MOpcode::RET, {}, {}, {}, {}, {}}
    };
    return function;
}

} // namespace

int main() {
    auto add = arithmeticFunction(toyc::MOpcode::ADD, 5);
    const auto addResult = toyc::runPreRAMachineCombine(add);
    toyc::verifyMIR(add, toyc::MIRStage::PreRegisterAllocation);
    check(addResult.changed && countOpcode(add, toyc::MOpcode::ADDI) == 1,
          "small constant ADD was not combined");
    check(countOpcode(add, toyc::MOpcode::LI) == 0, "absorbed ADD constant remains live");
    check(!toyc::runPreRAMachineCombine(add).changed, "machine combine is not idempotent");

    auto subBoundary = arithmeticFunction(toyc::MOpcode::SUB, 2048);
    toyc::runPreRAMachineCombine(subBoundary);
    check(countOpcode(subBoundary, toyc::MOpcode::ADDI) == 1,
          "x-2048 did not select ADDI -2048");
    auto subTooLarge = arithmeticFunction(toyc::MOpcode::SUB, -2048);
    toyc::runPreRAMachineCombine(subTooLarge);
    check(countOpcode(subTooLarge, toyc::MOpcode::SUB) == 1,
          "x-(-2048) selected an illegal immediate");

    auto multiply = arithmeticFunction(toyc::MOpcode::MUL,
                                       static_cast<std::int32_t>(0x80000000u));
    toyc::runPreRAMachineCombine(multiply);
    check(countOpcode(multiply, toyc::MOpcode::SLLI) == 1 &&
          countOpcode(multiply, toyc::MOpcode::MUL) == 0,
          "INT_MIN multiplier was not reduced to SLLI");
    const auto& shift = multiply.blocks[0].instructions[1];
    check(std::get<toyc::Immediate>(shift.uses[1]).value == 31,
          "INT_MIN multiplier selected the wrong shift");

    auto nonPower = arithmeticFunction(toyc::MOpcode::MUL, 5);
    toyc::runPreRAMachineCombine(nonPower);
    check(countOpcode(nonPower, toyc::MOpcode::MUL) == 1,
          "non-power-of-two multiply was expanded");

    auto conservative = arithmeticFunction(toyc::MOpcode::DIV, 3);
    toyc::runPreRAMachineCombine(conservative);
    check(countOpcode(conservative, toyc::MOpcode::DIV) == 1,
          "potentially trapping DIV was removed");

    toyc::MachineFunction physicalCopy;
    physicalCopy.vregCount = 1;
    physicalCopy.blocks.resize(1);
    physicalCopy.blocks[0].id = 0;
    physicalCopy.blocks[0].instructions = {
        {toyc::MOpcode::COPY, {toyc::vreg(0)}, {toyc::PhysReg::A0}, {}, {}, {}},
        {toyc::MOpcode::COPY, {toyc::PhysReg::A1}, {toyc::vreg(0)}, {}, {}, {}},
        {toyc::MOpcode::RET, {}, {}, {}, {}, {}}
    };
    toyc::runPreRAMachineCombine(physicalCopy);
    check(countOpcode(physicalCopy, toyc::MOpcode::COPY) == 2,
          "physical-to-virtual copy was propagated globally");

    auto wrongStage = add;
    wrongStage.stage = toyc::MIRStage::PostRegisterAllocation;
    expectCompileError([&] { toyc::runPreRAMachineCombine(wrongStage); }, "requires pre-RA");
}
