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
}
