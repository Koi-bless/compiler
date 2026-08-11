#include <sstream>

#include "test_support.hpp"
#include "toyc/ir/ir_printer.hpp"

int main() {
    TestPipeline diamond("int main(){int x=0;if(x)x=1;else x=2;return x;}");
    std::ostringstream first;
    toyc::printIR(first, diamond.ir, diamond.semantic);
    check(first.str().find(" = phi ") != std::string::npos, "diamond lacks SSA phi");
    std::ostringstream second;
    toyc::printIR(second, diamond.ir, diamond.semantic);
    check(first.str() == second.str(), "SSA dump is not deterministic");

    TestPipeline loop("int main(){int x=0;while(x<4)x=x+1;return x;}");
    std::ostringstream loopDump;
    toyc::printIR(loopDump, loop.ir, loop.semantic);
    check(loopDump.str().find("phi [bb0:") != std::string::npos, "loop header lacks entry/backedge phi");

    TestPipeline globals("int g=1;int main(){g=g+1;return g;}");
    std::ostringstream globalDump;
    toyc::printIR(globalDump, globals.ir, globals.semantic);
    check(globalDump.str().find("load_global @g") != std::string::npos &&
          globalDump.str().find("store_global @g") != std::string::npos,
          "global memory was incorrectly promoted");

    auto duplicate = diamond.ir;
    auto& instructions = duplicate.functions[0].blocks[0].instructions;
    check(instructions.size() >= 2, "test IR lacks two definitions");
    instructions[1].result = instructions[0].result;
    expectCompileError([&] { toyc::verifyIR(duplicate, diamond.semantic); }, "multiple definitions");

    auto brokenPhi = diamond.ir;
    bool damaged = false;
    for (auto& block : brokenPhi.functions[0].blocks) for (auto& instruction : block.instructions)
        if (instruction.op == toyc::IROp::Phi) {
            instruction.phiInputs.pop_back(); damaged = true; break;
        }
    check(damaged, "test IR lacks phi to damage");
    expectCompileError([&] { toyc::verifyIR(brokenPhi, diamond.semantic); }, "phi input count");
}
