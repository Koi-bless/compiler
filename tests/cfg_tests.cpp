#include <sstream>

#include "test_support.hpp"
#include "toyc/ir/cfg.hpp"

int main() {
    TestPipeline pipeline("int side(){return 1;} int main(){int x=0;while(x<3){x=x+1;if(x==2)continue;if(x==3)break;}return x&&side()||x;}");
    std::ostringstream output;
    toyc::printCFG(output, pipeline.cfg, pipeline.semantic);
    check(output.str().find("branch") != std::string::npos, "CFG has no conditional branch");
    check(output.str().find("call @side") != std::string::npos, "short-circuit RHS call was not lowered");
    expectCompileError([] { TestPipeline bad("int main(){int x=0;if(x)return 1;}"); }, "reachable path without a return");
    TestPipeline constantReturn("int main(){if(1)return 1;}");
    check(!constantReturn.cfg.functions.empty(), "constant true return path was rejected");
    TestPipeline foldedReturn("int main(){if(1==1)return 1;}");
    check(!foldedReturn.cfg.functions.empty(), "constant expression return path was rejected");

    TestPipeline shortCircuitReturn(
        "int side(){return 0;}int main(){if((0&&side())||(1||side()))return 7;}");
    std::ostringstream shortCircuitOutput;
    toyc::printCFG(shortCircuitOutput, shortCircuitReturn.cfg,
                   shortCircuitReturn.semantic);
    check(shortCircuitOutput.str().find("call @side") == std::string::npos,
          "unreachable short-circuit RHS was lowered");

    TestPipeline globalInit("int seed=3;int next(){seed=seed+4;return seed;}int value=next();int main(){return value+seed;}");
    std::ostringstream globalInitOutput;
    toyc::printCFG(globalInitOutput, globalInit.cfg, globalInit.semantic);
    check(globalInitOutput.str().find("call @next") != std::string::npos &&
              globalInitOutput.str().find("store_global $1") != std::string::npos,
          "runtime global initializer was not lowered into main");
}
