#include <sstream>

#include "test_support.hpp"
#include "toyc/backend/asm_printer.hpp"

int main() {
    TestPipeline pipeline("int sum(int a,int b,int c,int d,int e,int f,int g,int h,int i){return a+i;} int main(){return sum(1,2,3,4,5,6,7,8,9);}");
    std::ostringstream output;
    toyc::AsmPrinter(output).print(pipeline.module, pipeline.semantic);
    const std::string assembly = output.str();
    check(assembly.find("call sum") != std::string::npos, "call was not emitted");
    check(assembly.find("sw t0, 0(sp)") != std::string::npos, "ninth argument was not passed on stack");
    check(assembly.find(".Lmain_epilogue") != std::string::npos, "shared epilogue is missing");

    std::string source = "int main(){";
    for (int index = 0; index < 300; ++index)
        source += "int v" + std::to_string(index) + "=" + std::to_string(index) + ";";
    source += "return v299;}";
    TestPipeline large(source);
    std::ostringstream largeOutput;
    toyc::AsmPrinter(largeOutput).print(large.module, large.semantic);
    check(largeOutput.str().find("add sp, sp, t0") != std::string::npos,
          "large frame adjustment was not materialized");
    check(largeOutput.str().find("add t2, sp, t2") != std::string::npos,
          "large stack offset was not materialized");

    TestPipeline globals("int state=3;int main(){state=state+1;return state;}");
    std::ostringstream globalOutput;
    toyc::AsmPrinter(globalOutput).print(globals.module, globals.semantic);
    check(globalOutput.str().find(".section .data") != std::string::npos &&
          globalOutput.str().find("la t1, state") != std::string::npos,
          "global object access was not emitted");
}
