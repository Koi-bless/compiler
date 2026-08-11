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
}
