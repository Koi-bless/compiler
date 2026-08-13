#include <algorithm>

#include "test_support.hpp"

int main() {
    TestPipeline leaf("int main(){return 7;}");
    const auto leafAssembly = leaf.emitAssembly();
    check(leafAssembly.find("sw ra") == std::string::npos, "leaf function saves return address");

    TestPipeline nonLeaf("int f(){return 2;}int main(){return f();}");
    const auto assembly = nonLeaf.emitAssembly();
    check(assembly.find("sw ra") != std::string::npos && assembly.find("lw ra") != std::string::npos,
          "non-leaf return address is not preserved");

    TestPipeline arguments("int f(int a,int b,int c,int d,int e,int f,int g,int h,int i){return a+i;}int main(){return f(1,2,3,4,5,6,7,8,9);}");
    const auto argumentAssembly = arguments.emitAssembly();
    check(argumentAssembly.find("call f") != std::string::npos && argumentAssembly.find("0(sp)") != std::string::npos,
          "stack argument ABI was not emitted");

    toyc::MachineFunction largeLoads;
    largeLoads.blocks.resize(2);
    largeLoads.entry = 0;
    largeLoads.epilogue = 1;
    largeLoads.spillSlotCount = 514;
    largeLoads.stage = toyc::MIRStage::PostRegisterAllocation;
    largeLoads.blocks[0].id = 0;
    largeLoads.blocks[0].successors = {1};
    largeLoads.blocks[0].instructions = {
        {toyc::MOpcode::LW, {toyc::PhysReg::T5},
         {toyc::StackSlot{toyc::StackSlotKind::Spill, 512}}, {}, {}, {}},
        {toyc::MOpcode::LW, {toyc::PhysReg::T6},
         {toyc::StackSlot{toyc::StackSlotKind::Spill, 513}}, {}, {}, {}},
        {toyc::MOpcode::ADD, {toyc::PhysReg::A0},
         {toyc::PhysReg::T5, toyc::PhysReg::T6}, {}, {}, {}},
        {toyc::MOpcode::JUMP, {}, {toyc::MachineBlockRef{1}}, {}, {}, {}}
    };
    largeLoads.blocks[1].id = 1;
    largeLoads.blocks[1].predecessors = {0};
    largeLoads.blocks[1].instructions = {
        {toyc::MOpcode::RET, {}, {}, {}, {}, {}}
    };
    toyc::FrameLowering().run(largeLoads);
    const auto& lowered = largeLoads.blocks[0].instructions;
    const auto selfAddressed = [&](toyc::PhysReg destination) {
        return std::any_of(lowered.begin(), lowered.end(), [&](const auto& instruction) {
            return instruction.opcode == toyc::MOpcode::LW &&
                   instruction.defs == std::vector<toyc::MOperand>{destination} &&
                   instruction.uses.size() == 2 &&
                   instruction.uses[0] == toyc::MOperand{destination};
        });
    };
    check(selfAddressed(toyc::PhysReg::T5) && selfAddressed(toyc::PhysReg::T6),
          "large stack loads clobber the other spill scratch register");
}
