#include <sstream>

#include "test_support.hpp"
#include "toyc/backend/mir_printer.hpp"

int main() {
    TestPipeline pipeline("int f(int x){return x+1;}int main(){return f(4);}");
    bool foundCall = false;
    for (const auto& function : pipeline.machine.functions)
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions) {
                check(instruction.opcode != toyc::MOpcode::PARALLEL_COPY, "parallel copy remains in pre-RA MIR");
                if (instruction.opcode == toyc::MOpcode::CALL) {
                    foundCall = true;
                    check(instruction.implicitDefs.contains(toyc::PhysReg::Ra) &&
                          instruction.implicitDefs.contains(toyc::PhysReg::A0) &&
                          instruction.implicitDefs.contains(toyc::PhysReg::T6),
                          "call clobber mask is incomplete");
                }
            }
    check(foundCall, "MIR call was not selected");
    std::ostringstream output;
    toyc::printMIR(output, pipeline.machine, pipeline.semantic);
    check(output.str().find("stage=pre-ra") != std::string::npos && output.str().find("%v") != std::string::npos,
          "pre-RA MIR dump lacks virtual registers");

    auto broken = pipeline.machine;
    bool damaged = false;
    for (auto& function : broken.functions) for (auto& block : function.blocks)
        for (auto& instruction : block.instructions) if (instruction.opcode == toyc::MOpcode::CALL) {
            instruction.implicitDefs.erase(toyc::PhysReg::Ra); damaged = true;
        }
    check(damaged, "test MIR lacks call to damage");
    expectCompileError([&] { toyc::verifyMIR(broken, toyc::MIRStage::PreRegisterAllocation); }, "clobber mask");

    const auto shiftFunction = [](std::int32_t amount) {
        toyc::MachineFunction function;
        function.blocks.resize(1);
        function.blocks[0].id = 0;
        function.blocks[0].instructions = {
            {toyc::MOpcode::SLLI, {toyc::PhysReg::T0},
             {toyc::PhysReg::T1, toyc::Immediate{amount}}, {}, {}, {}},
            {toyc::MOpcode::RET, {}, {}, {}, {}, {}}
        };
        return function;
    };
    auto shift0 = shiftFunction(0);
    auto shift31 = shiftFunction(31);
    toyc::verifyMIR(shift0, toyc::MIRStage::PreRegisterAllocation);
    toyc::verifyMIR(shift31, toyc::MIRStage::PreRegisterAllocation);
    auto shiftNegative = shiftFunction(-1);
    auto shift32 = shiftFunction(32);
    expectCompileError([&] { toyc::verifyMIR(shiftNegative, toyc::MIRStage::PreRegisterAllocation); },
                       "shift amount");
    expectCompileError([&] { toyc::verifyMIR(shift32, toyc::MIRStage::PreRegisterAllocation); },
                       "shift amount");
}
