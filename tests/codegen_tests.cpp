#include "test_support.hpp"

int main() {
    TestPipeline pipeline("int sum(int a,int b,int c,int d,int e,int f,int g,int h,int i){return a+i;} int main(){return sum(1,2,3,4,5,6,7,8,9);}");
    const std::string assembly = pipeline.emitAssembly();
    check(assembly.find("call sum") != std::string::npos, "call was not emitted");
    check(assembly.find("sw") != std::string::npos && assembly.find("0(sp)") != std::string::npos,
          "ninth argument was not passed on stack");
    check(assembly.find(".Lmain_") != std::string::npos, "shared epilogue is missing");

    TestPipeline leaf("int main(){return 0;}");
    const std::string leafAssembly = leaf.emitAssembly();
    check(leafAssembly.find("sw ra") == std::string::npos, "leaf function saves ra");

    TestPipeline globals("int state=3;int main(){state=state+1;return state;}");
    const std::string globalAssembly = globals.emitAssembly();
    check(globalAssembly.find(".section .data") != std::string::npos &&
          globalAssembly.find("state") != std::string::npos,
          "global object access was not emitted");

    const auto functionBody = [](const std::string& assembly, const std::string& name) {
        const std::size_t begin = assembly.find("\n" + name + ":\n");
        const std::size_t end = assembly.find("  .size " + name, begin);
        check(begin != std::string::npos && end != std::string::npos,
              "function body is missing from assembly");
        return assembly.substr(begin, end - begin);
    };
    TestPipeline immediate(
        "int f(int x){int y=x+5;int z=y*8;return z-3;}int main(){return f(4);}", true);
    const std::string immediateAssembly = immediate.emitAssembly();
    const std::string immediateFunction = functionBody(immediateAssembly, "f");
    check(immediateFunction.find("addi ") != std::string::npos,
          "small constant arithmetic did not emit addi");
    check(immediateFunction.find("slli ") != std::string::npos &&
          immediateFunction.find("  mul ") == std::string::npos,
          "power-of-two multiply did not emit slli");
    check(immediateFunction.find("  li ") == std::string::npos,
          "absorbed arithmetic constant was still materialized");
    check(immediateFunction.find("j .Lf_1") == std::string::npos,
          "jump to adjacent epilogue was emitted in optimized mode");

    TestPipeline baselineFallthrough("int f(int x){return x+5;}int main(){return f(1);}");
    check(functionBody(baselineFallthrough.emitAssembly(), "f").find("j .Lf_1") != std::string::npos,
          "default mode no longer provides the non-fallthrough baseline");

    TestPipeline zeroCompare("int f(int x){return x==0;}int main(){return f(1);}", true);
    const std::string zeroAssembly = functionBody(zeroCompare.emitAssembly(), "f");
    check(zeroAssembly.find("sltiu ") != std::string::npos &&
          zeroAssembly.find("  li ") == std::string::npos,
          "materialized zero comparison did not use the zero-specialized form");

    TestPipeline branchContext("int main(){return 0;}");
    const auto checkInversion = [&](toyc::MOpcode opcode, const std::string& inverse) {
        auto module = branchContext.buildFinalMIR();
        auto& function = module.functions[0];
        function.blocks.resize(3);
        function.entry = 0;
        function.epilogue = 2;
        function.stage = toyc::MIRStage::AfterFrameLowering;
        function.blocks[0] = {0, {{opcode, {}, {toyc::PhysReg::T0, toyc::PhysReg::T1,
                                                toyc::MachineBlockRef{1}, toyc::MachineBlockRef{2}},
                                         {}, {}, {}}}, {}, {1, 2}};
        function.blocks[1] = {1, {{toyc::MOpcode::RET, {}, {}, {}, {}, {}}}, {0}, {}};
        function.blocks[2] = {2, {{toyc::MOpcode::RET, {}, {}, {}, {}, {}}}, {0}, {}};
        toyc::verifyMIR(function, toyc::MIRStage::AfterFrameLowering);
        std::ostringstream output;
        toyc::AsmPrinterOptions options;
        options.enableFallthrough = true;
        toyc::AsmPrinter(output, options).print(module, branchContext.semantic);
        check(output.str().find("  " + inverse + " t0, t1, .Lmain_2\n") !=
                  std::string::npos,
              "adjacent true target did not invert fused branch");
        check(output.str().find("  j .Lmain_2\n") == std::string::npos,
              "inverted fused branch still emitted a redundant jump");
    };
    checkInversion(toyc::MOpcode::BEQ, "bne");
    checkInversion(toyc::MOpcode::BNE, "beq");
    checkInversion(toyc::MOpcode::BLT, "bge");
    checkInversion(toyc::MOpcode::BGE, "blt");
}
