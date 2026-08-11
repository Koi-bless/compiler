#include <sstream>

#include "test_support.hpp"
#include "toyc/ir/cfg.hpp"

int main() {
    TestPipeline pipeline("int side(){return 1;} int main(){int x=0;while(x<3){x=x+1;if(x==2)continue;if(x==3)break;}return 0&&side()||x;}");
    std::ostringstream output;
    toyc::printCfg(output, pipeline.module, pipeline.semantic);
    check(output.str().find("branch") != std::string::npos, "CFG has no conditional branch");
    check(output.str().find("call @side") != std::string::npos, "short-circuit RHS call was not lowered");
    expectCompileError([] { TestPipeline bad("int main(){int x=0;if(x)return 1;}"); }, "reachable path without a return");
    TestPipeline constantReturn("int main(){if(1)return 1;}");
    check(!constantReturn.module.functions.empty(), "constant true return path was rejected");
    TestPipeline foldedReturn("int main(){if(1==1)return 1;}");
    check(!foldedReturn.module.functions.empty(), "constant expression return path was rejected");
}
